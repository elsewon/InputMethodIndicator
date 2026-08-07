// App.cpp — 최상위 조율. 입력 상태는 입력 표시기에서 온다
// (TrayIndicatorWatcher, UI Automation). 포어그라운드 WinEvent 훅이 캐럿 배치와
// 앱 전환 유예 구간을 위해 포커스된 창을 추적한다.
// 상태 머신과 오버레이는 하나의 UI 스레드에서 돌고, 프로세스 경계를 넘는 UIA
// 작업은 세 작업자 스레드(트레이 감시자, 캐럿 해석기, 포커스 감시자)가 맡는다.
// OS 콜백(LL hook, UIA 이벤트)과 작업자는 PostMessage만 하므로, 무거운 경로
// (상태 머신 -> 캐럿 -> 오버레이)는 일반 메시지 루프에서 실행된다. UI 스레드는
// LL 키보드 훅을 소유하므로 결코 블로킹해서는 안 된다 — 여기서의 블로킹은
// 시스템 전역 입력 지연이 된다.
#include "App.h"

#include <windows.h>
#include <objbase.h>          // CoInitializeEx / CoUninitialize
#include <cstdio>             // _snwprintf_s, _TRUNCATE
#include <cwchar>             // wcsrchr
#include <tlhelp32.h>         // CreateToolhelp32Snapshot (explorer 재시작 + 디버그 인스턴스 탈취)

#include "Constants.h"
#include "InputState.h"
#include "DpiUtil.h"
#include "Log.h"
#include "Version.h"
#include "resource.h"

namespace imi {

namespace {

// 마지막으로 해석된 캐럿. 오직 DiagStatus를 채우기 위해 보관한다.
CaretResult g_lastCaret;

// 우리가 호출한 CoInitializeEx를 CoUninitialize와 짝지어야 하는지 여부
// (S_OK/S_FALSE를 반환했을 때만. RPC_E_CHANGED_MODE에서는 절대 아니다).
bool g_comInitialized = false;

// 정적 WinEvent 썽크에서 접근할 수 있는 단일 App 인스턴스
// (단일 인스턴스로 보호된다).
App* g_app = nullptr;

constexpr uint32_t kIndicatorDebounceMs = 60;
constexpr uint32_t kForegroundGraceMs   = 400;

// Caps 표시기: 폴링 주기, 그리고 사용자가 입력을 멈춘 뒤 캡슐이 다시 나타나기까지의
// 유휴 시간.
constexpr UINT     kCapsIdlePollMs   = 100;
constexpr uint64_t kCapsReshowIdleMs = 1000;

// 캡슐을 표시하기 전에 포커스 변경 이벤트 폭주가 잦아들도록 기다리는 시간.
constexpr UINT kFocusSettleMs = 160;

// 포커스 표시기가 켜져 있는 동안 렌더러 접근성을 깨우는 주기.
constexpr UINT kAccessibilityPokeMs = 4000;

// Chromium은 각 렌더러의 접근성을 지연(LAZY) 활성화한다. 그 렌더러에 처음
// 도달하는 WM_GETOBJECT(OBJID_CLIENT)를 받아야 켜지며, 그전까지 탭의 웹 콘텐츠는
// 포커스 이벤트를 전혀 발생시키지 않는다. 따라서 다른 무언가가 우연히 질의하기
// 전까지, 갓 연 페이지에서는 포커스 표시기가 잠자코 있게 된다. 그 메시지를
// 우리가 직접 보낸다. 포어그라운드 창 아래의 모든 렌더러 hwnd에 보내며(탭마다
// 하나씩 중첩되어 있고, 페이지 이동 시 교체된다), 포어그라운드가 바뀔 때 한 번,
// 그리고 느린 타이머로 다시 보낸다 — 창이 포어그라운드인 채로 페이지를 열거나
// 이동하면 깨우지 않은 새 렌더러가 생기기 때문이다.
//
// SendNotifyMessage를 쓰는 이유: 접근성 활성화는 렌더러가 WM_GETOBJECT를
// **받아 처리하는** 순간 일어나고, 우리는 답(접근성 객체)을 쓰지 않는다.
// AccessibleObjectFromWindow는 답을 받으려고 동기로 기다리므로 바쁜 렌더러가
// UI 스레드(와 LL 키보드 훅)를 얼릴 수 있다 — 기다릴 이유가 없으니 전달만
// 하고 즉시 돌아온다. (렌더러가 만든 마샬링 참조는 소비되지 않으면 COM이
// 시간 초과로 회수한다.)
BOOL CALLBACK PokeRendererThunk(HWND hwnd, LPARAM lParam) {
    wchar_t cls[64];
    if (GetClassNameW(hwnd, cls, 64) != 0 &&
        wcscmp(cls, L"Chrome_RenderWidgetHostHWND") == 0) {
        SendNotifyMessageW(hwnd, WM_GETOBJECT, 0, static_cast<LPARAM>(OBJID_CLIENT));
        int* poked = reinterpret_cast<int*>(lParam);
        if (++(*poked) >= 8) return FALSE;   // 안전 상한
    }
    return TRUE;
}

void PokeRendererAccessibility(HWND foreground) {
    if (foreground == nullptr) return;
    int poked = 0;
    EnumChildWindows(foreground, &PokeRendererThunk,
                     reinterpret_cast<LPARAM>(&poked));
}

// 캐럿 해석 예산. 의도적으로 넉넉하게 잡았다. 예열된 UIA 공급자는 약 15 ms 만에
// 답하지만, 갓 시작한 앱을 상대로 하는 첫 번째 해석은 그 공급자를 깨워야 해서
// 약 440 ms로 측정되었다. 이 값을 짧게 잡으면 단순히 캡슐이 늦어지는 게 아니라,
// 만료되면 캡슐을 아예 버리므로 표시기가 나타나지 않게 된다.
constexpr DWORD kCaretTimeoutMs = 500;

// Windows 탐색기(셸)를 다시 시작한다. 현재 세션의 explorer.exe를 종료하면
// Windows가 AutoRestartShell(기본값)로 셸을 자동 재시작한다. 직접 다시 띄우지
// 않는 이유: UI 스레드에서 재시작을 기다리며 폴링하면 LL 키보드 훅이 그동안 멈춰
// 시스템 전역 입력이 지연되고, 성급히 explorer.exe를 실행하면 자동 재시작과 겹쳐
// 여분의 파일 탐색기 창이 열린다. 우리 트레이 아이콘은 새 셸이 브로드캐스트하는
// TaskbarCreated에서 다시 등록된다. 다른 세션의 셸은 건드리지 않는다.
void RestartExplorer() {
    DWORD mySession = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &mySession);

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        IMI_WARN(L"App: RestartExplorer 스냅샷 실패 gle=%lu", GetLastError());
        return;
    }
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    int killed = 0;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (CompareStringOrdinal(pe.szExeFile, -1, L"explorer.exe", -1, TRUE) != CSTR_EQUAL) {
                continue;
            }
            DWORD sess = 0;
            if (!ProcessIdToSessionId(pe.th32ProcessID, &sess) || sess != mySession) {
                continue;   // 다른 세션의 셸은 건드리지 않는다
            }
            HANDLE proc = OpenProcess(PROCESS_TERMINATE, FALSE, pe.th32ProcessID);
            if (proc != nullptr) {
                if (TerminateProcess(proc, 0)) ++killed;
                CloseHandle(proc);
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    IMI_INFO(L"App: Windows 탐색기를 종료했습니다(%d개). 셸이 자동 재시작됩니다.", killed);
}

#ifdef _DEBUG
// 단일 인스턴스 선택 규칙: DEBUG 빌드는 경합에서 항상 이긴다. 실행 중인 모든
// 인스턴스(더 오래된 디버그 빌드든 릴리스든)를 종료시키고 — 트레이 아이콘이
// 깔끔히 제거되도록 먼저 정상 종료를 시도하고, 최후의 수단으로 강제 종료한다 —
// 자리를 넘겨받는다. 릴리스는 반대 규칙을 유지한다. 실행 중인 인스턴스가 이기고,
// 새로 실행된 쪽이 kMsg_SecondInstance를 보낸 뒤 종료한다.

// 다른 모든 인스턴스에 종료를 요청한다 (각자의 트레이 창이 DefWindowProc ->
// DestroyWindow -> PostQuitMessage -> 정상 Shutdown() 경로로 WM_CLOSE를 처리한다).
void CloseOtherInstanceWindows() {
    HWND w = nullptr;
    while ((w = FindWindowExW(nullptr, w, kTrayWindowClass, nullptr)) != nullptr) {
        PostMessageW(w, WM_CLOSE, 0, 0);
    }
}

// 응답 없는 인스턴스에 대한 최후의 수단: 우리를 제외한 같은 이름의 프로세스를
// 모두 종료한다.
void ForceTerminateOtherInstances() {
    wchar_t self[MAX_PATH];
    if (GetModuleFileNameW(nullptr, self, ARRAYSIZE(self)) == 0) return;
    const wchar_t* slash = wcsrchr(self, L'\\');
    const wchar_t* base = (slash != nullptr) ? slash + 1 : self;
    const DWORD myPid = GetCurrentProcessId();

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            if (pe.th32ProcessID != myPid &&
                CompareStringOrdinal(pe.szExeFile, -1, base, -1, TRUE) == CSTR_EQUAL) {
                HANDLE proc = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE,
                                          pe.th32ProcessID);
                if (proc != nullptr) {
                    IMI_WARN(L"App: 멈춘 인스턴스를 강제 종료합니다 pid=%lu.",
                             pe.th32ProcessID);
                    TerminateProcess(proc, 0);
                    WaitForSingleObject(proc, 2000);
                    CloseHandle(proc);
                }
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
}
#endif // _DEBUG

} // namespace

