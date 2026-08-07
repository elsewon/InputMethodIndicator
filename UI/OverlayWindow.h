// OverlayWindow.h — 캐럿 근처에 뜨는 표시기 캡슐.
//
// 픽셀 단위 알파를 쓰는 레이어드 윈도우(WS_EX_LAYERED|TRANSPARENT|NOACTIVATE|
// TOPMOST|TOOLWINDOW)로, GDI 호환 DC 렌더 타깃에 Direct2D로 그린 뒤
// UpdateLayeredWindow로 표시한다. 포커스도 입력도 절대 가져가지 않는다.
// 자체 소유한 WM_TIMER가 페이드인 -> 홀드 -> 페이드아웃을 구동한다. 애니메이션이
// 진행 중일 때 Show()를 다시 호출하면 깔끔하게 재시작한다(앵커 재설정 + 타임라인
// 초기화).
//
// DPI: 렌더 타깃 DPI는 96으로 고정하고 기하는 직접 스케일한다 — 그렇게 하지
// 않으면 DC 렌더 타깃이 96이 아닌 모니터에서 캡슐을 이중으로 스케일한다.
// 모든 캐럿 좌표는 물리 화면 픽셀이다.
#pragma once

#include <windows.h>
#include <cstdint>
#include <string>

namespace imi {

// 호출자가 Config에서 결정하는 시각 스타일. 모든 캡슐은 상태와 무관하게 한 가지
// 색(한글 블루)을 쓰므로 상태별 강조 플래그는 여기에 없다.
struct OverlayStyle {
    int     pillHeight96 = 24;
    int     caretGap96   = 8;
    int     holdMs       = 650;
    int     fadeInMs     = 130;
    int     fadeOutMs    = 260;
    uint8_t peakOpacity  = 235;   // 0..255
};

// 무엇을 그릴지. 가운데 정렬된 글리프 하나 — 가 / A / ⇪ — 로, macOS의 캐럿 근처
// 표시기와 동일하다(보조 캡션 줄은 없다).
struct OverlayContent {
    std::wstring glyph;    // 예: L"가", L"A", L"⇪"
};

class OverlayWindow {
public:
    OverlayWindow() = default;
    ~OverlayWindow();

    bool Create(HINSTANCE hinst);
    void Destroy();

    // |dpi|인 모니터에서 |caretRectPhysical|(화면 px)에 앵커를 맞춰 캡슐을 표시하거나
    // 갱신한다. 배치는 캐럿 아래이며 모니터 작업 영역 안으로 제한된다. 페이드
    // 타임라인을 재시작한다. |sticky|가 true면 캡슐이 팝인한 뒤 Hide()까지 무기한
    // 유지된다(페이드아웃 없음) — 상시 표시되는 Caps Lock 표시기에 쓴다.
    // 스레드 친화성: UI 스레드 전용.
    void Show(const OverlayContent& content, const RECT& caretRectPhysical,
              uint32_t dpi, const OverlayStyle& style, bool sticky = false);

    // 즉시 숨긴다(포커스 상실 / 데스크톱 전환 / 비활성화 시 사용).
    void Hide();

    bool IsVisible() const { return visible_; }
    HWND Hwnd() const { return hwnd_; }

private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT HandleMessage(UINT, WPARAM, LPARAM);

    bool EnsureDeviceResources();
    void DiscardDeviceResources();
    // 현재 프레임을 |globalAlpha|로 UpdateLayeredWindow를 통해 표시한다. DIB에
    // 대한 D2D 드로우는 프레임이 더러울 때만(새 Show, 디바이스 재생성) 수행하고,
    // 페이드 틱은 캐시된 DIB를 새 알파로 다시 표시하기만 한다.
    void RenderAndPresent(uint8_t globalAlpha);
    void ComputePlacement();                     // 캐럿 + 모니터로 rc_를 채운다
    void OnFadeTimer();

    HINSTANCE hinst_ = nullptr;
    HWND      hwnd_ = nullptr;
    bool      visible_ = false;

    // 현재 요청/기하.
    OverlayContent content_;
    OverlayStyle   style_;
    RECT           caret_ = {0,0,0,0};   // 물리 화면 px
    uint32_t       dpi_ = 96;
    RECT           rc_ = {0,0,0,0};      // 윈도우 사각형(화면 px)
    int            widthPx_ = 0, heightPx_ = 0;

    // 애니메이션 타임라인(표시 시작 이후 ms, 틱 차이로 추적). 등장 단계는 알파
    // 페이드와 함께 스케일 "팝"(scale_ 0.x -> 1.0)을 애니메이션하며 macOS와 같다.
    // Hold/FadeOut은 scale_ == 1.0을 유지하므로 캐시된 프레임을 재사용하고 레이어드
    // 윈도우의 전역 알파만 바꾼다.
    enum class Phase { Idle, FadeIn, Hold, FadeOut };
    Phase    phase_ = Phase::Idle;
    uint64_t phaseStartTick_ = 0;
    float    scale_ = 1.0f;         // RenderAndPresent에서 적용되는 현재 팝 스케일
    bool     frameValid_ = false;   // DIB가 현재 내용/기하/스케일을 담고 있다
    bool     sticky_ = false;       // 페이드인 후 최대값에서 유지(페이드아웃 없음)

    // 불투명 디바이스 리소스 보관자(D2D를 헤더 밖에 두려고 .cpp에 정의한다).
    // 소유하며, 지연 생성하고 디바이스 손실 시 폐기한다.
    struct Gfx;
    Gfx* gfx_ = nullptr;
};

} // namespace imi
