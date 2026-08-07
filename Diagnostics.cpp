// Diagnostics.cpp — 이 앱의 문제 진단 도구 모음(Diagnostics.h 참고).
//
// 자신을 imi::Log 싱크로 설치한다: 서식이 적용된 모든 줄은 mutex_ 아래에서 크기가
// 제한된 링 버퍼(kRingCap)에 담기고, OutputDebugStringW로 복사되며, 선택적으로
// 디스크의 로그 파일에도 덧붙는다(핸들은 Init에서 한 번 열리고, 약 1 MB에서
// imi.log.old로 크기 기준 회전된다).
//
// 싱크 경로(OnLogLine)의 재진입/교착 규칙:
//  - 로거의 잠금을 붙든 채 실행되므로(Log::Write가 잠금을 쥔 상태로 싱크를 부른다)
//    여기서 IMI_*를 다시 호출해서는 안 된다 — 재귀에 빠져 교착된다. 이 경로의
//    실패는 OutputDebugStringW로만 알린다.
//  - UI가 아닌 스레드에서 실행될 수 있다(캐럿 UIA 작업자 스레드가 로그를 남긴다).
//    따라서 창을 절대 건드리지 않는다: 여기서 UI 스레드로 SendMessage를 보내면
//    로거 잠금을 기다리는 UI 스레드의 Log 호출과 교착될 수 있다. 대신 뷰어가
//    표시 중일 때 WM_TIMER로 원자적 dirty 플래그를 폴링한다.
#include "Diagnostics.h"

#include <windows.h>
#include <shellapi.h>     // ShellExecuteW
#include <cstdio>         // _snwprintf_s, _TRUNCATE
#include <cstdarg>        // va_list (ReportAnomaly)
#include <atomic>
#include <mutex>

#include "Config.h"       // Config::GetLogFilePath
#include "Version.h"      // 리포트 머리말 · GitHub 저장소 URL

