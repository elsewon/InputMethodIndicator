// DpiUtil.cpp — Per-Monitor-V2 DPI 도우미.
//
// shcore(GetDpiForMonitor), user32(GetDpiForWindow /
// SetProcessDpiAwarenessContext), gdi32(GetDeviceCaps)를 사용한다.
#include "DpiUtil.h"

#include <shellscalingapi.h>  // GetDpiForMonitor, MDT_EFFECTIVE_DPI

namespace imi {

namespace {

// |mon|의 실효 DPI. 모니터 DPI를 조회할 수 없으면 0을 반환하므로 호출자는 대체
// 체인의 다음 단계로 넘어갈 수 있다.
uint32_t DpiFromMonitor(HMONITOR mon) {
    if (mon != nullptr) {
        UINT dpiX = 0, dpiY = 0;
        if (SUCCEEDED(GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dpiX, &dpiY)) &&
            dpiX != 0) {
            return static_cast<uint32_t>(dpiX);
        }
    }
    return 0;
}

// 주 디스플레이의 디바이스 컨텍스트에서 얻는 시스템 DPI. 그것마저 실패하면 96.
uint32_t SystemDpiFallback() {
    HDC hdc = GetDC(nullptr);
    if (hdc != nullptr) {
        int lpx = GetDeviceCaps(hdc, LOGPIXELSX);
        ReleaseDC(nullptr, hdc);
        if (lpx > 0) {
            return static_cast<uint32_t>(lpx);
        }
    }
    return 96;
}

} // namespace

uint32_t DpiForPoint(POINT pt) {
    uint32_t dpi = DpiFromMonitor(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST));
    if (dpi != 0) {
        return dpi;
    }
    // 모니터별 값이 없다: 데스크톱에 대한 GetDpiForWindow는 시스템 DPI를 준다.
    UINT winDpi = GetDpiForWindow(GetDesktopWindow());
    if (winDpi != 0) {
        return static_cast<uint32_t>(winDpi);
    }
    return SystemDpiFallback();
}

uint32_t DpiForWindow(HWND hwnd) {
    uint32_t dpi = DpiFromMonitor(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST));
    if (dpi != 0) {
        return dpi;
    }
    UINT winDpi = GetDpiForWindow(hwnd);
    if (winDpi != 0) {
        return static_cast<uint32_t>(winDpi);
    }
    return SystemDpiFallback();
}

void EnsurePerMonitorV2Awareness() {
    // 멱등이다: 보통은 매니페스트가 이것을 설정한다. 인식이 이미 확립된 뒤의 늦은
    // 호출은 무해하게 실패하므로(ERROR_ACCESS_DENIED), 시작할 때마다 무해한 실패를
    // 로그로 남기는 대신 결과를 무시한다.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
}

} // namespace imi
