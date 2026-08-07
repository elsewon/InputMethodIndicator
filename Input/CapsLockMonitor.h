// CapsLockMonitor.h — WH_KEYBOARD_LL을 통한 전역 키보드 관찰자.
//
// 주 임무: Caps Lock 토글 감지(트레이 입력 표시기는 Caps 상태를 담지 않는다).
// 같은 훅에서 수행하는 부차적 임무: 언어 전환 키(한/영, VK_IME_ON/OFF,
// Win+Space)를 마지막으로 누른 시점을 기록한다 — 상태 기계는 그 타임스탬프로
// 진짜 한/영 토글과 포어그라운드 유예 구간 안에서 일어나는 트레이의 앱 전환
// 동기화를 구별한다.
//
// 저수준 키보드 훅은 UI 스레드에 설치되고 그 프로시저는 같은 스레드의 메시지
// 큐에서 디스패치되므로, 여기의 모든 상태는 UI 스레드 상태다. 프로시저는
// 사소한 수준을 유지한다(할당 없음, 블로킹 없음): 지연되는 LL 훅은 Windows가
// 조용히 제거해 버린다.
#pragma once

#include <windows.h>
#include <functional>
#include <cstdint>

namespace imi {

class CapsLockMonitor {
public:
    using Callback = std::function<void(bool capsOn)>;

    CapsLockMonitor() = default;
    ~CapsLockMonitor();

    // 훅을 설치한다. 메시지 루프를 돌리는 스레드(UI 스레드)에서 호출해야 한다.
    // SetWindowsHookEx가 실패하면 false를 반환한다.
    bool Start(HINSTANCE hinst, Callback cb);
    void Stop();

    // 최선 노력(best-effort) 토글 상태로, 시작 시 기준값을 잡는 데에만 쓴다.
    // 주의: GetKeyState는 **호출 스레드의 입력 큐**를 기준으로 답을 내는데 이
    // 프로세스는 키보드 포커스를 가진 적이 없으므로, 실시간 토글을 판단하는 데
    // 써서는 **안 된다** — 훅의 엣지 추적이 진실의 원천이다.
    static bool CurrentState();

    // 훅이 관측한 마지막 언어 전환 키 입력의 틱(GetTickCount64). 아직 없으면 0.
    // UI 스레드 상태다(훅이 그 스레드에서 디스패치된다).
    static uint64_t LastLanguageKeyTick();

    // 마지막 "타이핑" 키다운의 틱 — Caps Lock과 단독 수정자 키를 제외한 모든 키.
    // 그런 키가 처음 눌리기 전까지는 0이다. caps 표시기의 타이핑 중 숨김 /
    // 유휴 시 재등장 동작을 좌우한다.
    static uint64_t LastTypingKeyTick();

private:
    static LRESULT CALLBACK LowLevelProc(int code, WPARAM wParam, LPARAM lParam);

    HHOOK    hook_ = nullptr;
    Callback callback_;
};

} // namespace imi
