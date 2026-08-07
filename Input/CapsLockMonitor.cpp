// CapsLockMonitor.cpp — 전역 Caps Lock + 언어 키 관찰자(WH_KEYBOARD_LL).
//
// 훅은 UI 스레드에 설치되고 그 LL 프로시저는 같은 스레드의 메시지 큐에서
// 디스패치된다. 프로시저는 사소한 수준을 유지하고 절대 블로킹해서는 안 된다 —
// 약 300ms 넘게 지연되는 LL 훅은 Windows가 조용히 제거한다 — 그래서 우리가
// 추적하는 토글을 뒤집거나 틱을 기록하고 (post만 하는) 콜백을 부른 뒤, 항상
// CallNextHookEx를 호출하는 일만 한다.
//
// 프로시저 안에서 다시 조회하는 대신 CurrentState() 기준값에서 출발해 **우리
// 자신의** Caps 토글을 추적한다(훅이 호출되는 시점에는 OS가 아직 토글을
// 적용하지 않았다). 그리고 키의 물리적 up→down 전환을 지켜보는 방식으로 자동
// 반복을 감지한다(KBDLLHOOKSTRUCT에는 반복 횟수가 없다). 훅이 볼 수 없는
// 토글(UIPI 아래 권한 상승된 포커스, RDP 동기화)은 나중에 ResyncTrackedState로
// 바로잡는다 — App::OnCapsLock / App::OnForeground 참고.
#include "CapsLockMonitor.h"

#include "Log.h"

// VK_IME_ON/OFF는 최신 SDK 헤더에 등장했다. 구버전을 위해 직접 정의한다.
#ifndef VK_IME_ON
#define VK_IME_ON  0x16
#endif
#ifndef VK_IME_OFF
#define VK_IME_OFF 0x1A
#endif

namespace imi {

namespace {

// LL 키보드 훅은 한 번에 하나만 살아 있다. C 스타일 프로시저는 이 파일 스코프
// 포인터를 통해 자신의 소유자에게 닿는다. 토글/엣지 상태도 여기에 산다
// (프로시저 상태는 호출 사이에 유지되어야 하고 구조상 단일 인스턴스다).
// 전부 UI 스레드 상태다 — 훅은 설치한 스레드에서 디스패치된다.
CapsLockMonitor* s_self     = nullptr;
bool             s_capsOn   = false;  // 우리가 추적하는 토글 상태
bool             s_capsDown = false;  // 물리 키가 현재 눌린 상태(반복 방지용)
bool             s_winDown  = false;  // Win 키가 눌린 상태(Win+Space 감지용)
uint64_t         s_lastLangKeyTick = 0;
uint64_t         s_lastTypingKeyTick = 0;

// caps 표시기 기준으로 단독 수정자 / 토글 키다운은 "타이핑"이 아니다.
bool IsNonTypingKey(DWORD vk) {
    switch (vk) {
        case VK_CAPITAL:
        case VK_LSHIFT: case VK_RSHIFT: case VK_SHIFT:
        case VK_LCONTROL: case VK_RCONTROL: case VK_CONTROL:
        case VK_LMENU: case VK_RMENU: case VK_MENU:
        case VK_LWIN: case VK_RWIN:
            return true;
        default:
            return false;
    }
}

} // namespace

CapsLockMonitor::~CapsLockMonitor() {
    Stop();
}

bool CapsLockMonitor::CurrentState() {
    return (GetKeyState(VK_CAPITAL) & 1) != 0;
}

uint64_t CapsLockMonitor::LastLanguageKeyTick() {
    return s_lastLangKeyTick;
}

uint64_t CapsLockMonitor::LastTypingKeyTick() {
    return s_lastTypingKeyTick;
}

bool CapsLockMonitor::Start(HINSTANCE hinst, Callback cb) {
    if (hook_) return true;  // 이미 실행 중

    callback_  = std::move(cb);
    s_capsOn   = CurrentState();
    s_capsDown = false;
    s_winDown  = false;
    s_self     = this;

    // WH_KEYBOARD_LL은 전역 훅이다. hmod는 유효한 모듈 핸들이면 무엇이든 되고
    // 대상 스레드 id는 0(시스템 전역)이다. 디스패치는 **이** 스레드에서 일어난다.
    hook_ = SetWindowsHookExW(WH_KEYBOARD_LL, &LowLevelProc, hinst, 0);
    if (!hook_) {
        const DWORD err = GetLastError();
        IMI_ERROR(L"CapsLockMonitor: SetWindowsHookExW(WH_KEYBOARD_LL) 실패, gle=%lu", err);
        s_self = nullptr;
        callback_ = nullptr;
        return false;
    }
    return true;
}

void CapsLockMonitor::Stop() {
    if (hook_) {
        UnhookWindowsHookEx(hook_);
        hook_ = nullptr;
    }
    if (s_self == this) s_self = nullptr;
    callback_ = nullptr;
}

LRESULT CALLBACK CapsLockMonitor::LowLevelProc(int code, WPARAM wParam, LPARAM lParam) {
    if (code == HC_ACTION && s_self) {
        const KBDLLHOOKSTRUCT* kb = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam);
        if (kb) {
            const bool isDown = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
            const bool isUp   = (wParam == WM_KEYUP   || wParam == WM_SYSKEYUP);
            if (isDown && !IsNonTypingKey(kb->vkCode)) {
                s_lastTypingKeyTick = GetTickCount64();
            }
            switch (kb->vkCode) {
            case VK_CAPITAL:
                if (isDown) {
                    // up→down 엣지만 인정한다. 키를 누르고 있을 때의 자동 반복은 무시.
                    if (!s_capsDown) {
                        s_capsDown = true;
                        s_capsOn   = !s_capsOn;
                        if (s_self->callback_) s_self->callback_(s_capsOn);
                    }
                } else if (isUp) {
                    s_capsDown = false;
                }
                break;

            // 한/영(VK_HANGUL == VK_KANA)과 명시적인 IME on/off 키는 유예 구간
            // 재정의를 위한 의도적 언어 전환으로 표시된다.
            case VK_HANGUL:
            case VK_IME_ON:
            case VK_IME_OFF:
                if (isDown) s_lastLangKeyTick = GetTickCount64();
                break;

            case VK_LWIN:
            case VK_RWIN:
                if (isDown)    s_winDown = true;
                else if (isUp) s_winDown = false;
                break;

            case VK_SPACE:
                // Win+Space는 입력기를 순환시킨다 — 이 또한 의도적인 전환이다.
                if (isDown && s_winDown) s_lastLangKeyTick = GetTickCount64();
                break;

            default:
                break;
            }
        }
    }
    // 입력 체인을 절대 막지 않는다.
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

} // namespace imi
