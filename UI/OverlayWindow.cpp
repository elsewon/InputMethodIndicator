// OverlayWindow.cpp — 캐럿 근처에 뜨는 표시기 캡슐.
//
// 픽셀 단위 알파를 쓰는 레이어드 윈도우로, GDI DIB에 Direct2D로 그린 뒤
// UpdateLayeredWindow로 표시한다. 위에서 아래로 배치된 32bpp BGRA DIB에
// ID2D1DCRenderTarget(B8G8R8A8_UNORM + ALPHA_MODE_PREMULTIPLIED)으로 렌더링하며,
// 이 렌더 타깃의 DPI는 96으로 고정한다 — 모든 좌표를 *물리* 픽셀로 작성하고
// 모니터별 스케일링을 직접 처리한다. 그렇게 하지 않으면 DC 렌더 타깃이 96이 아닌
// 모니터에서 캡슐을 이중으로 스케일한다. 자체 소유한 ~15ms WM_TIMER가 FadeIn ->
// Hold -> FadeOut -> Hide를 구동한다. D2D 드로우는 Show마다 한 번만 일어나며,
// 페이드 틱은 캐시된 DIB를 새 전역 알파로 다시 표시한다. 새로운 Show()는 앵커를
// 다시 맞추고 타임라인을 재시작한다. 이 창은 절대 활성화되지 않고 입력도 받지 않는다.
#include "OverlayWindow.h"

#include <d2d1.h>
#include <d2d1helper.h>
#include <dwrite.h>
#include <new>

#include "Constants.h"
#include "DpiUtil.h"
#include "Log.h"

namespace imi {

namespace {

// 페이드 애니메이션 틱. ~15ms면 바쁜 대기 없이도 페이드가 부드럽다.
constexpr UINT kFadeTickMs = 15;

// 등장 "팝": 페이드인 동안 캡슐이 이 계수에서 1.0까지 스케일된다. macOS 표시기의
// 스프링 등장과 같다.
constexpr float kAppearStartScale = 0.80f;

template <class T>
void SafeRelease(T*& p) {
    if (p != nullptr) {
        p->Release();
        p = nullptr;
    }
}

// |fmt|를 적용했을 때 |text|의 폭(DIP 단위 == 물리 px, DPI는 96으로 고정).
// 텍스트가 비었거나 실패하면 0을 반환하므로 호출자는 최소 폭으로 대체한다.
float MeasureTextWidth(IDWriteFactory* dwrite, const wchar_t* text, UINT32 len,
                       IDWriteTextFormat* fmt) {
    if (dwrite == nullptr || fmt == nullptr || len == 0) {
        return 0.0f;
    }
    IDWriteTextLayout* layout = nullptr;
    if (FAILED(dwrite->CreateTextLayout(text, len, fmt, 4096.0f, 4096.0f, &layout)) ||
        layout == nullptr) {
        return 0.0f;
    }
    DWRITE_TEXT_METRICS m{};
    layout->GetMetrics(&m);
    layout->Release();
    return m.width;
}

// (|w|,|h|) 크기의 표시 레이아웃. 정렬은 |fmt|에서 상속된다(가운데/가운데).
// 반환된 레이아웃은 호출자가 소유한다. 비었거나 실패하면 nullptr.
IDWriteTextLayout* MakeLayout(IDWriteFactory* dwrite, const wchar_t* text, UINT32 len,
                              IDWriteTextFormat* fmt, float w, float h) {
    if (dwrite == nullptr || fmt == nullptr || len == 0) {
        return nullptr;
    }
    IDWriteTextLayout* layout = nullptr;
    if (FAILED(dwrite->CreateTextLayout(text, len, fmt, w, h, &layout))) {
        return nullptr;
    }
    return layout;
}

} // namespace

// ---------------------------------------------------------------------------
// Gfx — 불투명 디바이스 리소스 보관자(헤더에 선언하고 여기에 정의해서 D2D/DWrite를
// 헤더 밖에 두고, ~OverlayWindow 시점에는 불완전 타입이 완전해지도록 한다).
// 팩토리, 텍스트 포맷, 레이아웃은 디바이스 독립적이라 디바이스 손실에도 살아남는다.
// DC 렌더 타깃과 그 브러시는 D2DERR_RECREATE_TARGET에서 폐기 후 재생성한다.
// ---------------------------------------------------------------------------
struct OverlayWindow::Gfx {
    // 디바이스 독립.
    ID2D1Factory*      d2dFactory    = nullptr;
    IDWriteFactory*    dwriteFactory = nullptr;
    IDWriteTextFormat* glyphFormat   = nullptr;
    float              glyphFormatSize = 0.0f;
    IDWriteTextLayout* glyphLayout   = nullptr;  // Show()마다 다시 만든다