namespace imi {

// ---------------------------------------------------------------------------
// 이상 추적(프로세스 전역, 스레드 안전). Log와 같은 방식으로 자유 함수 + 원자
// 카운터로 두어, UI 스레드와 감시견 스레드 어디서든 안전하게 기록한다.
// ---------------------------------------------------------------------------
namespace {

std::atomic<uint64_t> g_anomalyCounts[static_cast<int>(Anomaly::Count)];
std::mutex            g_anomalyLastMutex;
Anomaly               g_anomalyLastCode = Anomaly::Count;
uint64_t              g_anomalyLastTick = 0;
wchar_t               g_anomalyLastDetail[128] = {0};

} // namespace

const wchar_t* AnomalyCode(Anomaly code) {
    switch (code) {
        case Anomaly::CaretTimeout:        return L"CARET_TIMEOUT";
        case Anomaly::IndicatorUnreadable: return L"INDICATOR_UNREADABLE";
        case Anomaly::UiStall:             return L"UI_STALL";
        default:                           return L"UNKNOWN";
    }
}

const wchar_t* AnomalyLabel(Anomaly code) {
    switch (code) {
        case Anomaly::CaretTimeout:        return L"캐럿 타임아웃";
        case Anomaly::IndicatorUnreadable: return L"표시기 읽기 실패";
        case Anomaly::UiStall:             return L"UI 정지";
        default:                           return L"알 수 없음";
    }
}

void ReportAnomaly(Anomaly code, const wchar_t* detailFmt, ...) {
    if (code < Anomaly::CaretTimeout || code >= Anomaly::Count) {
        return;
    }
    wchar_t detail[128];
    detail[0] = L'\0';
    if (detailFmt != nullptr) {
        va_list args;
        va_start(args, detailFmt);
        _vsnwprintf_s(detail, _countof(detail), _TRUNCATE, detailFmt, args);
        va_end(args);
    }

    g_anomalyCounts[static_cast<int>(code)].fetch_add(1, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(g_anomalyLastMutex);
        g_anomalyLastCode = code;
        g_anomalyLastTick = GetTickCount64();
        lstrcpynW(g_anomalyLastDetail, detail, ARRAYSIZE(g_anomalyLastDetail));
    }
    // 코드 태그를 붙여 WARN으로 남긴다. Log 잠금과 이 함수의 잠금은 서로 겹치지
    // 않으므로(여긴 로그 싱크가 아니다) 재진입 우려가 없다.
    IMI_WARN(L"[%s] %s", AnomalyCode(code), detail);
}

AnomalySnapshot GetAnomalies() {
    AnomalySnapshot s;
    for (int i = 0; i < static_cast<int>(Anomaly::Count); ++i) {
        s.counts[i] = g_anomalyCounts[i].load(std::memory_order_relaxed);
    }
    std::lock_guard<std::mutex> lock(g_anomalyLastMutex);
    s.lastCode = g_anomalyLastCode;
    s.lastTick = g_anomalyLastTick;
    lstrcpynW(s.lastDetail, g_anomalyLastDetail, ARRAYSIZE(s.lastDetail));
    return s;
}

namespace {

constexpr const wchar_t* kWindowClass = L"ImiDiagnosticsWindow";
constexpr const wchar_t* kWindowTitle = L"InputMethodIndicator — 진단";

// 뷰어 상단에 상태 창용으로 확보하는 고정 높이(디자인 픽셀).
constexpr int kStatusPaneHeight = 168;

// 뷰어 새로고침 주기와 타이머 id(뷰어 창에서만 쓴다).
constexpr UINT_PTR kRefreshTimerId = 1;
constexpr UINT     kRefreshMs      = 250;

// 디스크 로그가 이 크기를 넘으면 회전시킨다(시작 시 확인한다).
constexpr uint64_t kMaxLogBytes = 1u << 20;   // 1 MB

// 편집 컨트롤을 전체 다시 쓰기로 전환하는 임계값(문자 수). 덧붙이기는 점진적으로
// 이뤄지지만, 그대로 두면 컨트롤의 텍스트가 링 버퍼보다 커진다.
constexpr int kEditTextCapChars = 1 << 20;

const wchar_t* LanguageName(Language lang) {
    switch (lang) {
        case Language::English:  return L"영어";
        case Language::Korean:   return L"한국어";
        case Language::Japanese: return L"일본어";
        case Language::ChineseS: return L"중국어(간체)";
        case Language::ChineseT: return L"중국어(번체)";
        case Language::Unknown:
        default:                 return L"알 수 없음";
    }
}

// 캐럿 대체 체인의 어느 단계가 마지막 위치를 만들어 냈는지
// (UI/CaretResolver.h의 CaretMethod).
const wchar_t* CaretMethodName(int method) {
    switch (method) {
        case 1:  return L"Win32 캐럿";
        case 2:  return L"MSAA 캐럿";
        case 3:  return L"IA2 캐럿";
        case 4:  return L"UIA 텍스트패턴";
        case 5:  return L"UIA 포커스 요소";
        default: return L"없음";
    }
}

// 두 컨트롤이 공유하는, 프로세스 수명 동안 유지되는 고정폭 글꼴 하나. 의도적으로
// 누수시킨다(프로세스 수명 내내 소유). 창별 정리는 필요 없다.
HFONT SharedMonoFont() {
    static HFONT s_font = CreateFontW(
        -12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    return s_font;
}

} // namespace

void Diagnostics::Init(HINSTANCE hinst, LogLevel minLevel, bool logToFile) {
    hinst_ = hinst;
    logToFile_ = logToFile;

    if (logToFile_) {
        wchar_t path[MAX_PATH];
        if (Config::GetLogFilePath(path, ARRAYSIZE(path))) {
            logFilePath_.assign(path);
            OpenLogFileHandle();
        } else {
            // 경로 확인 실패 — 메모리 전용으로 완만하게 격하한다.
            logToFile_ = false;
        }
    }

    // 전역 로거를 이 객체로 연결한다. 창이 만들어지기 전에 호출해도 안전하다.
    // OnLogLine은 창을 절대 건드리지 않는다.
    Log::Init(&Diagnostics::LogSinkThunk, this, minLevel);
    IMI_INFO(L"Diagnostics: 로그 싱크를 설치했습니다 (minLevel=%d, file=%d)",
             static_cast<int>(minLevel), logToFile_ ? 1 : 0);
}

void Diagnostics::Shutdown() {
    // 싱크를 **먼저** 떼어 낸다(Log::Init은 진행 중인 싱크 호출과 직렬화된다).
    // 그래야 아래에서 해체되는 상태로 OnLogLine이 들어오지 않는다.
    Log::Init(nullptr, nullptr, Log::MinLevel());

    if (logFileHandle_ != INVALID_HANDLE_VALUE) {
        CloseHandle(logFileHandle_);
        logFileHandle_ = INVALID_HANDLE_VALUE;
    }

    if (hwnd_ != nullptr) {
        DestroyWindow(hwnd_);   // WM_DESTROY가 핸들 멤버들을 null로 만든다
        hwnd_ = nullptr;
        hEdit_ = nullptr;
        hStatus_ = nullptr;
    }
}

void Diagnostics::OpenLogFileHandle() {
    // 크기 기준 회전: 직전 세대 하나만 imi.log.old로 보관한다.
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (GetFileAttributesExW(logFilePath_.c_str(), GetFileExInfoStandard, &fad)) {
        const uint64_t size =
            (static_cast<uint64_t>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
        if (size > kMaxLogBytes) {
            const std::wstring old = logFilePath_ + L".old";
            MoveFileExW(logFilePath_.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING);
        }
    }

    logFileHandle_ = CreateFileW(logFilePath_.c_str(), FILE_APPEND_DATA,
                                 FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);
    if (logFileHandle_ == INVALID_HANDLE_VALUE) {
        logToFile_ = false;   // 메모리 로깅과 ODS 로깅은 그대로 동작한다
        return;
    }
    LARGE_INTEGER size{};
    DWORD written = 0;
    if (GetFileSizeEx(logFileHandle_, &size) && size.QuadPart == 0) {
        // 새로 만들어진(비어 있는) 파일에는 UTF-16 LE BOM을 쓴다.
        const unsigned char bom[2] = { 0xFF, 0xFE };
        WriteFile(logFileHandle_, bom, 2, &written, nullptr);
    }
}

void Diagnostics::ShowWindow() {
    EnsureWindow();
    if (hwnd_ == nullptr) {
        return;
    }
    RefreshStatusPane();
    RefreshLogPane(/*force*/ true);
    logDirty_.store(false);
    statusDirty_.store(false);
    ::ShowWindow(hwnd_, SW_SHOW);
    ::SetForegroundWindow(hwnd_);
    ::UpdateWindow(hwnd_);
    SetTimer(hwnd_, kRefreshTimerId, kRefreshMs, nullptr);
}

void Diagnostics::OpenLogFile() {
    // 파일 로깅이 꺼져 있어도 경로를 확인한다. 이전 실행에서 남은 파일이 있으면
    // 메뉴 항목이 그대로 동작하게 하기 위해서다.
    std::wstring path = logFilePath_;
    if (path.empty()) {
        wchar_t buf[MAX_PATH];
        if (Config::GetLogFilePath(buf, ARRAYSIZE(buf))) {
            path.assign(buf);
        }
    }
    if (path.empty()) {
        IMI_WARN(L"Diagnostics::OpenLogFile: 사용할 수 있는 로그 경로가 없습니다");
        return;
    }

    HINSTANCE rc = ShellExecuteW(hwnd_, L"open", path.c_str(),
                                 nullptr, nullptr, SW_SHOWNORMAL);
    // ShellExecuteW는 성공 시 32보다 큰 값을 반환한다.
    if (reinterpret_cast<INT_PTR>(rc) <= 32) {
        IMI_WARN(L"Diagnostics::OpenLogFile: ShellExecuteW 실패 rc=%lld 경로=%s",
                 static_cast<long long>(reinterpret_cast<INT_PTR>(rc)), path.c_str());
    }
}

namespace {

// HKLM\...\CurrentVersion에서 정확한 OS 표기를 읽는다. GetVersionEx는 매니페스트
// 없이는 거짓을 답하므로 레지스트리를 직접 읽는다. 실패해도 리포트는 계속된다.
std::wstring OsBuildString() {
    const wchar_t* key = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
    auto regStr = [&](const wchar_t* name) -> std::wstring {
        wchar_t v[128]; DWORD cb = sizeof(v);
        if (RegGetValueW(HKEY_LOCAL_MACHINE, key, name, RRF_RT_REG_SZ, nullptr, v, &cb)
                == ERROR_SUCCESS) {
            return std::wstring(v);
        }
        return std::wstring();
    };
    DWORD ubr = 0, cb = sizeof(ubr);
    RegGetValueW(HKEY_LOCAL_MACHINE, key, L"UBR", RRF_RT_REG_DWORD, nullptr, &ubr, &cb);

    std::wstring product = regStr(L"ProductName");
    std::wstring display = regStr(L"DisplayVersion");
    std::wstring build   = regStr(L"CurrentBuildNumber");
    wchar_t out[256];
    _snwprintf_s(out, _TRUNCATE, L"%s%s%s (빌드 %s.%lu)",
                 product.empty() ? L"Windows" : product.c_str(),
                 display.empty() ? L"" : L", ",
                 display.c_str(),
                 build.empty() ? L"?" : build.c_str(),
                 static_cast<unsigned long>(ubr));
    return std::wstring(out);
}

std::wstring UrlEncode(const std::wstring& s) {
    int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string utf8(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) {
        WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, utf8.data(), n, nullptr, nullptr);
    }
    static const wchar_t hex[] = L"0123456789ABCDEF";
    std::wstring out;
    out.reserve(utf8.size() * 3);
    for (unsigned char c : utf8) {
        const bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                                (c >= '0' && c <= '9') ||
                                c == '-' || c == '_' || c == '.' || c == '~';
        if (unreserved) {
            out.push_back(static_cast<wchar_t>(c));
        } else {
            out.push_back(L'%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 0x0F]);
        }
    }
    return out;
}

} // namespace

bool Diagnostics::ReportIssue() {
    // 리포트 폴더는 로그 파일과 같은 곳(%LOCALAPPDATA%\InputMethodIndicator).
    wchar_t logPath[MAX_PATH];
    if (logFilePath_.empty()) {
        if (!Config::GetLogFilePath(logPath, ARRAYSIZE(logPath))) {
            IMI_WARN(L"Diagnostics::ReportIssue: 리포트 경로를 확인할 수 없습니다");
            return false;
        }
    } else {
        lstrcpynW(logPath, logFilePath_.c_str(), ARRAYSIZE(logPath));
    }
    std::wstring folder(logPath);
    const size_t slash = folder.find_last_of(L'\\');
    if (slash != std::wstring::npos) {
        folder.resize(slash);
    }

    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t name[64];
    _snwprintf_s(name, _TRUNCATE, L"report-%04d%02d%02d-%02d%02d%02d.txt",
                 t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    const std::wstring reportPath = folder + L"\\" + name;

    const DiagStatus s = GetStatus();
    const AnomalySnapshot a = GetAnomalies();
    const std::wstring os = OsBuildString();

    // 리포트 본문을 만든다. 타이핑 내용은 담지 않는다(로그는 좌표·단계·exe명만
    // 기록한다). 활성 창 exe명 정도가 민감 경계이므로 로컬 우선이며, 사용자가
    // 제출 전에 파일을 검토할 수 있다.
    wchar_t head[2048];
    _snwprintf_s(head, _TRUNCATE,
        L"InputMethodIndicator 진단 리포트\r\n"
        L"========================================\r\n"
        L"생성: %04d-%02d-%02d %02d:%02d:%02d\r\n"
        L"앱: %s v%s\r\n"
        L"OS: %s\r\n"
        L"모니터: %d개   가상 화면: %dx%d\r\n"
        L"\r\n"
        L"[현재 상태]\r\n"
        L"입력 언어: %s%s   Caps Lock: %s\r\n"
        L"캐럿 탐지 단계: %d   영역: (%ld,%ld)-(%ld,%ld)   DPI: %lu   소요: %ums\r\n"
        L"입력 표시기: %s   언어 변경 횟수: %llu\r\n"
        L"활성 창: pid %lu  %s\r\n"
        L"\r\n"
        L"[이상 요약]\r\n"
        L"CARET_TIMEOUT: %llu\r\n"
        L"INDICATOR_UNREADABLE: %llu\r\n"
        L"UI_STALL: %llu\r\n"
        L"마지막 이상: %s %s\r\n"
        L"\r\n"
        L"참고: 이 리포트에는 타이핑한 내용이 포함되지 않습니다.\r\n"
        L"\r\n"
        L"[최근 로그]\r\n",
        t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond,
        IMI_PRODUCT_NAME_W, IMI_VERSION_STRING_W,
        os.c_str(),
        GetSystemMetrics(SM_CMONITORS),
        GetSystemMetrics(SM_CXVIRTUALSCREEN), GetSystemMetrics(SM_CYVIRTUALSCREEN),
        LanguageName(s.state.language),
        s.state.imeOpen ? L" (IME 켜짐)" : L"",
        s.state.capsLock ? L"켜짐" : L"꺼짐",
        s.lastCaretMethod,
        static_cast<long>(s.lastCaretRect.left), static_cast<long>(s.lastCaretRect.top),
        static_cast<long>(s.lastCaretRect.right), static_cast<long>(s.lastCaretRect.bottom),
        static_cast<unsigned long>(s.lastCaretDpi),
        static_cast<unsigned>(s.lastCaretElapsedMs),
        s.indicatorFound ? L"찾음" : L"못 찾음",
        static_cast<unsigned long long>(s.languageChanges),
        static_cast<unsigned long>(s.lastForegroundPid),
        (s.lastForegroundExe[0] != L'\0') ? s.lastForegroundExe : L"(없음)",
        static_cast<unsigned long long>(a.counts[static_cast<int>(Anomaly::CaretTimeout)]),
        static_cast<unsigned long long>(a.counts[static_cast<int>(Anomaly::IndicatorUnreadable)]),
        static_cast<unsigned long long>(a.counts[static_cast<int>(Anomaly::UiStall)]),
        (a.lastCode != Anomaly::Count) ? AnomalyCode(a.lastCode) : L"(없음)",
        (a.lastCode != Anomaly::Count) ? a.lastDetail : L"");

    std::wstring content(head);
    content += SnapshotLog();

    // UTF-16 LE + BOM으로 쓴다(로그 파일과 같은 인코딩이라 같은 뷰어로 열린다).
    HANDLE h = CreateFileW(reportPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        IMI_WARN(L"Diagnostics::ReportIssue: 리포트 파일 생성 실패 gle=%lu 경로=%s",
                 GetLastError(), reportPath.c_str());
        return false;
    }
    DWORD written = 0;
    const unsigned char bom[2] = { 0xFF, 0xFE };
    WriteFile(h, bom, 2, &written, nullptr);
    WriteFile(h, content.data(),
              static_cast<DWORD>(content.size() * sizeof(wchar_t)), &written, nullptr);
    CloseHandle(h);
    IMI_INFO(L"Diagnostics: 진단 리포트를 저장했습니다: %s", reportPath.c_str());

    // 리포트 파일을 선택한 채 탐색기를 연다.
    std::wstring selectArgs = L"/select,\"" + reportPath + L"\"";
    ShellExecuteW(hwnd_, nullptr, L"explorer.exe", selectArgs.c_str(),
                  nullptr, SW_SHOWNORMAL);

    // 제목·본문이 채워진 GitHub 새 이슈를 연다. 본문에는 환경·이상 요약만 담고,
    // 사용자가 리포트 파일을 첨부하도록 안내한다(URL 길이 제한 때문에 로그 원문은
    // 넣지 않는다).
    wchar_t body[1024];
    _snwprintf_s(body, _TRUNCATE,
        L"## 증상\r\n(무슨 일이 있었는지 적어 주세요)\r\n\r\n"
        L"## 환경\r\n- 앱: %s v%s\r\n- OS: %s\r\n\r\n"
        L"## 진단 리포트\r\n아래 파일을 이 이슈에 첨부해 주세요(파일 탐색기에 선택된 채 열려 있습니다):\r\n`%s`\r\n\r\n"
        L"## 이상 요약\r\nCARET_TIMEOUT %llu · INDICATOR_UNREADABLE %llu · UI_STALL %llu\r\n",
        IMI_PRODUCT_NAME_W, IMI_VERSION_STRING_W, os.c_str(), reportPath.c_str(),
        static_cast<unsigned long long>(a.counts[static_cast<int>(Anomaly::CaretTimeout)]),
        static_cast<unsigned long long>(a.counts[static_cast<int>(Anomaly::IndicatorUnreadable)]),
        static_cast<unsigned long long>(a.counts[static_cast<int>(Anomaly::UiStall)]));

    const std::wstring url =
        std::wstring(IMI_REPO_URL_W) + L"/issues/new?title=" +
        UrlEncode(L"[이상 제보] ") + L"&body=" + UrlEncode(body);
    ShellExecuteW(hwnd_, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    return true;
}

void Diagnostics::UpdateStatus(const DiagStatus& status) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        status_ = status;
    }
    statusDirty_.store(true);   // 뷰어의 타이머가 표시 중일 때 그려 준다
}

DiagStatus Diagnostics::GetStatus() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
}

std::wstring Diagnostics::SnapshotLog() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::wstring out;
    size_t estimate = 0;
    for (const std::wstring& line : ring_) {
        estimate += line.size() + 2;   // + CRLF 분
    }
    out.reserve(estimate);
    for (const std::wstring& line : ring_) {
        out.append(line);
        out.append(L"\r\n");           // EDIT 컨트롤은 CRLF 줄바꿈이 필요하다
    }
    return out;
}

// ---- private -----------------------------------------------------------------

void CALLBACK Diagnostics::LogSinkThunk(LogLevel level, const wchar_t* line, void* ctx) {
    Diagnostics* self = static_cast<Diagnostics*>(ctx);
    if (self != nullptr && line != nullptr) {
        self->OnLogLine(level, line);
    }
}

void Diagnostics::OnLogLine(LogLevel level, const wchar_t* line) {
    // 로거는 서식화된 본문만 넘긴다(레벨 태그·시각 없음). 진단·제보가 "언제·어느
    // 심각도"를 알 수 있도록 여기서 wall-clock 타임스탬프와 레벨 태그를 붙인다.
    // 이 값이 링 버퍼·파일·디버거 출력 모두의 표준 형식이 된다.
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t stamped[640];
    _snwprintf_s(stamped, _countof(stamped), _TRUNCATE,
                 L"%04d-%02d-%02d %02d:%02d:%02d.%03d [%s] %s",
                 t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond,
                 t.wMilliseconds, Log::LevelName(level), line);

    // 디버거로도 내보낸다 — 우리가 싱크를 소유하는 동안 기본 싱크는 동작하지 않는다.
    OutputDebugStringW(stamped);
    OutputDebugStringW(L"\n");

    // 잠금 아래에서 크기 제한된 링 버퍼에 추가한다. 임계 구역은 짧게 유지한다.
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ring_.emplace_back(stamped);
        ++ringNextSeq_;
        while (ring_.size() > kRingCap) {
            ring_.pop_front();
        }
    }

