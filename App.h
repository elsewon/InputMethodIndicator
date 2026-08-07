// App.h — 최상위 애플리케이션 객체. 모든 서브시스템과 숨은 트레이 창을 소유하고,
// 서로 연결하며, 단일 UI 스레드 메시지 루프를 돌린다.
//
// 구조: 입력 상태는 OS 입력 표시기를 감시해서 얻는다
// (TrayIndicatorWatcher, UI Automation). 가벼운 EVENT_SYSTEM_FOREGROUND 훅이
// 캐럿 배치를 위해 포커스된 창을 추적하고, 앱 전환 때 캡슐이 번쩍이지 않도록 짧은
// 유예 구간을 연다.
#pragma once

#include <windows.h>
#include "Config.h"
#include "Diagnostics.h"
#include "TrayIcon.h"
#include "OverlayWindow.h"
#include "CaretResolver.h"
#include "InputStateMachine.h"
#include "CapsLockMonitor.h"
#include "TrayIndicatorWatcher.h"
#include "FocusWatcher.h"

namespace imi {

class App {
public:
    App() = default;
    ~App() = default;

    App(const App&) = delete;
    App& operator=(const App&) = delete;

    // 전체 수명 주기. 프로세스 종료 코드를 반환한다. 단일 인스턴스를 강제한다.
    int Run(HINSTANCE hinst, LPWSTR cmdLine, int nCmdShow);

private:
    bool AcquireSingleInstance();
    bool CreateHostWindow();          // 숨은 트레이/메시지 창
    bool StartSubsystems();
    void Shutdown();

    // 이벤트 처리기(전부 UI 스레드에서 실행).
    void OnIndicator(const IndicatorDecision& decision);  // sm_ -> 오버레이 + 트레이
    // 트레이 감시자로부터. |methodSwitch| = 입력기 전환에서 온 보고(같은 글리프라도
    // 캡슐을 띄운다 — InputStateMachine::OnLanguage 참고).
    void OnTrayLanguage(Language lang, bool methodSwitch);
    // 트레이 감시자의 건강 상태 전이(이상/회복). 트레이 풍선 알림으로 사용자에게 surface한다.
    void OnTrayWatcherHealth(TrayWatcherHealth state);
    // 사용자에게 확인을 받은 뒤 Windows 탐색기를 다시 시작한다(알림 클릭 또는
    // 트레이 메뉴에서 호출). 파괴적 동작이라 반드시 동의를 먼저 받는다.
    void ConfirmAndRestartExplorer();
    // 트레이 메뉴 "이상 제보…". 무엇을 수집하는지 알린 뒤 동의를 받고 제보
    // 번들(리포트 파일 + 폴더 열기 + GitHub 이슈)을 만든다.
    void ConfirmAndReportIssue();
    void OnCapsLock(bool capsOn);
    void OnForeground(HWND fg);                            // WinEvent 훅으로부터
    void OnFocusChanged();          // 두 포커스 소스 중 하나로부터 (WinEvent / UIA)
    void ShowFocusPill();           // 포커스 이벤트 폭주가 잦아든 뒤 발생
    void ApplyFocusWatcher();       // 설정에 따라 UIA 구독을 시작/중지
    void OnTrayCommand(UINT commandId);

    void ApplyConfigToSubsystems();
    void PushDiagStatus();
    // |decision|에 해당하는 캡슐을 표시한다. 캐럿 해석은 블로킹 없이 진행한다.
    // 값싼 Win32 단계가 답을 주면 캡슐이 즉시 나타나고, 그렇지 않으면 UIA 해석을
    // 큐에 넣은 뒤 답(또는 타임아웃)이 도착할 때 PresentPending()이 마무리한다.
    // |requireTextInput|: 포커스가 텍스트를 받는 대상에 있을 때만 표시한다.
    // 포커스 변경 캡슐이 버튼이나 목록 위에서 방해되지 않도록 하기 위함이다.
    void ShowOverlayFor(const IndicatorDecision& decision, bool sticky = false,
                        bool requireTextInput = false);
    void OnCaretResolved(uint64_t requestId);   // 작업자 스레드가 답함
    void OnCaretTimeout();                      // 작업자 스레드가 너무 오래 걸림
    void PresentPending(const CaretResult& caret);
    void CancelPendingCaret();                  // 문맥이 바뀜. 답을 버린다
    OverlayStyle StyleFromConfig() const;

