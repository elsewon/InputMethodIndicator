// Diagnostics.h — 이 앱의 문제 진단 도구 모음.
//
// 소유물: 최근 로그 줄을 담는 메모리 내 링 버퍼, 그 링 버퍼에 로그를 공급하는
// imi::Log 싱크(OutputDebugStringW와 선택적 파일 출력 포함), 그리고 트레이
// 메뉴에서 여는 가벼운 로그 뷰어 창.
//
// 싱크 계약: OnLogLine은 로거의 잠금을 붙든 채 실행되며, UI가 아닌 스레드
// (예: 캐럿 UIA 작업자 스레드)에서 실행될 수도 있다. 따라서 창을 절대 건드리지
// 않는다 — 링 버퍼에 추가하고, 이미 열려 있는 로그 파일에 쓰고, 원자적 dirty
// 플래그를 세울 뿐이다. 뷰어는 표시 중일 때 짧은 WM_TIMER로 dirty 플래그를
// 폴링해서 새로 늘어난 줄만 편집 컨트롤에 덧붙인다.
#pragma once

#include <windows.h>
#include <cstdint>
#include <string>
#include <mutex>
#include <deque>
#include <atomic>
#include "Log.h"
#include "InputState.h"

namespace imi {

// 사용 중 감지하는 이상 유형. 안정적인 코드(AnomalyCode)로 로그·상태 창·제보
// 리포트에 그대로 실려, 리포트만으로 자가 설명이 되게 한다. 순서를 바꾸지 않는다.
enum class Anomaly : int {
    CaretTimeout = 0,       // 캐럿 해석이 예산을 넘겨 캡슐을 버림
    IndicatorUnreadable,    // 작업 표시줄 입력 표시기를 오래 읽지 못함(대개 explorer 병듦)
    UiStall,                // UI 스레드가 메시지를 펌프하지 못함 → 시스템 전역 입력 지연
    Count
};

// 이상 유형별 누적 카운터와 마지막 이상 요약. GetAnomalies로 스냅샷을 얻는다.
struct AnomalySnapshot {
    uint64_t counts[static_cast<int>(Anomaly::Count)] = {0};
    Anomaly  lastCode = Anomaly::Count;   // Count = 아직 없음
    uint64_t lastTick = 0;                // GetTickCount64(마지막 이상 시각)
    wchar_t  lastDetail[128] = {0};
};

// 이상을 기록한다: 카운터를 올리고 마지막 이상을 갱신하며, 코드 태그를 붙여
// WARN 로그를 남긴다. 스레드 안전하다(UI 스레드·감시견 스레드 모두에서 부른다).
void ReportAnomaly(Anomaly code, const wchar_t* detailFmt = nullptr, ...);
AnomalySnapshot GetAnomalies();
const wchar_t* AnomalyCode(Anomaly code);    // "CARET_TIMEOUT" 등(리포트/로그용)
const wchar_t* AnomalyLabel(Anomaly code);   // "캐럿 타임아웃" 등(상태 창용)

// 진단 창이 상단에 표시하는 실시간 상태(앱이 갱신한다).
struct DiagStatus {
    InputStateSnapshot state;
    int      lastCaretMethod = 0;      // 마지막으로 성공한 CaretResolver 대체 단계
    RECT     lastCaretRect = {0,0,0,0};
    uint32_t lastCaretDpi = 96;
    uint32_t lastCaretElapsedMs = 0;   // 마지막 캐럿 해석에 걸린 시간
    bool     indicatorFound = false;   // UIA로 찾아낸 트레이 입력 표시기
    uint64_t languageChanges = 0;      // 트레이에서 받은 총 언어 변경 횟수
    uint32_t lastForegroundPid = 0;
    wchar_t  lastForegroundExe[64] = {0};
};

class Diagnostics {
public:
    // 로그 싱크(링 버퍼 + OutputDebugString + 선택적 파일)를 설치하고 최소
    // 레벨을 설정한다. 로그 파일을 한 번 열고(크기 기준 회전 포함), 그 핸들은
    // 프로세스 수명 내내 열린 채로 둔다. 시작 시 한 번만 호출한다.
    void Init(HINSTANCE hinst, LogLevel minLevel, bool logToFile);
    void Shutdown();

    // 로그 뷰어 창을 표시한다(필요하면 생성한다).
    void ShowWindow();

    // 디스크의 로그 파일을 기본 텍스트 편집기로 연다.
    void OpenLogFile();

    // 이상 제보 번들을 만든다: 환경·현재 상태·이상 요약·최근 로그를 담은 자기완결
    // 리포트 파일을 쓰고, 그 파일을 선택한 채 폴더를 열며, 제목·본문이 채워진
    // GitHub 새 이슈를 브라우저로 연다(사용자가 파일을 첨부·검토 후 제출). 타이핑
    // 내용은 담지 않는다. 파일을 쓰지 못하면 false. UI 스레드에서 호출한다.
    bool ReportIssue();

    // 상태 갱신(UI 스레드). 뷰어가 새로고침 타이머에서 가져간다.
    void UpdateStatus(const DiagStatus& status);
    DiagStatus GetStatus() const;

    // 링 버퍼 전체를 CRLF로 이어 붙인 하나의 문자열로 복사한다.
    std::wstring SnapshotLog() const;

    HINSTANCE Instance() const { return hinst_; }

private:
    static void CALLBACK LogSinkThunk(LogLevel level, const wchar_t* line, void* ctx);
    void OnLogLine(LogLevel level, const wchar_t* line);
    void OpenLogFileHandle();          // 디스크 로그를 열고 크기 기준으로 회전
    void EnsureWindow();
    void RefreshStatusPane();
    // 편집 컨트롤을 최신 상태로 만든다. |force|는 링 버퍼 전체로 다시 쓰고,
    // 그렇지 않으면 renderedSeq_보다 새로운 줄만 덧붙인다.
    void RefreshLogPane(bool force);

    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

    HINSTANCE hinst_ = nullptr;
    HWND      hwnd_ = nullptr;       // 뷰어 창(숨김 상태에서는 null일 수 있다)
    HWND      hEdit_ = nullptr;      // 읽기 전용 여러 줄 편집 컨트롤
    HWND      hStatus_ = nullptr;    // 상태 표시용 STATIC 컨트롤

    bool         logToFile_ = false;
    std::wstring logFilePath_;
    HANDLE       logFileHandle_ = INVALID_HANDLE_VALUE;

    mutable std::mutex mutex_;
    std::deque<std::wstring> ring_;  // 최근 줄들, kRingCap 개로 제한
    uint64_t ringNextSeq_ = 0;       // 지금까지 추가된 총 줄 수
    DiagStatus status_{};

    std::atomic<bool> logDirty_{false};
    std::atomic<bool> statusDirty_{false};
    uint64_t renderedSeq_ = 0;       // UI 스레드: 이미 편집 컨트롤에 들어간 줄 수

    static constexpr size_t kRingCap = 2000;
};

} // namespace imi