// ---------------------------------------------------------------------------
// 수명 주기
// ---------------------------------------------------------------------------

int App::Run(HINSTANCE hinst, LPWSTR cmdLine, int nCmdShow) {
    (void)cmdLine;
    (void)nCmdShow;
    hinst_ = hinst;

    if (!AcquireSingleInstance()) {
        IMI_INFO(L"App: 다른 인스턴스가 이미 실행 중입니다. 종료합니다.");
        return 0;
    }

    EnsurePerMonitorV2Awareness();

    config_ = Config::Load();
    diag_.Init(hinst_, config_.logLevel, config_.logToFile);
    IMI_INFO(L"App: InputMethodIndicator %s 시작합니다.", IMI_VERSION_STRING_W);

    // UI 스레드는 STA다(UIA 클라이언트 + 셸). CaretResolver는 자체 MTA 작업자
    // 스레드를 소유한다.
    const HRESULT hrCo = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    g_comInitialized = SUCCEEDED(hrCo);
    if (!g_comInitialized && hrCo != RPC_E_CHANGED_MODE) {
        IMI_ERROR(L"App: CoInitializeEx 실패 hr=0x%08lX", static_cast<unsigned long>(hrCo));
    }

    if (!CreateHostWindow()) {
        IMI_ERROR(L"App: 호스트 창 생성에 실패했습니다 (le=%lu).", GetLastError());
        Shutdown();
        return 1;
    }
    if (!StartSubsystems()) {
        IMI_ERROR(L"App: 서브시스템 시작에 실패했습니다.");
        Shutdown();
        return 1;
    }

    StartWatchdog();   // 트레이 창이 준비된 뒤, 메시지 루프 직전에 시작한다

    IMI_INFO(L"App: 메시지 루프에 진입합니다.");
    MSG msg{};   // 0으로 초기화해 GetMessageW 오류 경로에서도 정의된 코드를 반환한다
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    Shutdown();
    return static_cast<int>(msg.wParam);
}

