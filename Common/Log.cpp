// Log.cpp — 프로세스 전역 레벨별 로거(Log.h 참고).
//
// CRT/COM 의존을 가볍게 유지하며 절대 예외를 던지지 않는다. 핫 패스는 IMI_LOG
// 매크로이며, 서식화하기 전에 MinLevel()(원자적 로드)로 걸러낸다.
#include "Log.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <mutex>
#include <cstdio>

namespace imi {

namespace {

// 싱크와 컨텍스트는 g_mutex 뒤에 있다. 서식화된 줄을 전달하는 콜백 전체가 잠금
// 안에서 실행된다(Log.h의 싱크 계약이 "로거 잠금을 잡은 상태에서 호출된다"고
// 약속한다). MinLevel은 별도의 원자 변수라서 매크로 게이트는 잠금을 잡지 않는다.
std::mutex               g_mutex;
LogSink                  g_sink    = nullptr;
void*                    g_context = nullptr;
std::atomic<LogLevel>    g_minLevel{ LogLevel::Info };

// 기본 싱크: "[IMI][LEVEL] " 접두사를 붙여 OutputDebugStringW에 넘긴다.
void DefaultSink(LogLevel level, const wchar_t* line, void* /*context*/) {
    wchar_t buf[576];
    // _snwprintf_s는 안전하게 잘라낸다. 접두사가 붙은 줄에는 끝에 개행이 없다.
    _snwprintf_s(buf, _countof(buf), _TRUNCATE, L"[IMI][%s] %s",
                 Log::LevelName(level), line);
    ::OutputDebugStringW(buf);
}

} // namespace

void Log::Init(LogSink sink, void* context, LogLevel minLevel) {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_sink    = sink;      // nullptr ⇒ WriteV에서 기본 싱크를 쓴다.
        g_context = context;
    }
    g_minLevel.store(minLevel, std::memory_order_relaxed);
}

void Log::SetMinLevel(LogLevel level) {
    g_minLevel.store(level, std::memory_order_relaxed);
}

LogLevel Log::MinLevel() {
    return g_minLevel.load(std::memory_order_relaxed);
}

void Log::Write(LogLevel level, const wchar_t* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    WriteV(level, fmt, args);
    va_end(args);
}

void Log::WriteV(LogLevel level, const wchar_t* fmt, va_list args) {
    if (level < MinLevel()) {
        return; // 재확인: WriteV는 매크로를 거치지 않고 직접 호출될 수도 있다.
    }

    // 고정 512 wchar 스택 버퍼. 핫 패스에서 힙을 건드리지 않는다. 넘칠 경우
    // _TRUNCATE를 쓴 _vsnwprintf_s는 -1을 반환하지만 널 종료는 해 준다.
    wchar_t buf[512];
    int written = _vsnwprintf_s(buf, _countof(buf), _TRUNCATE, fmt, args);
    if (written < 0) {
        buf[_countof(buf) - 1] = L'\0'; // 방어적 처리. _TRUNCATE가 이미 해 두었다.
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_sink) {
        g_sink(level, buf, g_context);
    } else {
        DefaultSink(level, buf, g_context);
    }
}

const wchar_t* Log::LevelName(LogLevel level) {
    switch (level) {
        case LogLevel::Trace: return L"TRACE";
        case LogLevel::Debug: return L"DEBUG";
        case LogLevel::Info:  return L"INFO";
        case LogLevel::Warn:  return L"WARN";
        case LogLevel::Error: return L"ERROR";
    }
    return L"?????";
}

} // namespace imi
