// TrayIndicatorWatcher.h — Windows 11 트레이 입력 표시기(가/A)를 UI Automation으로
// 감시하고 현재 입력 언어를 보고한다.
//
// 이것이 이 앱의 상태 원천이다. 표시기는 Shell_TrayWnd 아래의 UIA 요소로,
// 접근성 Name이 한/영 토글마다 "…한국어 입력 모드"/"…영어 입력 모드"(현지화됨)
// 사이를 오가며 Name 속성 변경 이벤트를 올린다. 입력기가 2개 이상이면 입력기
// 전환 칩("…한국어\nMicrosoft 입력기…")도 존재하며, 그 rename은 입력기
// 전환(methodSwitch)으로 보고된다 — 알림 메시지의 lParam이 1이다. 이때 보이는
// 언어가 같더라도(입력기 왕복 후 IME는 A 모드로 리셋된다) 호스트는 캡슐을 띄운다.
//
// 스레딩: Start/Stop은 호스트 UI 스레드에서 부르지만, 실제 UIA 작업(클라이언트
// 생성, 표시기 탐색, 구독, 재검증)은 전부 이 클래스가 소유한 MTA 작업자
// 스레드에서 실행된다. 탐색/재검증은 explorer를 상대로 한 동기 프로세스 간
// 호출이라 수백 ms~수 초씩 걸릴 수 있는데(표시기가 없는 기계에서는 트레이 전체
// 깊이 우선 탐색이 매번 실패를 반복한다), 호스트 UI 스레드는 WH_KEYBOARD_LL 훅과 트레이
// 메뉴를 소유하므로 거기서 블로킹하면 시스템 전역 입력 지연과 메뉴 멈춤이 된다.
// UIA는 속성 변경 콜백을 **자기 자신의** 스레드 풀 스레드에서 전달하므로,
// 핸들러는 우리 상태를 전혀 건드리지 않는다. 이벤트 VARIANT에서 새 Name을
// 곧바로 파싱해 그 결과를 호스트 윈도우로 PostMessage할 뿐이다. 그 PostMessage가
// 곧 UI 스레드로의 마샬링 단계이며, 핸들러 자체는 UIA를 호출하지 않고 절대
// 블로킹하지 않는다.
#pragma once

#include <windows.h>
#include "InputState.h"

namespace imi {

struct TrayWatcherImpl;   // PIMPL — .cpp의 네임스페이스 스코프에 정의된다.

// 감시자가 호스트로 보고하는 건강 상태 전이(healthMsg의 wParam). 표시기가 정상적
// 으로 부재한 경우(입력기 1개, 표시기 숨김)는 탐색이 빠르게 실패하므로 이 신호를
// 내지 않는다 — Unreadable은 탐색이 수 초씩 걸리며 지속 실패하는 이상 상태
// (대개 explorer 접근성 트리 이상)에서만 나온다.
enum class TrayWatcherHealth : uint8_t {
    Recovered  = 0,   // 오랜 실패 뒤 표시기를 다시 찾았다
    Unreadable = 1,   // 표시기를 지속적으로 읽지 못한다(사용자 조치 필요)
};

class TrayIndicatorWatcher {
public:
    TrayIndicatorWatcher() = default;
    ~TrayIndicatorWatcher();

    TrayIndicatorWatcher(const TrayIndicatorWatcher&) = delete;
    TrayIndicatorWatcher& operator=(const TrayIndicatorWatcher&) = delete;

    // 작업자 스레드를 시작한다. 작업자는 UIA 클라이언트를 만들고, 최선의 표시기
    // 칩을 찾아 초기 언어를 post하고, 트레이 루트에 서브트리 Name 구독을 건다.
    // 변경이 있을 때마다(최초 읽기 포함) |notifyWnd|가 wParam = (WPARAM)Language,
    // lParam = methodSwitch(0/1)와 함께 |langMsg|를 받는다. 작업자의 저빈도
    // 재검증 틱은 explorer 재시작 이후 구독을 다시 걸고 최선 칩을 신선하게 다시
    // 찾아 상태 표류를 바로잡으며, 표시기가 계속 없는 동안에는 백오프한다.
    // 표시기를 오래 읽지 못하는 이상 상태에 들어가거나 거기서 회복되면
    // |notifyWnd|가 wParam = (WPARAM)TrayWatcherHealth로 |healthMsg|를 받는다
    // (전이당 한 번, 사용자에게 트레이 풍선 알림으로 surface하기 위함).
    // 즉시 반환하며, 작업자 스레드를 만들지 못한 경우에만 false를 반환한다
    // (요소는 다시 확인될 때까지 정당하게 없을 수 있다).
    bool Start(HWND notifyWnd, UINT langMsg, UINT healthMsg);
    void Stop();

    bool IndicatorFound() const;

private:
    TrayWatcherImpl* impl_ = nullptr;
};

} // namespace imi
