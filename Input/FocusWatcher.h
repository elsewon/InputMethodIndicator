// FocusWatcher.h — "입력 포커스가 이동했다"를 UI Automation으로 보고한다.
//
// WinEvent(EVENT_OBJECT_FOCUS) 훅이 아니라 UIA 구독을 쓰는 이유: Win11의 XAML
// 셸 표면은 자기 컨트롤 사이에서 포커스가 옮겨갈 때 WinEvent를 올리지 않는다.
// 탐색기가 명확한 사례로, 주소 표시줄에서 검색 상자로 넘어가는 동작은 완전히
// 무음이다. 그래서 훅 기반 구현은 정작 표시기가 필요한 지점에서 죽어버린다.
// UIA는 그런 이동을 보고하고 레거시 컨트롤도 프록시해 주므로, 혼자서 양쪽
// 세계를 모두 감당한다.
//
// 스레딩: Start/Stop은 호스트 UI 스레드에서 부르지만, 구독을 걸고 푸는 일은
// 이 클래스가 소유한 MTA 작업자 스레드에서 실행된다.
// AddFocusChangedEventHandler는 시스템 전역의 공급자들을 구체화하는 무거운
// 호출이라 병든 explorer에서는 수 분까지 걸릴 수 있는데(실측 337초), UI
// 스레드는 WH_KEYBOARD_LL 훅을 소유하므로 거기서 블로킹하면 시스템 전역
// 입력이 언다. UIA는 콜백을 자기 스레드 풀 스레드에서 전달하므로 핸들러는
// 어떤 상태도 건드리지 않는다. PostMessage만 하며, 그것이 곧 UI 스레드로의
// 마샬링이다.
#pragma once

#include <windows.h>

namespace imi {

struct FocusWatcherImpl;   // PIMPL — .cpp의 네임스페이스 스코프에 정의된다.

class FocusWatcher {
public:
    FocusWatcher() = default;
    ~FocusWatcher();

    FocusWatcher(const FocusWatcher&) = delete;
    FocusWatcher& operator=(const FocusWatcher&) = delete;

    // 작업자 스레드를 시작한다. 작업자가 UIA 포커스 변경을 구독하며, 변경이
    // 있을 때마다 |notifyWnd|가 |notifyMsg|를 받는다(매개변수 없음 — 호스트가
    // 직접 포커스를 다시 읽는다). 즉시 반환하며, 작업자 스레드를 만들지 못한
    // 경우에만 false를 반환한다 — 구독 자체의 성패는 비동기이며 로그로
    // 보고된다(소요 시간 포함). 이미 실행 중일 때 호출해도 안전하다(아무 일도
    // 하지 않는다).
    bool Start(HWND notifyWnd, UINT notifyMsg);
    void Stop();

    bool Running() const { return impl_ != nullptr; }

private:
    FocusWatcherImpl* impl_ = nullptr;
};

} // namespace imi