bool App::AcquireSingleInstance() {
    singleInstance_ = CreateMutexW(nullptr, TRUE, kSingleInstanceMutex);
    DWORD err = GetLastError();
    if (singleInstance_ == nullptr) {
        IMI_WARN(L"App: CreateMutexW 실패 le=%lu. 보호 없이 실행합니다.", err);
        return true;
    }
    if (err != ERROR_ALREADY_EXISTS) {
        return true;
    }

#ifdef _DEBUG
    // 디버그가 이긴다(위의 도우미 함수 참고). 실행 중인 인스턴스에 양보하지 않고
    // 대체하므로, 새로 실행한 디버그 빌드가 항상 자리를 넘겨받는다.
    CloseHandle(singleInstance_);
    singleInstance_ = nullptr;
    IMI_INFO(L"App: 디버그 빌드가 실행 중인 인스턴스를 넘겨받습니다.");

    CloseOtherInstanceWindows();
    for (int attempt = 0; attempt < 20; ++attempt) {   // 정상 종료를 최대 2초까지 기다린다
        Sleep(100);
        singleInstance_ = CreateMutexW(nullptr, TRUE, kSingleInstanceMutex);
        err = GetLastError();
        if (singleInstance_ == nullptr) return true;   // 위와 같이 보호 없이 실행
        if (err != ERROR_ALREADY_EXISTS) return true;
        CloseHandle(singleInstance_);
        singleInstance_ = nullptr;
    }

    ForceTerminateOtherInstances();   // 프로세스가 죽으면 명명된 뮤텍스가 해제된다
    singleInstance_ = CreateMutexW(nullptr, TRUE, kSingleInstanceMutex);
    err = GetLastError();
    if (singleInstance_ == nullptr) return true;
    if (err != ERROR_ALREADY_EXISTS) return true;

    // 여전히 점유 중이다(예: 우리가 건드릴 수 없는 다른 세션의 인스턴스). 양보한다.
    CloseHandle(singleInstance_);
    singleInstance_ = nullptr;
    IMI_WARN(L"App: 실행 중인 인스턴스를 넘겨받지 못했습니다. 종료합니다.");
    return false;
#else
    CloseHandle(singleInstance_);
    singleInstance_ = nullptr;
    // 릴리스 규칙: 실행 중인 인스턴스가 이긴다. 풍선 알림을 띄울 수 있도록 그쪽에
    // 알린다 — 그러지 않고 조용히 종료하면 "앱이 실행되지 않는다"로 읽힌다.
    HWND existing = FindWindowW(kTrayWindowClass, nullptr);
    if (existing != nullptr) {
        PostMessageW(existing, kMsg_SecondInstance, 0, 0);
    }
    return false;
#endif
}

bool App::CreateHostWindow() {
    WNDCLASSEXW tc = {};
    tc.cbSize        = sizeof(tc);
    tc.lpfnWndProc   = &App::TrayWndProc;
    tc.hInstance     = hinst_;
    tc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    tc.lpszClassName = kTrayWindowClass;
    if (RegisterClassExW(&tc) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        IMI_ERROR(L"App: RegisterClassExW(tray) 실패 le=%lu.", GetLastError());
        return false;
    }
    trayWnd_ = CreateWindowExW(0, kTrayWindowClass, IMI_PRODUCT_NAME_W,
                               WS_OVERLAPPED, CW_USEDEFAULT, 0, 0, 0,
                               nullptr, nullptr, hinst_, this);
    if (trayWnd_ == nullptr) {
        IMI_ERROR(L"App: CreateWindowExW(tray) 실패 le=%lu.", GetLastError());
        return false;
    }
    return true;
}

bool App::StartSubsystems() {
    if (!overlay_.Create(hinst_)) {
        IMI_ERROR(L"App: OverlayWindow::Create 실패.");
        return false;
    }
    if (!tray_.Create(trayWnd_, hinst_, kTrayCallbackMessage, kTrayIconId)) {
        IMI_WARN(L"App: TrayIcon::Create 실패. 트레이 아이콘 없이 계속 진행합니다.");
    }
    // explorer가 재시작하면 셸이 트레이 등록을 잃는다. 새 셸은 이 메시지를 모든
    // 최상위 창에 브로드캐스트하므로, 이를 받아 아이콘을 다시 등록한다
    // (HandleTrayMessage 참고). 우리 창은 top-level이라 브로드캐스트를 받는다.
    taskbarCreatedMsg_ = RegisterWindowMessageW(L"TaskbarCreated");

    if (!caret_.Start(trayWnd_, kMsg_CaretResolved)) {
        IMI_WARN(L"App: CaretResolver::Start 실패. UIA 캐럿 단계가 비활성화됩니다.");
    }

    // 상태 머신을 연결한다. emit을 연결하기 전에 caps를 미리 넣어, 시작 시 초기
    // 상태 때문에 캡슐이 번쩍이지 않게 한다. 언어 기준선은 의도적으로 설정하지 않은
    // 채 둔다. 그래야 감시자의 첫 보고가 — 표시기를 늦게 찾으면 몇 초 뒤에 올 수도
    // 있다 — 캡슐을 번쩍이게 할 Unknown->한국어 "변경"으로 읽히지 않고 조용히
    // 기준선을 잡는다.
    sm_.SetGates(config_.notifyLanguage, config_.notifyCapsLock);
    sm_.SetDebounceMs(kIndicatorDebounceMs);
    sm_.SetForegroundGraceMs(kForegroundGraceMs);
    sm_.OnCapsLock(CapsLockMonitor::CurrentState());   // emit_이 아직 null => 오버레이 없음
    sm_.SetEmit([this](const IndicatorDecision& d) { OnIndicator(d); });

    if (!caps_.Start(hinst_, [this](bool capsOn) {
            PostMessageW(trayWnd_, kMsg_CapsChanged, capsOn ? 1u : 0u, 0);
        })) {
        IMI_WARN(L"App: CapsLockMonitor::Start 실패. Caps Lock 표시기가 비활성화됩니다.");
    }

    // 포어그라운드 추적 (캐럿 배치와 앱 전환 유예 구간용).
    g_app = this;
    foregroundHook_ = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
                                      nullptr, &App::ForegroundEventThunk, 0, 0,
                                      WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    OnForeground(GetForegroundWindow());   // lastForeground_ 초기값을 채운다

    // 트레이 감시자: UI 스레드로 kMsg_TrayLangChanged(언어)와
    // kMsg_TrayWatcherHealth(이상/회복 전이)를 보낸다(최초 읽기 포함).
    if (!watcher_.Start(trayWnd_, kMsg_TrayLangChanged, kMsg_TrayWatcherHealth)) {
        IMI_WARN(L"App: TrayIndicatorWatcher::Start 실패. 언어 표시기가 비활성화됩니다.");
    }

    ApplyFocusWatcher();

    PushDiagStatus();
    return true;
}

void App::Shutdown() {
    // LL 키보드 훅을 가장 먼저 내린다. 아래의 작업자 Stop들은 각각 최대 2초씩
    // join을 기다릴 수 있는데 그동안 이 스레드는 메시지를 펌핑하지 않으므로,
    // 훅이 아직 걸려 있으면 그 시간만큼 시스템 전역 입력이 지연된다.
    caps_.Stop();
    if (foregroundHook_ != nullptr) { UnhookWinEvent(foregroundHook_); foregroundHook_ = nullptr; }
    g_app = nullptr;

    // 감시견은 트레이 창으로 프로브를 보내므로 창을 파괴하기 전에 멈춘다.
    StopWatchdog();

    focusWatcher_.Stop();
    watcher_.Stop();
    caret_.Stop();
    overlay_.Destroy();
    tray_.Destroy();

    if (trayWnd_ != nullptr) { DestroyWindow(trayWnd_); trayWnd_ = nullptr; }

    diag_.Shutdown();

    if (g_comInitialized) { CoUninitialize(); g_comInitialized = false; }
    if (singleInstance_ != nullptr) {
        ReleaseMutex(singleInstance_);
        CloseHandle(singleInstance_);
        singleInstance_ = nullptr;
    }
}

