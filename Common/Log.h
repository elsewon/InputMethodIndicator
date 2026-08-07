// Log.h — 앱을 위한 작은 레벨별 로거.
//
// 설계 목표: 핫 패스에서 저렴하고(힙을 건드리지 않으며 절대 예외를 던지지 않음)
// 경로를 바꿀 수 있을 것 — 싱크는 링 버퍼, 선택적 파일, 그리고 진단 창을
// 가리킨다(기본값은 OutputDebugStringW).
//
// 구현: Log.cpp.
#pragma once

#include <cstdint>
#include <cstdarg>

namespace imi {

enum class LogLevel : int32_t {
    Trace = 0,
    Debug = 1,
    Info  = 2,
    Warn  = 3,
    Error = 4,
};

// 싱크는 이미 서식화되고 레벨 태그가 붙은 UTF-16 한 줄을 받는다(끝에 개행 없음).
// 저렴하고 재진입에 안전해야 한다. 로거 잠금을 잡은 상태에서 호출된다.
using LogSink = void (*)(LogLevel level, const wchar_t* line, void* context);

// 전역(프로세스 범위) 로거 상태. 스레드 안전하다.
class Log {
public:
    // 싱크와 최소 레벨을 설치한다. sink에 nullptr을 넘기면 기본값
    // (OutputDebugStringW)이 설치된다. 경로를 바꾸려고 다시 호출해도 안전하다.
    static void Init(LogSink sink, void* context, LogLevel minLevel);

    static void SetMinLevel(LogLevel level);
    static LogLevel MinLevel();

    // 서식화 진입점. printf 방식이며, 고정 크기 스택 버퍼에 맞춰 조용히 잘라낸다.
    // 호출자를 대신해 절대 할당하지 않는다.
    static void Write(LogLevel level, const wchar_t* fmt, ...);
    static void WriteV(LogLevel level, const wchar_t* fmt, va_list args);

    static const wchar_t* LevelName(LogLevel level);
};

} // namespace imi

// 편의 매크로. 핫 패스를 저렴하게 유지하기 위해 서식화하기 전에(BEFORE) 레벨로
// 걸러낸다.
#define IMI_LOG(level, ...) \
    do { if ((level) >= ::imi::Log::MinLevel()) ::imi::Log::Write((level), __VA_ARGS__); } while (0)

#define IMI_TRACE(...) IMI_LOG(::imi::LogLevel::Trace, __VA_ARGS__)
#define IMI_DEBUG(...) IMI_LOG(::imi::LogLevel::Debug, __VA_ARGS__)
#define IMI_INFO(...)  IMI_LOG(::imi::LogLevel::Info,  __VA_ARGS__)
#define IMI_WARN(...)  IMI_LOG(::imi::LogLevel::Warn,  __VA_ARGS__)
#define IMI_ERROR(...) IMI_LOG(::imi::LogLevel::Error, __VA_ARGS__)