    // 렌더링 대상이자 UpdateLayeredWindow에 넘기는 GDI 표면. 위에서 아래 32bpp.
    HDC     memDc  = nullptr;
    HBITMAP dib    = nullptr;
    HBITMAP oldBmp = nullptr;   // memDc에서 선택 해제된 기본 비트맵
    void*   bits   = nullptr;
    int     dibW   = 0;
    int     dibH   = 0;

    // 디바이스 의존(디바이스 손실 시 재생성).
    ID2D1DCRenderTarget*  rt          = nullptr;
    ID2D1SolidColorBrush* bgBrush     = nullptr;  // 캡슐 채움
    ID2D1SolidColorBrush* outlineBrush = nullptr; // 얇고 밝은 테두리
    ID2D1SolidColorBrush* glyphBrush  = nullptr;  // 글리프 텍스트

    ~Gfx();

    bool EnsureFactories();
    bool EnsureDib(int w, int h);
    bool EnsureRenderTarget();
    bool EnsureBrushes();
    void DiscardDevice();  // rt와 브러시만 해제한다

    IDWriteTextFormat* GlyphFormat(float size);
};

OverlayWindow::Gfx::~Gfx() {
    SafeRelease(glyphLayout);
    SafeRelease(bgBrush);
    SafeRelease(outlineBrush);
    SafeRelease(glyphBrush);
    SafeRelease(rt);
    SafeRelease(glyphFormat);
    SafeRelease(dwriteFactory);
    SafeRelease(d2dFactory);
    if (memDc != nullptr) {
        if (dib != nullptr) {
            SelectObject(memDc, oldBmp);
        }
        DeleteDC(memDc);
        memDc = nullptr;
    }
    if (dib != nullptr) {
        DeleteObject(dib);
        dib = nullptr;
    }
}

bool OverlayWindow::Gfx::EnsureFactories() {
    if (d2dFactory == nullptr) {
        HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &d2dFactory);
        if (FAILED(hr)) {
            d2dFactory = nullptr;
            IMI_ERROR(L"OverlayWindow: D2D1CreateFactory 실패 hr=0x%08lX",
                      static_cast<unsigned long>(hr));
            return false;
        }
    }
    if (dwriteFactory == nullptr) {
        HRESULT hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
                                         __uuidof(IDWriteFactory),
                                         reinterpret_cast<IUnknown**>(&dwriteFactory));
        if (FAILED(hr)) {
            dwriteFactory = nullptr;
            IMI_ERROR(L"OverlayWindow: DWriteCreateFactory 실패 hr=0x%08lX",
                      static_cast<unsigned long>(hr));
            return false;
        }
    }
    return true;
}

bool OverlayWindow::Gfx::EnsureDib(int w, int h) {
    if (w <= 0 || h <= 0) {
        return false;
    }
    if (dib != nullptr && dibW == w && dibH == h) {
        return true;
    }
    if (memDc == nullptr) {
        HDC screen = GetDC(nullptr);
        memDc = CreateCompatibleDC(screen);
        if (screen != nullptr) {
            ReleaseDC(nullptr, screen);
        }
        if (memDc == nullptr) {
            return false;
        }
    }
    // 이전 비트맵을 버린다(먼저 DC의 기본 비트맵을 복원한다).
    if (dib != nullptr) {
        SelectObject(memDc, oldBmp);
        DeleteObject(dib);
        dib = nullptr;
        bits = nullptr;
    }

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = w;
    bmi.bmiHeader.biHeight      = -h;   // 음수 => 위에서 아래
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    dib = CreateDIBSection(memDc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (dib == nullptr) {
        bits = nullptr;
        return false;
    }
    oldBmp = static_cast<HBITMAP>(SelectObject(memDc, dib));
    dibW = w;
    dibH = h;
    return true;
}

bool OverlayWindow::Gfx::EnsureRenderTarget() {
    if (rt != nullptr) {
        return true;
    }
    if (d2dFactory == nullptr) {
        return false;
    }
    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        0.0f, 0.0f,                         // dpi 플레이스홀더. 아래에서 고정한다
        D2D1_RENDER_TARGET_USAGE_NONE,
        D2D1_FEATURE_LEVEL_DEFAULT);
    HRESULT hr = d2dFactory->CreateDCRenderTarget(&props, &rt);
    if (FAILED(hr)) {
        rt = nullptr;
        IMI_ERROR(L"OverlayWindow: CreateDCRenderTarget 실패 hr=0x%08lX",
                  static_cast<unsigned long>(hr));
        return false;
    }
    // 96 DPI로 고정한다 — 기하는 물리 px로 작성하고 스케일링은 직접 한다.
    rt->SetDpi(96.0f, 96.0f);
    return true;
}