// ---------------------------------------------------------------------------
// UI 응답성 감시견 (App.h 참고)
// ---------------------------------------------------------------------------

void App::StartWatchdog() {
    watchdogStop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);   // 수동 리셋
    if (watchdogStop_ == nullptr) {
        IMI_WARN(L"App: 감시견 정지 이벤트 생성 실패 gle=%lu. 감시견 없이 진행합니다.",
                 GetLastError());
        return;
    }
    watchdogThread_ = CreateThread(nullptr, 0, &App::WatchdogThunk, this, 0, nullptr);
    if (watchdogThread_ == nullptr) {
        IMI_WARN(L"App: 감시견 스레드 생성 실패 gle=%lu.", GetLastError());
        CloseHandle(watchdogStop_);
        watchdogStop_ = nullptr;
    }
}

void App::StopWatchdog() {
    if (watchdogStop_ != nullptr) {
        SetEvent(watchdogStop_);
    }
    if (watchdogThread_ != nullptr) {
        WaitForSingleObject(watchdogThread_, 2000);
        CloseHandle(watchdogThread_);
        watchdogThread_ = nullptr;
    }
    if (watchdogStop_ != nullptr) {
        CloseHandle(watchdogStop_);
        watchdogStop_ = nullptr;
    }
}

DWORD WINAPI App::WatchdogThunk(void* self) {
    static_cast<App*>(self)->WatchdogLoop();
    return 0;
}

void App::WatchdogLoop() {
    // 프로브 주기와 응답 대기 한도. UI 스레드가 이 한도 안에 프로브에 답하지 못하면
    // 펌프하지 못하는 것으로 본다(= LL 훅이 멈춰 입력이 지연되는 상태).
    constexpr DWORD kProbePeriodMs  = 500;
    constexpr DWORD kProbeTimeoutMs = 400;
    uint64_t lastOkTick = GetTickCount64();
    bool stalled = false;

    for (;;) {
        // 정지 신호를 프로브 주기만큼 기다린다. 신호가 오면 끝낸다.
        if (WaitForSingleObject(watchdogStop_, kProbePeriodMs) == WAIT_OBJECT_0) {
            return;
        }
        DWORD_PTR result = 0;
        const LRESULT ok = SendMessageTimeoutW(trayWnd_, kMsg_WatchdogPing, 0, 0,
                                               SMTO_BLOCK, kProbeTimeoutMs, &result);
        const uint64_t now = GetTickCount64();
        if (ok != 0) {
            if (stalled) {
                IMI_INFO(L"App: UI 스레드가 응답을 회복했습니다.");
                stalled = false;
            }
            lastOkTick = now;
        } else if (!stalled) {
            // 프로브 타임아웃. 마지막 정상 응답 이후 경과를 담아 한 번만 기록한다.
            ReportAnomaly(Anomaly::UiStall, L"UI 스레드 응답 없음 (>=%llums)",
                          static_cast<unsigned long long>(now - lastOkTick));
            stalled = true;
        }
    }
}

// ---------------------------------------------------------------------------
// 윈도우 프로시저
// ---------------------------------------------------------------------------

