// CaretResolver.h — 포커스된 컨트롤의 텍스트 캐럿 사각형을 프로세스 경계를
// 넘어, 앱별 예외 처리(quirk)를 갖춘 순서 있는 대체 체인으로 알아낸다.
//
// 체인:
//   1. GetGUIThreadInfo(tid).rcCaret + hwndCaret -> ClientToScreen  (Win32 EDIT/RichEdit)
//   2. MSAA 시스템 캐럿(OBJID_CARET) -> accLocation. 포커스된 앱이 1단계가
//      놓친 진짜 시스템 캐럿을 유지하고 있을 때만 답한다. 답한다면 그것이 곧
//      캐럿이므로 결코 잘못된 위치를 줄 수 없다. 호출 한 번으로 끝난다.
//   3. IAccessible2 캐럿 소유자                                     (Chromium/Electron)
//      UIA 단계와 중복이 아니다. Chromium은 비어 있는 필드의 플레이스홀더를
//      문서 텍스트로 노출하고 UIA 캐럿을 그 끝에 보고하는데, 이는 텍스트 끝에
//      있는 진짜 캐럿과 구별되지 않는다(ValuePattern도 플레이스홀더를 반환하고,
//      IsReadOnly/IsHidden도 다르지 않다. 테마에 따라 달라지는 전경색만이
//      다르다). 진실은 오직 IA2 캐럿 소유권(OWNERSHIP)을 통해서만 드러난다.
//      get_caretOffset은 실제로 캐럿을 쥐고 있는 노드에서만 S_OK로 답한다.
//      자손이 소유자라면 -> 그 characterExtents가 곧 캐럿이다. 어떤 자손도
//      소유하지 않고 컨테이너만 소유를 주장한다면 -> 그 내용은 장식
//      (플레이스홀더)이며, 입력은 첫 자식의 왼쪽(LEFT) 가장자리에서 시작된다.
//   4a. UIA TextPattern2::GetCaretRange -> 바운딩 사각형             (UWP/WinUI/Terminal)
//   4b. 삽입 지점 앞의 텍스트를 측정한다. 그 줄을 잡아 선택 시작 지점까지
//       잘라내고, 남은 부분의 오른쪽(RIGHT) 가장자리를 쓴다. Win11 XAML
//       상자(탐색기 주소 표시줄)가 동작하게 만드는 것이 바로 이것이다 — 이들은
//       degenerate range에 대해 사각형을 아예 하나도 반환하지 않으므로, 캐럿은
//       그 앞의 텍스트 런을 측정하는 간접적인 방법으로만 위치를 알아낼 수 있다.
//       (XAML은 MSAA 캐럿도 IA2도 노출하지 않는다. 2-3단계와 4단계가 모두
//       존재하는 이유가 이것이다.)
//   5. UIA 포커스 요소의 CurrentBoundingRectangle을 왼쪽(LEFT) 끝에 앵커링
//      (포커스가 텍스트 컨트롤이 전혀 아닌 대상에 있는 경우)
//
// 이 체인은 캐럿을 찾아내거나, 아무것도 보고하지 않는다. 위치를 잡을 수 없는
// 캡슐은 아예 표시하지 않는다. 사용자가 입력하고 있지 않은 곳에 그려진 캡슐은
// 캡슐이 없는 것보다 나쁘기 때문이다.
//
// 1단계만 호출자의 (UI) 스레드에서 인라인으로 실행된다 — GetGUIThreadInfo는
// 대상의 협조가 필요 없는 커널 호출이라 진짜로 값싸다. 2단계부터는 전부 전용
// MTA 작업자 스레드에서 실행된다. MSAA/IA2 단계도 겉보기와 달리 프로세스 간
// 동기 COM이다: 포커스된 앱의 UI 스레드가 응답해야 돌아오고 그 대기에는
// 타임아웃이 없으므로, 바쁘거나 멈춘 앱이 우리 UI 스레드(와 그것이 소유한 LL
// 키보드 훅 — 시스템 전역 입력)를 얼릴 수 있다. UIA 호출 역시 예열되지 않은
// 공급자를 상대로 수백 밀리초가 걸릴 수 있다.
//
// 따라서 그 단계들은 **비동기**다. 호출자는 요청을 큐에 넣고 메시지 루프로
// 돌아가며, 작업자 스레드는 답이 준비되면 완료 메시지를 post한다. 대신 작업자를
// 기다린다면 해석이 끝날 때까지 UI 스레드가 통째로 멈추어 — 오버레이 애니메이션이
// 지연되고, 더 나쁘게는 큐에 쌓인 WinEvent가 유실될 수 있다.
//
// 반환되는 사각형은 물리(PHYSICAL) 화면 픽셀이다.
//
// UI 스레드에서의 사용법:
//     CaretResult r;
//     if (caret.TryFast(fg, tid, r))  -> 지금 r을 쓴다 (살아 있는 Win32 캐럿)
//     else if (id = caret.RequestAsync(fg, tid)) -> 통지 메시지를 기다린 뒤
//                                                   caret.TakeResult(id, r)
//     else                            -> 표시할 것이 없다
#pragma once