bool OverlayWindow::Gfx::EnsureBrushes() {
    if (rt == nullptr) {
        return false;
    }
    if (bgBrush == nullptr) {
        if (FAILED(rt->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 1.0f),
                                             &bgBrush))) {
            bgBrush = nullptr;
            return false;
        }
    }
    if (outlineBrush == nullptr) {
        if (FAILED(rt->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 1.0f),
                                             &outlineBrush))) {
            outlineBrush = nullptr;
            return false;
        }
    }
    if (glyphBrush == nullptr) {
        if (FAILED(rt->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f),
                                             &glyphBrush))) {
            glyphBrush = nullptr;
            return false;
        }
    }
    return true;
}

void OverlayWindow::Gfx::DiscardDevice() {
    SafeRelease(bgBrush);
    SafeRelease(outlineBrush);
    SafeRelease(glyphBrush);
    SafeRelease(rt);
}

IDWriteTextFormat* OverlayWindow::Gfx::GlyphFormat(float size) {
    if (glyphFormat != nullptr && glyphFormatSize == size) {
        return glyphFormat;
    }
    SafeRelease(glyphFormat);
    if (dwriteFactory == nullptr) {
        return nullptr;
    }
    // Segoe UI + DWrite 시스템 폰트 대체로 가(Malgun), A, ⇪를 모두 렌더링한다.
    HRESULT hr = dwriteFactory->CreateTextFormat(
        L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", &glyphFormat);
    if (FAILED(hr)) {
        glyphFormat = nullptr;
        return nullptr;
    }
    glyphFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    glyphFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    glyphFormatSize = size;
    return glyphFormat;
}

// ---------------------------------------------------------------------------
// OverlayWindow
// ---------------------------------------------------------------------------

OverlayWindow::~OverlayWindow() {
    // 소멸자를 .cpp에 정의해서 여기서는 Gfx가 완전한 타입이 되도록 한다.
    Destroy();
}

bool OverlayWindow::Create(HINSTANCE hinst) {
    hinst_ = hinst;

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = 0;
    wc.lpfnWndProc   = &OverlayWindow::WndProc;
    wc.hInstance     = hinst;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kOverlayWindowClass;
    if (RegisterClassExW(&wc) == 0) {
        DWORD gle = GetLastError();
        if (gle != ERROR_CLASS_ALREADY_EXISTS) {
            IMI_ERROR(L"OverlayWindow: RegisterClassExW 실패 gle=%lu", gle);
            return false;
        }
    }

    // WS_POPUP + layered/transparent/topmost/noactivate/toolwindow: 클릭이 통과하는
    // HUD로, 작업 표시줄이나 Alt-Tab에 나타나지 않고 포커스도 빼앗지 않는다.
    hwnd_ = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_NOACTIVATE |
            WS_EX_TOOLWINDOW,
        kOverlayWindowClass, L"", WS_POPUP,
        0, 0, 0, 0, nullptr, nullptr, hinst, this);   // 'this' -> WM_NCCREATE로 전달
    if (hwnd_ == nullptr) {
        IMI_ERROR(L"OverlayWindow: CreateWindowExW 실패 gle=%lu", GetLastError());
        return false;
    }

    gfx_ = new (std::nothrow) Gfx();
    if (gfx_ == nullptr) {
        IMI_ERROR(L"OverlayWindow: Gfx 할당에 실패했습니다");
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
        return false;
    }
    return true;
}