    // 계속 열려 있는 핸들로 파일에 덧붙인다(줄마다 열고 닫지 않는다). 우리가 붙들고
    // 실행되는 로거 잠금이 이미 쓰는 쪽을 직렬화해 준다.
    if (logToFile_ && logFileHandle_ != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        std::wstring buf(stamped);
        buf.append(L"\r\n");
        WriteFile(logFileHandle_, buf.data(),
                  static_cast<DWORD>(buf.size() * sizeof(wchar_t)),
                  &written, nullptr);
    }

    // 싱크에서는 창을 절대 건드리지 않는다(파일 머리말 참고). dirty 표시만 한다.
    logDirty_.store(true);
}

void Diagnostics::EnsureWindow() {
    if (hwnd_ != nullptr) {
        return;
    }
    if (hinst_ == nullptr) {
        return;   // Init이 아직 호출되지 않았다 — 만들 근거가 없다
    }

    static bool s_registered = false;
    if (!s_registered) {
        WNDCLASSEXW wc{};
        wc.cbSize        = sizeof(wc);
        wc.style         = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc   = &Diagnostics::WndProc;
        wc.hInstance     = hinst_;
        wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        wc.lpszClassName = kWindowClass;
        if (RegisterClassExW(&wc) == 0) {
            DWORD gle = GetLastError();
            if (gle != ERROR_CLASS_ALREADY_EXISTS) {
                IMI_ERROR(L"Diagnostics: RegisterClassExW 실패 gle=%lu", gle);
                return;
            }
        }
        s_registered = true;
    }

    hwnd_ = CreateWindowExW(
        0, kWindowClass, kWindowTitle,
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 760, 520,
        nullptr, nullptr, hinst_, this);   // 'this'를 WM_NCCREATE로 전달한다
    if (hwnd_ == nullptr) {
        IMI_ERROR(L"Diagnostics: CreateWindowExW 실패 gle=%lu", GetLastError());
    }
}

