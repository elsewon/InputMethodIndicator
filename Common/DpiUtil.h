// DpiUtil.h — Per-Monitor-V2 DPI 도우미(호스트 전용이지만 헤더는 깔끔하게 유지).
//
// 앱 전체가 Per-Monitor-V2 DPI 인식이다(매니페스트에 선언되어 있다). UIA와
// GetGUIThreadInfo는 *물리* 픽셀을 반환하고 오버레이도 물리(화면) 픽셀로
// 배치하므로, 논리<->물리 변환은 대체로 피한다. 다만 캡슐의 크기와 글꼴을 모니터별로
// 조정하려면 모니터 DPI가 여전히 필요하다.
//
// 구현: DpiUtil.cpp.
#pragma once

#include <windows.h>
#include <cstdint>

namespace imi {

// |pt|(화면 좌표)를 포함하는 모니터의 실효 DPI(dots-per-inch). 최신 API를 쓸 수
// 없으면 시스템 DPI로, 그다음 96으로 대체한다.
uint32_t DpiForPoint(POINT pt);

// |hwnd|를 포함하는 모니터의 실효 DPI.
uint32_t DpiForWindow(HWND hwnd);

// 디자인 픽셀 값(96 DPI 기준으로 작성됨)을 주어진 DPI에 맞게 조정한다.
inline int ScaleForDpi(int value96, uint32_t dpi) {
    return static_cast<int>((static_cast<int64_t>(value96) * dpi + 48) / 96);
}
inline float ScaleForDpiF(float value96, uint32_t dpi) {
    return value96 * (static_cast<float>(dpi) / 96.0f);
}

// 매니페스트가 제거되었더라도 프로세스가 Per-Monitor-V2 인식이 되도록 보장한다.
// 시작 시 어떤 창이든 만들기 전에(BEFORE) 한 번 호출한다. 이미 설정되어 있으면
// 아무것도 하지 않는다.
void EnsurePerMonitorV2Awareness();

} // namespace imi
