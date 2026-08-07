// Config.h — HKCU\Software\InputMethodIndicator 아래에 저장되는 사용자 설정.
#pragma once

#include <cstdint>
#include "Log.h"

namespace imi {

struct Config {
    bool notifyLanguage    = true;   // 가/A 변경 시 표시기를 띄운다
    bool notifyCapsLock    = true;   // Caps Lock 변경 시 표시기를 띄운다
    bool notifyFocusChange = false;  // 텍스트 필드가 포커스를 받을 때 현재 모드를
                                     // 표시한다(옵트인: 본질적으로 더 잦다)
    bool startWithWindows  = false;  // HKCU\...\Run 항목

    // 오버레이 모양과 타이밍(96 DPI 기준 디자인 픽셀. 모니터별로 배율 적용).
    int  overlayHoldMs    = 650;    // 페이드 아웃 전 완전히 보이는 상태로 유지
    int  overlayFadeInMs  = 130;    // 페이드 + 크기 "팝" 인
    int  overlayFadeOutMs = 260;
    int  overlaySize96    = 24;     // 96 DPI 기준 캡슐 높이(본문 글자 옆에 어울리는 크기)
    int  caretGap96       = 8;      // 96 DPI 기준 캐럿 아래 수직 간격
    uint8_t overlayOpacity = 235;   // 0..255 최대 불투명도

    // 진단.
    LogLevel logLevel = LogLevel::Info;
    bool logToFile    = false;      // %LOCALAPPDATA%\InputMethodIndicator\imi.log

    // 레지스트리에서 읽는다(없는 값은 기본값을 반환하며, 절대 실패하지 않는다).
    // 읽어 온 값은 Sanitize()를 거친다.
    static Config Load();

    // 모든 수치 필드를 정상 범위로 제한한다. 손으로 편집했거나 손상된
    // 레지스트리 값이 비정상적인 오버레이를 만들지 못하게 한다.
    void Sanitize();

    // 모든 값을 레지스트리에 저장한다. 레지스트리 오류 시 false를 반환한다.
    bool Save() const;

    // startWithWindows 값에 맞춰 HKCU Run 항목을 추가하거나 제거한다.
    bool ApplyStartWithWindows() const;

    // 로그 파일의 절대 경로(LOCALAPPDATA. 시작 시 Diagnostics가 약 1 MB에서
    // .old로 회전시킨다). |out|을 채운다.
    static bool GetLogFilePath(wchar_t* out, size_t count);
};

} // namespace imi