    // Caps Lock의 지속 표시기: caps가 켜져 있는 동안 캡슐이 유지되고, 사용자가
    // 입력하는 중에는 숨겨지며, 잠시 쉬면 다시 나타난다(macOS 동작).
    void ShowCapsSticky();
    void OnCapsIdleTick();

    // UI 스레드 응답성 감시견. 별도 스레드가 주기적으로 트레이 창에
    // SendMessageTimeout(kMsg_WatchdogPing)을 보낸다. UI 스레드가 메시지를
    // 펌프하지 못하면(= LL 키보드 훅이 멈춰 시스템 전역 입력이 지연되는 바로 그
    // 상태) 프로브가 타임아웃되어 UI_STALL 이상으로 기록된다. UI 스레드에 상시
    // 타이머를 걸지 않으려고 프로브 방식을 쓴다.
    void StartWatchdog();
    void StopWatchdog();
    static DWORD WINAPI WatchdogThunk(void* self);
    void WatchdogLoop();

    static LRESULT CALLBACK TrayWndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT HandleTrayMessage(HWND, UINT, WPARAM, LPARAM);

    static void CALLBACK ForegroundEventThunk(HWINEVENTHOOK, DWORD event, HWND hwnd,
                                              LONG idObject, LONG idChild, DWORD, DWORD);

    HINSTANCE hinst_ = nullptr;
    HWND      trayWnd_ = nullptr;
    HANDLE    singleInstance_ = nullptr;
    HWINEVENTHOOK foregroundHook_ = nullptr;
    // explorer 재시작 시 셸이 브로드캐스트하는 등록 메시지. 이것을 받으면 트레이
    // 아이콘을 다시 등록한다(RegisterWindowMessageW(L"TaskbarCreated")).
    UINT      taskbarCreatedMsg_ = 0;
    // 표시기를 오래 읽지 못하는 이상 상태다. 알림 클릭을 탐색기 재시작으로
    // 해석할지, 트레이 메뉴에 조치 항목을 노출할지를 이 플래그로 게이팅한다.
    bool      explorerRestartAdvised_ = false;

    // UI 응답성 감시견 스레드와 그 정지 신호(StartWatchdog/StopWatchdog).
    HANDLE    watchdogThread_ = nullptr;
    HANDLE    watchdogStop_   = nullptr;

    Config              config_;
    Diagnostics         diag_;
    TrayIcon            tray_;
    OverlayWindow       overlay_;
    CaretResolver       caret_;
    InputStateMachine   sm_;
    CapsLockMonitor     caps_;
    TrayIndicatorWatcher watcher_;
    FocusWatcher        focusWatcher_;   // notifyFocusChange일 때만 동작

    // 캐럿 배치와 진단을 위한 포어그라운드 스냅숏.
    HWND    lastForeground_ = nullptr;
    DWORD   lastForegroundTid_ = 0;
    DWORD   lastForegroundPid_ = 0;
    wchar_t lastForegroundExe_[64] = {0};

    uint64_t languageChangeCount_ = 0;

    // Caps 표시기 상태 (ShowCapsSticky / OnCapsIdleTick 참고).
    bool capsActive_    = false;   // caps가 켜져 있고 캡슐을 관리 중이다
    bool capsPillShown_ = false;   // 고정(sticky) caps 캡슐이 현재 표시 중이다

    // 진행 중인 비동기 캐럿 해석 (0 = 없음). 답이 오면 그릴 캡슐을 함께 들고 있는다.
    uint64_t           pendingCaretId_ = 0;
    OverlayContent     pendingContent_;
    OverlayStyle       pendingStyle_;
    bool               pendingSticky_ = false;
    bool               pendingRequireTextInput_ = false;
};

} // namespace imi