void Diagnostics::RefreshLogPane(bool force) {
    if (hEdit_ == nullptr) {
        return;
    }

    // 편집 컨트롤에 링 버퍼가 담는 양보다 훨씬 많은 텍스트가 쌓이면 전체 다시 쓰기로
    // 되돌린다(덧붙이기만 하면 무한정 커진다).
    bool full = force || renderedSeq_ == 0;
    if (!full && GetWindowTextLengthW(hEdit_) > kEditTextCapChars) {
        full = true;
    }

    // 쓸 줄들을 잠금 아래에서 스냅샷으로 뜬다.
    std::wstring text;
    uint64_t total = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        total = ringNextSeq_;
        uint64_t pending = total - renderedSeq_;
        if (pending == 0 && !full) {
            return;
        }
        if (pending > ring_.size()) {
            full = true;   // 빠진 구간이 링 버퍼 밖으로 밀려났다
        }
        const size_t count = full ? ring_.size() : static_cast<size_t>(pending);
        size_t estimate = 0;
        for (size_t i = ring_.size() - count; i < ring_.size(); ++i) {
            estimate += ring_[i].size() + 2;
        }
        text.reserve(estimate);
        for (size_t i = ring_.size() - count; i < ring_.size(); ++i) {
            text.append(ring_[i]);
            text.append(L"\r\n");
        }
    }
    renderedSeq_ = total;

    if (full) {
        ::SetWindowTextW(hEdit_, text.c_str());
    } else {
        // 끝에 덧붙인다. ES_READONLY가 EM_REPLACESEL을 막으므로 덧붙이는 동안만
        // 해제한다(그래도 컨트롤은 사용자 편집을 전혀 받지 않는다 — WM_CHAR가
        // 이 경로에 도달하지 않는다).
        const int len = GetWindowTextLengthW(hEdit_);
        SendMessageW(hEdit_, EM_SETREADONLY, FALSE, 0);
        SendMessageW(hEdit_, EM_SETSEL, len, len);
        SendMessageW(hEdit_, EM_REPLACESEL, FALSE,
                     reinterpret_cast<LPARAM>(text.c_str()));
        SendMessageW(hEdit_, EM_SETREADONLY, TRUE, 0);
    }

    // 가장 새로운 줄이 화면에 보이도록 유지한다.
    const int end = GetWindowTextLengthW(hEdit_);
    SendMessageW(hEdit_, EM_SETSEL, static_cast<WPARAM>(end), static_cast<LPARAM>(end));
    SendMessageW(hEdit_, EM_SCROLLCARET, 0, 0);
}