#include <windows.h>
#include <cstdint>

namespace imi {

enum class CaretMethod : int {
    None = 0,
    GuiThreadInfo = 1,
    MsaaCaret = 2,       // 진짜 시스템 캐럿이 있었다 (있다면 그것이 정답이다)
    Ia2Caret = 3,        // IAccessible2 캐럿 소유자 (Chromium/Electron)
    UiaTextPattern = 4,
    UiaFocusedElement = 5,
};

struct CaretResult {
    RECT        rect = {0,0,0,0};   // 물리 화면 픽셀
    CaretMethod method = CaretMethod::None;
    uint32_t    dpi = 96;           // 캐럿이 놓인 모니터의 유효 DPI
    bool        valid = false;      // 배치 위치를 찾기는 했는지 여부
    // 포커스가 텍스트를 받아들이는 대상에 있다. |method|와 무관하게 판정된다 —
    // 비어 있는 텍스트 상자는 찾을 캐럿도, 측정할 텍스트도 없지만 여전히 텍스트
    // 입력이다(TextPattern / Edit 컨트롤 형식을 보고한다).
    bool        textInput = false;
    // 체인 전체가 걸린 실제 시간. 느린 공급자(타임아웃, 즉 캡슐이 뜨지 않는
    // 형태로 드러난다)를 진단할 수 있도록 로그에 남긴다.
    uint32_t    elapsedMs = 0;
};

class CaretResolver {
public:
    CaretResolver() = default;
    ~CaretResolver();

    // UIA MTA 작업자 스레드를 시작하고 접근성을 깨운다(UIA 클라이언트를 생성해
    // Chromium/Electron이 접근성 트리를 활성화하도록 한다). 큐에 넣은 해석이
    // 끝나면 |notifyWnd|가 wParam = 요청 id로 |notifyMsg|를 받는다.
    // 작업자를 시작하지 못하면 false를 반환한다(작업자가 없어도 TryFast는 동작한다).
    bool Start(HWND notifyWnd, UINT notifyMsg);
    void Stop();

    // 1단계만: Win32 캐럿(GetGUIThreadInfo). 대상의 협조가 필요 없는 커널
    // 호출이라 무언가를 큐에 넣기 전에 공짜로 시도해 볼 수 있다. 답하면
    // true를 반환한다(|out|이 완성된다).
    bool TryFast(HWND foreground, DWORD threadId, CaretResult& out);

    // |foreground|(의 포커스된 자손)에 대한 나머지 체인(MSAA -> IA2 -> UIA)을
    // 큐에 넣는다. 0이 아닌 요청 id를 반환하며, 작업자가 없으면 0을 반환한다.
    // 절대 블로킹하지 않는다. 가장 최신 요청만 수령할 수 있다 — 이전 요청은
    // 대체되고 그 답은 버려진다.
    uint64_t RequestAsync(HWND foreground, DWORD threadId);

    // |id|에 대해 끝난 답을 수령한다. 그 요청이 대체되었거나, 아직 완료되지
    // 않았거나, 아무것도 찾지 못했으면 false다. 통지 메시지를 받은 뒤 UI
    // 스레드에서 호출한다.
    bool TakeResult(uint64_t id, CaretResult& out);

private:
    struct Worker;               // MTA 스레드 + UIA 클라이언트. .cpp에 정의된다
    Worker* worker_ = nullptr;

    // 1단계: Win32 캐럿. 값싸고, 동기적이며, UI 스레드에서 실행된다.
    bool TryGuiThreadInfo(HWND foreground, DWORD threadId, CaretResult& out);
    // 2단계: MSAA 시스템 캐럿(OBJID_CARET). 프로세스 간 동기 COM — 작업자
    // 스레드에서 실행된다(정적: 인스턴스 상태를 쓰지 않는다).
    static bool TryMsaaCaret(HWND focus, CaretResult& out);
    // 3단계: IA2 캐럿 소유자 — UIA와 나란히 이것이 존재하는 이유는 파일 상단의
    // 설명을 보라. 프로세스 간 동기 COM — 작업자 스레드에서 실행된다.
    static bool TryIa2Caret(HWND focus, CaretResult& out);
    // 포커스된 프로세스의 exe + 클래스를 키로 한 앱별 오프셋/예외 처리(quirk) 보정.
    void ApplyPerAppQuirks(HWND focus, CaretResult& io);
    // 공통 마무리: 예외 처리(quirk), 모니터 DPI, 유효성, 경과 시간.
    void Finalise(HWND focus, uint64_t startTick, CaretResult& io);
};

} // namespace imi