void OverlayWindow::Destroy() {
    if (hwnd_ != nullptr) {
        KillTimer(hwnd_, kTimer_OverlayFade);
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    delete gfx_;
    gfx_ = nullptr;
    visible_ = false;
    phase_ = Phase::Idle;
}

void OverlayWindow::Show(const OverlayContent& content, const RECT& caretRectPhysical,
                         uint32_t dpi, const OverlayStyle& style, bool sticky) {
    if (hwnd_ == nullptr) {
        return;
    }
    content_ = content;
    caret_   = caretRectPhysical;
    dpi_     = (dpi != 0) ? dpi : 96;
    style_   = style;
    sticky_  = sticky;

    ComputePlacement();

    // 타임라인을 처음부터 재시작하고 앵커를 다시 맞춘다. 내용/기하가 새것이므로
    // 캐시된 프레임이 있더라도 낡은 것이다. 등장 "팝"은 축소된 스케일에서
    // 시작하고, 페이드인이 이를 1.0까지 올린다.
    frameValid_     = false;
    scale_          = kAppearStartScale;
    phase_          = Phase::FadeIn;
    phaseStartTick_ = GetTickCount64();
    visible_        = true;

    // 첫 (투명한) 프레임을 표시한 뒤 활성화 없이 창을 드러낸다.
    RenderAndPresent(0);
    SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);

    SetTimer(hwnd_, kTimer_OverlayFade, kFadeTickMs, nullptr);
}

void OverlayWindow::Hide() {
    if (hwnd_ != nullptr) {
        KillTimer(hwnd_, kTimer_OverlayFade);
        ::ShowWindow(hwnd_, SW_HIDE);
    }
    phase_   = Phase::Idle;
    visible_ = false;
    sticky_  = false;
}

LRESULT CALLBACK OverlayWindow::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_NCCREATE) {
        CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        OverlayWindow* self = static_cast<OverlayWindow*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        if (self != nullptr) {
            self->hwnd_ = hwnd;   // Create()가 반환값을 보기 전에 이미 유효하다
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    OverlayWindow* self = reinterpret_cast<OverlayWindow*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self == nullptr) {
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    return self->HandleMessage(msg, wParam, lParam);
}

LRESULT OverlayWindow::HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_TIMER:
        if (wParam == kTimer_OverlayFade) {
            OnFadeTimer();
            return 0;
        }
        break;

    case WM_DISPLAYCHANGE:
        // 디스플레이 구성/포맷이 바뀌었다 — 디바이스 리소스를 버린다. 다음 표시
        // 시점에 지연 재생성된다.
        DiscardDeviceResources();
        break;

    default:
        break;
    }
    return DefWindowProcW(hwnd_, msg, wParam, lParam);
}

void OverlayWindow::OnFadeTimer() {
    if (!visible_) {
        return;
    }
    const uint64_t now     = GetTickCount64();
    const uint64_t elapsed = now - phaseStartTick_;
    const uint32_t peak    = style_.peakOpacity;
    uint32_t alpha = 0;

    switch (phase_) {
    case Phase::FadeIn: {
        const uint32_t dur = (style_.fadeInMs > 0) ? static_cast<uint32_t>(style_.fadeInMs) : 0;
        if (dur == 0 || elapsed >= dur) {
            phase_ = Phase::Hold;
            phaseStartTick_ = now;
            alpha = peak;
            scale_ = 1.0f;         // 전체 크기로 안착하고...
            frameValid_ = false;   // ...Hold가 캐시할 수 있게 한 번 다시 그린다
        } else {
            const float p = static_cast<float>(elapsed) / static_cast<float>(dur);
            const float eased = 1.0f - (1.0f - p) * (1.0f - p);   // easeOutQuad
            scale_ = kAppearStartScale + (1.0f - kAppearStartScale) * eased;
            frameValid_ = false;   // 스케일이 매 틱 바뀐다 -> 다시 그린다
            alpha = static_cast<uint32_t>((static_cast<uint64_t>(peak) * elapsed) / dur);
        }
        break;
    }
    case Phase::Hold: {
        alpha = peak;
        if (sticky_) {
            // 상시 표시(Caps Lock): 안착한 프레임을 한 번 더 표시한 뒤 타이머를
            // 멈추고 Hide()까지 계속 보여준다.
            RenderAndPresent(static_cast<uint8_t>(peak > 255 ? 255 : peak));
            KillTimer(hwnd_, kTimer_OverlayFade);
            return;
        }
        const uint32_t dur = (style_.holdMs > 0) ? static_cast<uint32_t>(style_.holdMs) : 0;
        if (elapsed >= dur) {
            phase_ = Phase::FadeOut;
            phaseStartTick_ = now;
        }
        break;
    }
    case Phase::FadeOut: {
        const uint32_t dur = (style_.fadeOutMs > 0) ? static_cast<uint32_t>(style_.fadeOutMs) : 0;
        if (dur == 0 || elapsed >= dur) {
            Hide();
            return;
        }
        alpha = static_cast<uint32_t>(
            (static_cast<uint64_t>(peak) * (dur - elapsed)) / dur);
        break;
    }
    case Phase::Idle:
    default:
        KillTimer(hwnd_, kTimer_OverlayFade);
        return;
    }

    RenderAndPresent(static_cast<uint8_t>(alpha > 255 ? 255 : alpha));
}