void Diagnostics::RefreshStatusPane() {
    if (hStatus_ == nullptr) {
        return;
    }
    DiagStatus s = GetStatus();

    const wchar_t* exe = (s.lastForegroundExe[0] != L'\0')
                             ? s.lastForegroundExe : L"(없음)";

    // 이상 요약: 유형별 누적 카운트 + (있으면) 마지막 이상 코드와 경과 시간.
    const AnomalySnapshot a = GetAnomalies();
    wchar_t lastAnomaly[96];
    if (a.lastCode != Anomaly::Count) {
        const unsigned long long agoSec =
            (GetTickCount64() - a.lastTick) / 1000ull;
        _snwprintf_s(lastAnomaly, _TRUNCATE, L"   최근: %s (%llu초 전)",
                     AnomalyCode(a.lastCode), agoSec);
    } else {
        lastAnomaly[0] = L'\0';
    }

    wchar_t buf[1536];
    _snwprintf_s(buf, ARRAYSIZE(buf), _TRUNCATE,
        L"입력 언어: %s%s   Caps Lock: %s\r\n"
        L"캐럿 탐지: %s(%d)   위치: (%ld,%ld)-(%ld,%ld)   DPI: %lu   소요: %ums\r\n"
        L"입력 표시기: %s   언어 변경 횟수: %llu\r\n"
        L"활성 창: pid %lu  %s\r\n"
        L"이상: 캐럿 타임아웃 %llu · 표시기 %llu · UI 정지 %llu%s",
        LanguageName(s.state.language),
        s.state.imeOpen ? L" (IME 켜짐)" : L"",
        s.state.capsLock ? L"켜짐" : L"꺼짐",
        CaretMethodName(s.lastCaretMethod),
        s.lastCaretMethod,
        static_cast<long>(s.lastCaretRect.left),
        static_cast<long>(s.lastCaretRect.top),
        static_cast<long>(s.lastCaretRect.right),
        static_cast<long>(s.lastCaretRect.bottom),
        static_cast<unsigned long>(s.lastCaretDpi),
        static_cast<unsigned>(s.lastCaretElapsedMs),
        s.indicatorFound ? L"찾음" : L"못 찾음",
        static_cast<unsigned long long>(s.languageChanges),
        static_cast<unsigned long>(s.lastForegroundPid),
        exe,
        static_cast<unsigned long long>(a.counts[static_cast<int>(Anomaly::CaretTimeout)]),
        static_cast<unsigned long long>(a.counts[static_cast<int>(Anomaly::IndicatorUnreadable)]),
        static_cast<unsigned long long>(a.counts[static_cast<int>(Anomaly::UiStall)]),
        lastAnomaly);

    ::SetWindowTextW(hStatus_, buf);
}