LRESULT CALLBACK App::TrayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_NCCREATE) {
        const CREATESTRUCTW* cs = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    }
    App* self = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self != nullptr) return self->HandleTrayMessage(hwnd, msg, wParam, lParam);
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT App::HandleTrayMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    // explorer 재시작 후 셸이 브로드캐스트하는 등록 메시지 — 트레이 아이콘을
    // 다시 등록한다(동적 값이라 switch에 넣을 수 없다).
    if (taskbarCreatedMsg_ != 0 && msg == taskbarCreatedMsg_) {
        tray_.Restore();
        return 0;
    }

    switch (msg) {
    case kMsg_TrayLangChanged:
        OnTrayLanguage(static_cast<Language>(static_cast<uint8_t>(wParam)),
                       lParam != 0);
        return 0;
    case kMsg_CapsChanged:
        OnCapsLock(wParam != 0);
        return 0;
    case kMsg_FocusChanged:
        OnFocusChanged();
        return 0;
    case kMsg_CaretResolved:
        OnCaretResolved(static_cast<uint64_t>(wParam));
        return 0;
    case kMsg_TrayWatcherHealth:
        OnTrayWatcherHealth(static_cast<TrayWatcherHealth>(static_cast<uint8_t>(wParam)));
        return 0;
    case kMsg_WatchdogPing:
        // 감시견의 응답성 프로브. 이 메시지가 디스패치된다는 것 자체가 UI 스레드가
        // 살아 펌프하고 있다는 증거다 — 즉시 답한다.
        return 0;
    case kMsg_SecondInstance:
        IMI_INFO(L"App: 두 번째 인스턴스가 실행되었습니다. 사용자에게 알립니다.");
        tray_.ShowBalloon(IMI_PRODUCT_NAME_W,
                          L"이미 실행 중입니다. 트레이 아이콘에서 설정할 수 있습니다.");
        return 0;
    case WM_TIMER:
        if (wParam == kTimer_CapsIdle) {
            OnCapsIdleTick();
            return 0;
        }
        if (wParam == kTimer_FocusShow) {
            ShowFocusPill();
            return 0;
        }
        if (wParam == kTimer_AccessibilityPoke) {
            PokeRendererAccessibility(GetForegroundWindow());
            return 0;
        }
        if (wParam == kTimer_CaretTimeout) {
            OnCaretTimeout();
            return 0;
        }
        break;
    default:
        break;
    }

    if (msg == kTrayCallbackMessage) {
        switch (tray_.OnCallback(wParam, lParam)) {
        case TrayAction::ContextMenu: {
            TrayMenuState st;
            st.notifyLanguage      = config_.notifyLanguage;
            st.notifyCapsLock      = config_.notifyCapsLock;
            st.notifyFocusChange   = config_.notifyFocusChange;
            st.startWithWindows    = config_.startWithWindows;
            st.explorerRestartHint = explorerRestartAdvised_;
            const UINT cmd = tray_.ShowContextMenu(st);
            if (cmd != 0) OnTrayCommand(cmd);
            break;
        }
        case TrayAction::NotificationClick:
            // 이상 알림을 클릭했을 때만 재시작을 제안한다(회복/두 번째 인스턴스
            // 알림 클릭은 무시). 이상 상태가 아니면 안내할 것이 없다.
            if (explorerRestartAdvised_) ConfirmAndRestartExplorer();
            break;
        case TrayAction::None:
            break;
        }
        return 0;
    }

    switch (msg) {
    case WM_COMMAND:
        OnTrayCommand(LOWORD(wParam));
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

// ---------------------------------------------------------------------------
// 포어그라운드 추적
// ---------------------------------------------------------------------------

void CALLBACK App::ForegroundEventThunk(HWINEVENTHOOK, DWORD event, HWND hwnd,
                                        LONG, LONG, DWORD, DWORD) {
    if (g_app != nullptr && event == EVENT_SYSTEM_FOREGROUND) {
        g_app->OnForeground(hwnd);
    }
}

// UIA 포커스 구독은 이 기능에서 비싼 쪽 절반이다(시스템 전역의 모든 포커스 변경마다
// 공급자를 구체화하게 만든다). 그래서 설정이 켜져 있는 동안에만 동작한다.
void App::ApplyFocusWatcher() {
    if (config_.notifyFocusChange) {
        if (!focusWatcher_.Running() && !focusWatcher_.Start(trayWnd_, kMsg_FocusChanged)) {
            IMI_WARN(L"App: FocusWatcher::Start 실패. 포커스 표시기가 비활성화됩니다.");
        }
        // 창이 포어그라운드가 된 뒤에 생성된 렌더러(새 탭, 페이지 이동)는 접근성이
        // 꺼진 채 시작되어 포커스 이벤트를 발생시키지 않는다. 느린 주기로 다시
        // 깨워서 몇 초 안에 활성화되도록 한다.
        PokeRendererAccessibility(GetForegroundWindow());
        SetTimer(trayWnd_, kTimer_AccessibilityPoke, kAccessibilityPokeMs, nullptr);
    } else {
        focusWatcher_.Stop();
        KillTimer(trayWnd_, kTimer_AccessibilityPoke);
    }
}

// 포커스 변경은 여러 이벤트가 몰아쳐서 도착한다 — 이전 컨트롤이 포커스를 잃고,
// 새 컨트롤이 가져가며, 앱 전환이라면 EVENT_SYSTEM_FOREGROUND도 함께 발생한다.
// 즉시 처리하는 대신 짧은 일회성 타이머를 다시 건다. 그러면 이벤트 폭주가 한 번의
// 표시로 합쳐지고, 이벤트가 어떤 순서로 오든 오버레이를 숨기는 포어그라운드
// 처리기가 먼저 실행된다.
void App::OnFocusChanged() {
    if (!config_.notifyFocusChange) {
        return;
    }
    IMI_DEBUG(L"App: 포커스 이벤트 -> 안정화 타이머");
    SetTimer(trayWnd_, kTimer_FocusShow, kFocusSettleMs, nullptr);
}

void App::ShowFocusPill() {
    IMI_DEBUG(L"App: 포커스 안정화 타이머 발생");
    if (!config_.notifyFocusChange) {
        KillTimer(trayWnd_, kTimer_FocusShow);
        return;
    }
    // 포어그라운드를 다시 읽는다. 이벤트 폭주가 시작될 때 기록한 창과 다른 창에
    // 포커스가 안착했을 수 있다. OnForeground는 폭주 처리 로직의 일부로 안정화
    // 타이머를 다시 걸므로, 호출한 뒤에 타이머를 죽여야 한다. 그러지 않으면 이
    // 처리기가 스스로를 영원히 재발동시킨다.
    OnForeground(GetForegroundWindow());
    KillTimer(trayWnd_, kTimer_FocusShow);

    IndicatorDecision d;
    d.kind  = IndicatorKind::LanguageChanged;
    d.state = sm_.Current();
    ShowOverlayFor(d, /*sticky*/ false, /*requireCaret*/ true);
    PushDiagStatus();
}

void App::OnForeground(HWND fg) {
    if (fg == nullptr) fg = GetForegroundWindow();
    DWORD pid = 0;
    const DWORD tid = (fg != nullptr) ? GetWindowThreadProcessId(fg, &pid) : 0;

    lastForeground_    = fg;
    lastForegroundTid_ = tid;
    lastForegroundPid_ = pid;
    lastForegroundExe_[0] = L'\0';
    if (pid != 0) {
        HANDLE hp = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (hp != nullptr) {
            wchar_t path[MAX_PATH];
            DWORD n = ARRAYSIZE(path);
            if (QueryFullProcessImageNameW(hp, 0, path, &n)) {
                const wchar_t* base = wcsrchr(path, L'\\');
                lstrcpynW(lastForegroundExe_, (base != nullptr) ? base + 1 : path,
                          static_cast<int>(ARRAYSIZE(lastForegroundExe_)));
            }
            CloseHandle(hp);
        }
    }

    IMI_DEBUG(L"App: 포어그라운드 -> %s", lastForegroundExe_);
    sm_.OnForegroundChanged();   // 유예 구간을 연다(앱 전환 동기화를 억제)
    overlay_.Hide();             // 이전 창에서 남은 낡은 캡슐을 없앤다
    CancelPendingCaret();        // ...그리고 그 캡슐을 위해 오고 있던 답도 버린다
    // 관리 중이던 caps 캡슐도 방금 나머지와 함께 숨겨졌다. 유휴 폴링이 새 창의 캐럿
    // 근처로 다시 자리를 잡게 한다.
    if (capsActive_) capsPillShown_ = false;

    // 결과를 버릴 해석을 한 번 돌려 새 앱의 UIA 공급자를 예열한다. 갓 시작한 앱을
    // 상대로 하는 첫 질의는 그 앱의 접근성 트리를 구체화해야 한다(Edge에서 약
    // 440 ms로 측정. 예열된 경우는 약 15 ms). 사용자가 기다리지 않는 지금 그 비용을
    // 치러 두면 첫 실제 캡슐이 빨라진다. 반환된 id는 의도적으로 저장하지 않는다.
    // OnCaretResolved는 pendingCaretId_와 맞지 않는 완료를 무시하고, 실제 요청은
    // 세대(generation)로 이 요청을 그냥 대체한다.
    if (fg != nullptr && tid != 0) {
        caret_.RequestAsync(lastForeground_, lastForegroundTid_);
    }
    // Chromium 렌더러의 지연 접근성 깨우기는 포커스 표시기를 위한 것이다(그래야
    // 새 탭이 포커스 이벤트를 낸다). AccessibleObjectFromWindow는 렌더러마다
    // 동기 SendMessage라서 바쁜 렌더러에 걸리면 UI 스레드가 멈추므로, 기능이
    // 꺼져 있을 때는 앱 전환마다 그 비용을 치르지 않는다. (언어/caps 캡슐의 캐럿
    // 해석은 작업자 스레드의 UIA 클라이언트가 스스로 공급자를 깨운다.)
    if (config_.notifyFocusChange) {
        PokeRendererAccessibility(fg);
    }

    // 포어그라운드 변경은 포커스 이벤트 폭주의 일부다. 입력 필드를 클릭해 창을
    // 활성화하면 FOREGROUND와 FOCUS 이벤트가 앱마다 다른 순서로 발생하는데,
    // 뒤늦게 도착한 포어그라운드 이벤트는 이미 표시된 포커스 캡슐을 숨기고 다시는
    // 되살리지 않는다. 여기서 안정화 타이머를 다시 걸면 그 이벤트도 폭주에 함께
    // 접히므로, 캡슐은 마지막 이벤트 뒤에 한 번만 나타난다.
    // (ShowFocusPill은 자신의 OnForeground 호출 뒤에 이 타이머를 죽이므로, 거기서
    // 수행하는 갱신이 무한 루프를 다시 걸 수는 없다.)
    if (config_.notifyFocusChange) {
        SetTimer(trayWnd_, kTimer_FocusShow, kFocusSettleMs, nullptr);
    }

    PushDiagStatus();
}

// ---------------------------------------------------------------------------
// 이벤트 처리기
// ---------------------------------------------------------------------------

void App::OnTrayLanguage(Language lang, bool methodSwitch) {
    ++languageChangeCount_;
    IMI_DEBUG(L"App: 트레이 언어 보고 -> %d (이전 %d, 전환=%d)", static_cast<int>(lang),
              static_cast<int>(sm_.Current().language), methodSwitch ? 1 : 0);
    // 언어 키 틱이 있어야 상태 머신이 실제 한/영 토글과, 유예 구간 안에서 일어나는
    // 트레이의 앱 전환 동기화를 구별할 수 있다.
    sm_.OnLanguage(lang, CapsLockMonitor::LastLanguageKeyTick(),
                   methodSwitch);   // emit 가능 -> OnIndicator
    PushDiagStatus();
}

// 감시자가 표시기를 오래 읽지 못하는 이상 상태(대개 explorer 접근성 트리 병듦)에
// 들어가거나 회복될 때. 로그는 진단 창을 열어야 보이므로, 사용자가 조치할 수 있게
// 트레이 풍선 알림으로 surface한다. 감시자가 전이당 한 번만 보내므로 여기서
// 별도의 스팸 방지는 필요 없다.
void App::OnTrayWatcherHealth(TrayWatcherHealth state) {
    if (state == TrayWatcherHealth::Unreadable) {
        explorerRestartAdvised_ = true;
        ReportAnomaly(Anomaly::IndicatorUnreadable,
                      L"작업 표시줄 입력 표시기 읽기 실패 (사용자에게 알림)");
        // 알림 본문을 클릭하면(또는 트레이 메뉴에서) 탐색기 재시작을 확인받는다.
        tray_.ShowBalloon(L"Windows 탐색기 재시작 필요",
                          L"작업 표시줄에서 입력 표시기를 찾을 수 없습니다.",
                          /*warning*/ true);
    } else {
        explorerRestartAdvised_ = false;
        IMI_INFO(L"App: 입력 표시기를 찾았습니다. 사용자에게 알립니다.");
        tray_.ShowBalloon(IMI_PRODUCT_NAME_W, L"작업 표시줄에서 입력 표시기를 찾았습니다.");
    }
}

// 알림 클릭 또는 트레이 메뉴에서 호출된다. 탐색기 재시작은 작업 표시줄이 사라지고
// 열린 탐색기 창이 닫히는 파괴적 동작이라, 반드시 사용자 동의를 먼저 받는다.
// 모달 대화상자는 이 UI 스레드에서 메시지를 계속 펌프하므로 LL 키보드 훅은 살아
// 있고, 작업자 스레드도 영향받지 않는다.
void App::ConfirmAndRestartExplorer() {
    SetForegroundWindow(trayWnd_);   // 숨은 소유자라 대화상자를 앞으로 끌어온다
    const int r = MessageBoxW(
        trayWnd_,
        L"작업 표시줄에서 입력 표시기를 찾을 수 없습니다.\n"
        L"Windows 탐색기를 재시작할까요?\n"
        L"(작업 표시줄이 잠시 사라지고 열려 있는 파일 탐색기 창이 닫힙니다.)",
        IMI_PRODUCT_NAME_W,
        MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
    if (r == IDYES) {
        IMI_INFO(L"App: 사용자 확인. Windows 탐색기를 재시작합니다.");
        RestartExplorer();
    }
}

// 트레이 메뉴 "이상 제보…". 무엇을 수집하는지 먼저 알리고 동의를 받은 뒤, 진단
// 리포트 파일을 만들어 폴더에 열고 GitHub 새 이슈를 띄운다. 실제 번들 생성은
// Diagnostics가 맡는다(파일·환경·이상 요약·최근 로그).
void App::ConfirmAndReportIssue() {
    SetForegroundWindow(trayWnd_);
    const int r = MessageBoxW(
        trayWnd_,
        L"진단 리포트를 만들어 이상을 제보합니다.\n\n"
        L"수집 항목: 앱·OS 버전, 현재 입력 상태, 이상 카운터, 최근 로그"
        L"(좌표·단계·앱 이름). 타이핑한 내용은 포함되지 않습니다.\n\n"
        L"리포트 파일이 폴더에 열리고 GitHub 새 이슈가 브라우저에 뜹니다. 파일을 "
        L"검토한 뒤 이슈에 첨부해 제출하세요. 계속할까요?",
        L"이상 제보 \x2014 " IMI_PRODUCT_NAME_W,
        MB_YESNO | MB_ICONINFORMATION);
    if (r != IDYES) {
        return;
    }
    if (!diag_.ReportIssue()) {
        MessageBoxW(trayWnd_, L"리포트 파일을 만들지 못했습니다. 로그를 확인하세요.",
                    IMI_PRODUCT_NAME_W, MB_OK | MB_ICONWARNING);
    }
}

void App::OnCapsLock(bool capsOn) {
    // 훅이 에지로 추적한 상태를 신뢰한다. GetKeyState(VK_CAPITAL)은 호출한 스레드의
    // 입력 큐를 기준으로 해석되는데, 이 프로세스는 키보드 포커스를 가진 적이 없으므로
    // 토글 비트가 뒤처지거나 아예 갱신되지 않는다. 여기서 다시 질의하면 실제 토글을
    // 바로잡는 게 아니라 뒤집게 된다.
    sm_.OnCapsLock(capsOn);
}

void App::OnIndicator(const IndicatorDecision& decision) {
    // 상태 머신은 허용되고 눈에 보여야 하는 변경만 내보내므로(설정 게이트가 거기
    // 있다), 이 지점에 도달한 결정은 전부 표시된다.
    switch (decision.kind) {
    case IndicatorKind::CapsLockOn:
        // Caps 켜짐: 지속 캡슐을 표시하고, 입력 중 숨김 / 유휴 시 재표시 수명 주기를
        // 관리하기 시작한다.
        capsActive_    = true;
        capsPillShown_ = true;
        ShowOverlayFor(decision, /*sticky*/ true);
        SetTimer(trayWnd_, kTimer_CapsIdle, kCapsIdlePollMs, nullptr);
        break;
    case IndicatorKind::CapsLockOff:
        // Caps 꺼짐: 관리를 멈추고 캡슐을 없앤다.
        capsActive_    = false;
        capsPillShown_ = false;
        KillTimer(trayWnd_, kTimer_CapsIdle);
        overlay_.Hide();
        break;
    default:
        // 언어 변경: 평소의 일시적인 팝업 후 페이드. caps를 관리 중이라면 이 일시적
        // 캡슐이 오버레이를 가져가므로, caps 캡슐을 표시되지 않음으로 표시해 두어
        // 입력이 잦아든 뒤 유휴 폴링이 다시 세우게 한다.
        if (capsActive_) capsPillShown_ = false;
        ShowOverlayFor(decision, /*sticky*/ false);
        break;
    }
    PushDiagStatus();
}

// 현재 상태로부터 caps 결정을 다시 만들어 고정(sticky) 캡슐로 표시한다.
void App::ShowCapsSticky() {
    IndicatorDecision d;
    d.kind  = IndicatorKind::CapsLockOn;
    d.state = sm_.Current();
    ShowOverlayFor(d, /*sticky*/ true);
}

// caps가 켜져 있는 동안 마지막 타이핑 키 입력 이후 경과 시간을 폴링한다. 사용자가
// 한창 입력 중이면 캡슐을 숨기고, 잠시 쉬면 다시 가져온다.
void App::OnCapsIdleTick() {
    if (!capsActive_) {
        KillTimer(trayWnd_, kTimer_CapsIdle);
        return;
    }
    const uint64_t idle = GetTickCount64() - CapsLockMonitor::LastTypingKeyTick();
    const bool shouldShow = config_.notifyCapsLock && (idle >= kCapsReshowIdleMs);

    if (shouldShow && !capsPillShown_) {
        ShowCapsSticky();
        capsPillShown_ = true;
        PushDiagStatus();
    } else if (!shouldShow && capsPillShown_) {
        overlay_.Hide();      // 입력 중이거나 설정으로 꺼졌을 때 숨긴다
        capsPillShown_ = false;
    }
}

void App::ShowOverlayFor(const IndicatorDecision& decision, bool sticky, bool requireTextInput) {
    OverlayContent content;
    if (decision.kind == IndicatorKind::CapsLockOn ||
        decision.kind == IndicatorKind::CapsLockOff) {
        // 윤곽선 화살표 글리프 하나만, 캡션 없이(macOS 캡슐과 동일하다). 켜짐은
        // 잠긴 ⇪(밑줄 있는 것), 꺼짐은 평범한 ⇧ 화살표.
        content.glyph = (decision.kind == IndicatorKind::CapsLockOn) ? L"⇪" : L"⇧";
    } else {
        content.glyph = LanguageGlyph(decision.state.language, decision.state.imeOpen);
    }

    // 이미 큐에 들어간 것은 곧 대체할 캡슐을 위한 것이다.
    CancelPendingCaret();

    pendingContent_          = content;
    pendingStyle_            = StyleFromConfig();
    pendingSticky_           = sticky;
    pendingRequireTextInput_ = requireTextInput;

    // 살아 있는 Win32 캐럿은 읽는 데 비용이 들지 않으므로, 가져와서 즉시 그린다.
    CaretResult caret;
    if (caret_.TryFast(lastForeground_, lastForegroundTid_, caret)) {
        PresentPending(caret);
        return;
    }

    // 그렇지 않으면 나머지 체인(MSAA -> IA2 -> UIA)은 작업자 스레드에 넘기고
    // 메시지 루프로 돌아간다. 대신 여기서 블로킹하면 오버레이 애니메이션이
    // 멈추고, 해석이 끝날 때까지 큐에 쌓인 WinEvent가 버려진다.
    pendingCaretId_ = caret_.RequestAsync(lastForeground_, lastForegroundTid_);
    if (pendingCaretId_ == 0) {
        IMI_DEBUG(L"App: 캡슐 생략 (캐럿 해석기 없음)");
        return;
    }
    SetTimer(trayWnd_, kTimer_CaretTimeout, kCaretTimeoutMs, nullptr);
}

// 작업자 스레드가 답했다. 기다리던 요청이 아닌 것은 전부 낡은 것이다.
void App::OnCaretResolved(uint64_t requestId) {
    if (pendingCaretId_ == 0 || requestId != pendingCaretId_) {
        return;
    }
    CaretResult caret;
    const bool ok = caret_.TakeResult(requestId, caret);
    KillTimer(trayWnd_, kTimer_CaretTimeout);
    pendingCaretId_ = 0;
    if (!ok) {
        // 기준으로 삼을 것이 없다. 위치를 추측하느니 아무것도 표시하지 않는다.
        IMI_DEBUG(L"App: 캡슐 생략 (캐럿을 찾지 못함)");
        return;
    }
    PresentPending(caret);
}

// 공급자가 제때 답하지 않았다. 캡슐을 놓을 자리가 없으므로 버린다. 뒤늦게 온 답은
// 요청 id가 더 이상 맞지 않으므로 무시된다.
void App::OnCaretTimeout() {
    KillTimer(trayWnd_, kTimer_CaretTimeout);
    if (pendingCaretId_ == 0) {
        return;
    }
    ReportAnomaly(Anomaly::CaretTimeout, L"캐럿 해석 %ums 초과 (exe=%s)",
                  kCaretTimeoutMs, lastForegroundExe_);
    pendingCaretId_ = 0;
}

// 진행 중인 해석을 버린다. 그 답은 이미 떠나온 창을 설명하기 때문이다.
void App::CancelPendingCaret() {
    if (pendingCaretId_ != 0) {
        pendingCaretId_ = 0;
        KillTimer(trayWnd_, kTimer_CaretTimeout);
    }
}

void App::PresentPending(const CaretResult& caret) {
    g_lastCaret = caret;
    IMI_DEBUG(L"App: 캐럿 단계=%d 텍스트=%d %ums 영역=(%ld,%ld)-(%ld,%ld) dpi=%lu exe=%s",
              static_cast<int>(caret.method), caret.textInput ? 1 : 0,
              static_cast<unsigned>(caret.elapsedMs),
              caret.rect.left, caret.rect.top, caret.rect.right, caret.rect.bottom,
              static_cast<unsigned long>(caret.dpi), lastForegroundExe_);

    if (pendingRequireTextInput_ && !caret.textInput) {
        IMI_DEBUG(L"App: 포커스 캡슐 생략 (포커스가 텍스트 입력이 아님)");
        return;
    }
    overlay_.Show(pendingContent_, caret.rect, caret.dpi, pendingStyle_, pendingSticky_);
    PushDiagStatus();
}

OverlayStyle App::StyleFromConfig() const {
    OverlayStyle s;
    s.pillHeight96 = config_.overlaySize96;
    s.caretGap96   = config_.caretGap96;
    s.holdMs       = config_.overlayHoldMs;
    s.fadeInMs     = config_.overlayFadeInMs;
    s.fadeOutMs    = config_.overlayFadeOutMs;
    s.peakOpacity  = config_.overlayOpacity;
    return s;
}

// ---------------------------------------------------------------------------
// 트레이 명령
// ---------------------------------------------------------------------------

void App::OnTrayCommand(UINT commandId) {
    switch (commandId) {
    case IDM_TRAY_TOGGLE_LANG:
        config_.notifyLanguage = !config_.notifyLanguage;
        config_.Save();
        ApplyConfigToSubsystems();
        IMI_INFO(L"App: 언어 표시기 %s.", config_.notifyLanguage ? L"켬" : L"끔");
        PushDiagStatus();
        break;

    case IDM_TRAY_TOGGLE_CAPS:
        config_.notifyCapsLock = !config_.notifyCapsLock;
        config_.Save();
        ApplyConfigToSubsystems();
        IMI_INFO(L"App: caps 표시기 %s.", config_.notifyCapsLock ? L"켬" : L"끔");
        PushDiagStatus();
        break;

    case IDM_TRAY_TOGGLE_FOCUS:
        config_.notifyFocusChange = !config_.notifyFocusChange;
        config_.Save();
        ApplyFocusWatcher();
        if (!config_.notifyFocusChange) KillTimer(trayWnd_, kTimer_FocusShow);
        IMI_INFO(L"App: 포커스 변경 표시기 %s.",
                 config_.notifyFocusChange ? L"켬" : L"끔");
        PushDiagStatus();
        break;

    case IDM_TRAY_SHOW_DIAGNOSTICS:
        PushDiagStatus();
        diag_.ShowWindow();
        break;

    case IDM_TRAY_OPEN_LOGFILE:
        diag_.OpenLogFile();
        break;

    case IDM_TRAY_REPORT_ISSUE:
        PushDiagStatus();   // 리포트에 최신 상태가 담기도록 먼저 밀어 넣는다
        ConfirmAndReportIssue();
        break;

    case IDM_TRAY_START_WITH_WIN: {
        const bool target = !config_.startWithWindows;
        config_.startWithWindows = target;
        if (!config_.ApplyStartWithWindows()) {
            // 설정을 사실과 맞게 유지한다. 메뉴 체크 표시는 실제 Run 항목과 일치해야
            // 하므로, 실패하면 플래그를 되돌린다.
            config_.startWithWindows = !target;
            IMI_WARN(L"App: ApplyStartWithWindows 실패. 설정을 되돌렸습니다.");
        }
        config_.Save();
        break;
    }

    case IDM_TRAY_ABOUT: {
        wchar_t text[512];
        _snwprintf_s(text, _TRUNCATE,
                     L"%s  v%s\n"
                     L"macOS에서 입력 캐럿 아래에 표시되는 입력 상태 표시기의 복제품입니다.\n"
                     L"작업 표시줄 트레이의 입력 상태가 바뀌면 캐럿 아래에 표시됩니다.",
                     IMI_PRODUCT_NAME_W, IMI_VERSION_STRING_W);
        MessageBoxW(trayWnd_, text, L"정보 \x2014 " IMI_PRODUCT_NAME_W,
                    MB_OK | MB_ICONINFORMATION);
        break;
    }

    case IDM_TRAY_RESTART_EXPLORER:
        ConfirmAndRestartExplorer();
        break;

    case IDM_TRAY_EXIT:
        IMI_INFO(L"App: 트레이 메뉴에서 종료를 요청했습니다.");
        PostQuitMessage(0);
        break;

    default:
        break;
    }
}

// ---------------------------------------------------------------------------
// 설정 연결 + 진단 상태
// ---------------------------------------------------------------------------

void App::ApplyConfigToSubsystems() {
    Log::SetMinLevel(config_.logLevel);
    sm_.SetGates(config_.notifyLanguage, config_.notifyCapsLock);
}

void App::PushDiagStatus() {
    DiagStatus st;
    st.state            = sm_.Current();
    st.lastCaretMethod  = static_cast<int>(g_lastCaret.method);
    st.lastCaretRect    = g_lastCaret.rect;
    st.lastCaretDpi     = g_lastCaret.dpi;
    st.lastCaretElapsedMs = g_lastCaret.elapsedMs;
    st.indicatorFound   = watcher_.IndicatorFound();
    st.languageChanges  = languageChangeCount_;
    st.lastForegroundPid = lastForegroundPid_;
    lstrcpynW(st.lastForegroundExe, lastForegroundExe_,
              static_cast<int>(ARRAYSIZE(st.lastForegroundExe)));
    diag_.UpdateStatus(st);
}

} // namespace imi