void OverlayWindow::ComputePlacement() {
    if (gfx_ == nullptr) {
        return;
    }
    heightPx_ = ScaleForDpi(style_.pillHeight96, dpi_);
    if (heightPx_ < 8) {
        heightPx_ = 8;
    }

    // 캡슐 높이에 대한 비율로 정한 글리프 em. 이것은 그려진 글리프가 아니라 **폰트**
    // 크기임에 유의한다. 잉크가 덮는 영역은 가의 경우 em의 약 0.8이고 라틴 문자는
    // 그보다 작아서, 눈에 보이는 글리프는 결국 캡슐의 절반쯤이 된다.
    const float glyphSize = heightPx_ * 0.54f;

    gfx_->EnsureFactories();
    IDWriteTextFormat* gf = gfx_->GlyphFormat(glyphSize);

    // 폭 = 글리프 폭 + 좁은 가로 여백. 최소값은 살짝 가로로 긴 캡슐로 잡아서
    // 글리프 하나짜리가 넓적한 캡슐이 아니라 둥근 타원(~1.25:1)이 되게 한다.
    const float textW = MeasureTextWidth(gfx_->dwriteFactory, content_.glyph.c_str(),
                                         static_cast<UINT32>(content_.glyph.size()), gf);
    const int padX = static_cast<int>(heightPx_ * 0.42f);
    widthPx_ = static_cast<int>(textW + 0.5f) + padX * 2;
    const int minW = static_cast<int>(heightPx_ * 1.20f + 0.5f);
    if (widthPx_ < minW) {
        widthPx_ = minW;
    }

    // 최종 캡슐 기하에 맞춘 크기로 글리프 레이아웃을 (다시) 만든다.
    SafeRelease(gfx_->glyphLayout);
    gfx_->glyphLayout = MakeLayout(
        gfx_->dwriteFactory, content_.glyph.c_str(),
        static_cast<UINT32>(content_.glyph.size()), gf,
        static_cast<float>(widthPx_), static_cast<float>(heightPx_));

    // 캐럿 아래에, 캐럿의 가로 중점을 기준으로 가운데 정렬해 앵커한다. 모니터
    // 작업 영역을 벗어나면 위로 뒤집고, 그다음 작업 영역 안으로 제한한다.
    const int gap     = ScaleForDpi(style_.caretGap96, dpi_);
    const int caretCx = static_cast<int>((caret_.left + caret_.right) / 2);
    int x = caretCx - widthPx_ / 2;
    const int yBelow = static_cast<int>(caret_.bottom) + gap;
    const int yAbove = static_cast<int>(caret_.top) - gap - heightPx_;

    POINT anchor = { caretCx, static_cast<LONG>((caret_.top + caret_.bottom) / 2) };
    HMONITOR mon = MonitorFromPoint(anchor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    const bool haveWork = (mon != nullptr) && GetMonitorInfoW(mon, &mi) != FALSE;

    int y = yBelow;
    if (haveWork) {
        const RECT& wa = mi.rcWork;
        if (yBelow + heightPx_ > wa.bottom) {
            y = (yAbove >= wa.top) ? yAbove : (wa.bottom - heightPx_);
        }
        if (x < wa.left) {
            x = wa.left;
        }
        if (x + widthPx_ > wa.right) {
            x = wa.right - widthPx_;
        }
        if (x < wa.left) {   // 캡슐이 작업 영역보다 넓다: 왼쪽 가장자리에 붙인다
            x = wa.left;
        }
    }

    rc_.left   = x;
    rc_.top    = y;
    rc_.right  = x + widthPx_;
    rc_.bottom = y + heightPx_;
}

bool OverlayWindow::EnsureDeviceResources() {
    if (gfx_ == nullptr) {
        return false;
    }
    Gfx* g = gfx_;
    if (!g->EnsureFactories()) {
        return false;
    }
    if (!g->EnsureDib(widthPx_, heightPx_)) {
        return false;
    }
    if (!g->EnsureRenderTarget()) {
        return false;
    }
    if (!g->EnsureBrushes()) {
        return false;
    }
    return true;
}

void OverlayWindow::DiscardDeviceResources() {
    if (gfx_ != nullptr) {
        gfx_->DiscardDevice();
    }
    frameValid_ = false;
}

void OverlayWindow::RenderAndPresent(uint8_t globalAlpha) {
    if (hwnd_ == nullptr || !EnsureDeviceResources()) {
        return;
    }
    Gfx* g = gfx_;

    // DIB가 현재 프레임을 아직 담고 있지 않을 때만 그린다. 페이드 애니메이션은
    // ULW 전역 알파 외에는 아무것도 바꾸지 않으므로, ~15ms 틱은 곧바로 아래의
    // 표시 단계로 건너뛴다.
    if (!frameValid_) {
        // 렌더 타깃을 현재 DIB 영역에 바인딩한다(DIB 크기 변경이나 디바이스 리소스
        // 재생성 후에도 다시 바인딩된다).
        RECT bind = { 0, 0, widthPx_, heightPx_ };
        if (FAILED(g->rt->BindDC(g->memDc, &bind))) {
            DiscardDeviceResources();
            return;
        }

        // 모든 캡슐(가 / A / ⇪)에 하나의 통일된 외양: 한글 블루 채움, 얇고 더 밝은
        // 테두리, 흰 글리프 — macOS 표시기와 같다.
        // 색은 straight 알파다 — premultiplied 타깃이 그릴 때 미리 곱한다.
        const D2D1_COLOR_F blue    = D2D1::ColorF(0.04f, 0.52f, 1.00f, 0.96f);
        const D2D1_COLOR_F rim     = D2D1::ColorF(0.55f, 0.80f, 1.00f, 0.95f);
        const D2D1_COLOR_F white   = D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f);
        g->bgBrush->SetColor(blue);
        g->outlineBrush->SetColor(rim);
        g->glyphBrush->SetColor(white);

        const float w = static_cast<float>(widthPx_);
        const float h = static_cast<float>(heightPx_);

        g->rt->BeginDraw();
        // 등장 "팝": 캡슐 전체를 중심을 기준으로 스케일한다. Hold/FadeOut 동안에는
        // 1.0이므로 그 단계들은 캐시된 프레임을 재사용한다.
        g->rt->SetTransform(D2D1::Matrix3x2F::Scale(scale_, scale_,
                                                    D2D1::Point2F(w * 0.5f, h * 0.5f)));
        g->rt->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));   // 완전 투명

        // 테두리가 DIB 안에 머물도록 캡슐을 스트로크 폭의 절반만큼 안으로 들인다
        // (DrawRoundedRectangle은 스트로크를 가장자리 중앙에 맞춘다).
        const float strokeW = (h * 0.055f > 1.0f) ? h * 0.055f : 1.0f;
        const float m = strokeW * 0.5f + 0.5f;
        const float radius = (h - 2.0f * m) * 0.5f;   // 완전한 캡슐 모서리
        D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(
            D2D1::RectF(m, m, w - m, h - m), radius, radius);
        g->rt->FillRoundedRectangle(&rr, g->bgBrush);
        g->rt->DrawRoundedRectangle(&rr, g->outlineBrush, strokeW);

        if (g->glyphLayout != nullptr) {
            g->rt->DrawTextLayout(D2D1::Point2F(0.0f, 0.0f), g->glyphLayout, g->glyphBrush);
        }

        HRESULT hr = g->rt->EndDraw();
        if (hr == D2DERR_RECREATE_TARGET) {
            DiscardDeviceResources();   // 다음 표시 때 재생성된다
            return;
        }

        GdiFlush();   // ULW가 DIB를 읽기 전에 GDI 배치를 비운다
        frameValid_ = true;
    }

    POINT ptSrc = { 0, 0 };
    POINT ptDst = { rc_.left, rc_.top };
    SIZE  size  = { widthPx_, heightPx_ };
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, globalAlpha, AC_SRC_ALPHA };
    UpdateLayeredWindow(hwnd_, nullptr, &ptDst, &size, g->memDc, &ptSrc, 0, &bf,
                        ULW_ALPHA);
}

} // namespace imi