LRESULT CALLBACK Diagnostics::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_NCCREATE) {
        CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    Diagnostics* self = reinterpret_cast<Diagnostics*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self == nullptr) {
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    switch (msg) {
    case WM_CREATE: {
        HFONT font = SharedMonoFont();

        self->hStatus_ = CreateWindowExW(
            0, L"STATIC", L"",
            WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX,
            0, 0, 0, 0, hwnd, nullptr,
            reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(hwnd, GWLP_HINSTANCE)),
            nullptr);

        self->hEdit_ = CreateWindowExW(
            WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL |
                ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | ES_AUTOHSCROLL,
            0, 0, 0, 0, hwnd, nullptr,
            reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(hwnd, GWLP_HINSTANCE)),
            nullptr);

        if (font != nullptr) {
            if (self->hStatus_ != nullptr) {
                SendMessageW(self->hStatus_, WM_SETFONT,
                             reinterpret_cast<WPARAM>(font), TRUE);
            }
            if (self->hEdit_ != nullptr) {
                SendMessageW(self->hEdit_, WM_SETFONT,
                             reinterpret_cast<WPARAM>(font), TRUE);
            }
        }
        return 0;
    }

    case WM_TIMER:
        if (wParam == kRefreshTimerId) {
            if (self->statusDirty_.exchange(false)) {
                self->RefreshStatusPane();
            }
            if (self->logDirty_.exchange(false)) {
                self->RefreshLogPane(/*force*/ false);
            }
            return 0;
        }
        break;

    case WM_SIZE: {
        RECT rc;
        GetClientRect(hwnd, &rc);
        int statusH = kStatusPaneHeight;
        if (statusH > rc.bottom) {
            statusH = rc.bottom;
        }
        if (self->hStatus_ != nullptr) {
            MoveWindow(self->hStatus_, 8, 6, rc.right - 16, statusH - 12, TRUE);
        }
        if (self->hEdit_ != nullptr) {
            MoveWindow(self->hEdit_, 0, statusH, rc.right, rc.bottom - statusH, TRUE);
        }
        return 0;
    }

    case WM_CLOSE:
        // 파괴하지 않고 숨긴다: 뷰어는 다시 열려도 링 버퍼를 유지한다.
        KillTimer(hwnd, kRefreshTimerId);
        ::ShowWindow(hwnd, SW_HIDE);
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, kRefreshTimerId);
        self->hwnd_ = nullptr;
        self->hEdit_ = nullptr;
        self->hStatus_ = nullptr;
        return 0;

    default:
        break;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace imi
