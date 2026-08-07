// InputStateMachine.cpp — InputStateMachine.h 참고. 그대로 두면 오버레이를
// 번쩍이게 만들 잡음을 억제한다: 시작 시 동기화, 중복 상태, 빠른 반복, 그리고 앱
// 전환 시 트레이의 창별 동기화. 모든 피드 지점은 UI 스레드에서 실행되므로
// (트레이 감시자의 UIA 콜백은 이미 여기로 마셜링되었다) 잠금이 필요 없다.
#include "InputStateMachine.h"

#include <windows.h>   // GetTickCount64

namespace imi {

uint64_t InputStateMachine::Now() const {
    return (clockOverride_ != nullptr) ? clockOverride_() : GetTickCount64();
}

void InputStateMachine::SetGates(bool notifyLanguage, bool notifyCapsLock) {
    notifyLanguage_ = notifyLanguage;
    notifyCapsLock_ = notifyCapsLock;
}

bool InputStateMachine::WithinDebounce(uint64_t nowTick) const {
    // 부호 없는 뺄셈이다. lastEmitTick_==0(한 번도 방출한 적 없음)이면 절대
    // debounceMs_보다 작지 않은 거대한 차이가 나오므로 첫 방출은 항상 통과한다.
    return (nowTick - lastEmitTick_) < static_cast<uint64_t>(debounceMs_);
}

void InputStateMachine::EmitIfGated(IndicatorKind kind) {
    if (kind == IndicatorKind::LanguageChanged && !notifyLanguage_) return;
    if ((kind == IndicatorKind::CapsLockOn || kind == IndicatorKind::CapsLockOff) && !notifyCapsLock_) return;

    const uint64_t now = Now();
    // 디바운스 구간 안에서는 완전히 동일한(IDENTICAL) 반복(같은 종류이면서 보이는
    // 상태도 같음)만 버린다. 상태가 다르면 반드시 표시해야 하므로, 빠른 가→A→가
    // 연타는 실제와 일치하는 글리프로 끝난다.
    if (kind == lastEmitKind_ && current_.SameVisibleState(lastEmitState_) &&
        WithinDebounce(now)) {
        return;
    }

    lastEmitKind_  = kind;
    lastEmitState_ = current_;
    lastEmitTick_  = now;

    if (emit_) {
        IndicatorDecision decision;
        decision.kind  = kind;
        decision.state = current_;
        emit_(decision);
    }
}

void InputStateMachine::OnLanguage(Language lang, uint64_t lastLangKeyTick,
                                   bool methodSwitch) {
    InputStateSnapshot next = current_;
    next.language      = lang;
    next.imeOpen       = IsNativeLanguage(lang);
    next.timestampTick = Now();

    // 시작 후 첫 보고는 조용히 기준선을 세운다. 감시자가 표시기를 늦게(실행 후 몇
    // 초 뒤에) 찾는 경우도 이것으로 처리된다.
    if (!haveLanguageBaseline_) {
        current_ = next;
        haveLanguageBaseline_ = true;
        return;
    }

    // 보이는 변화 없음(imeOpen은 language에서 파생되므로 language만 비교해도
    // 충분하다) — 메타데이터만 갱신하고 방출하지 않는다. 입력기 전환은 예외다:
    // 글리프가 같더라도(예: 영어 자판 → A 모드로 리셋된 한국어 IME) 사용자는
    // 방금 입력기를 바꿨고, 지금 상태를 확인해 주는 캡슐을 기대한다.
    if (current_.language == lang && !methodSwitch) {
        current_ = next;
        return;
    }

    current_ = next;

    // 앱 전환 유예 구간 안에서 트레이가 새 창의 모드로 동기화하는 것은 사용자가 한
    // 전환이 아니다 — 방금 언어 전환 키가 눌린 경우는 예외이며, 그때는 반드시
    // 표시해야 하는 진짜 전환으로 표시된다. 입력기 전환 보고는 구조상 사용자
    // 행동에서만 나오므로(트레이 전환 칩은 입력기가 실제로 바뀔 때만 rename)
    // 유예 검사를 건너뛴다.
    if (!methodSwitch) {
        const uint64_t now = Now();
        if (now < foregroundGraceUntil_) {
            const bool realToggle = (lastLangKeyTick != 0) &&
                                    (now - lastLangKeyTick) <= kLangKeyOverrideMs;
            if (!realToggle) {
                return;
            }
        }
    }
    EmitIfGated(IndicatorKind::LanguageChanged);
}

void InputStateMachine::OnCapsLock(bool capsOn) {
    const bool changed = (current_.capsLock != capsOn);
    current_.capsLock      = capsOn;
    current_.timestampTick = Now();
    if (!changed) {
        return;   // 현재 상태를 확인해 주는 재동기화 — 보이는 변화는 없었다
    }
    EmitIfGated(capsOn ? IndicatorKind::CapsLockOn : IndicatorKind::CapsLockOff);
}

void InputStateMachine::OnForegroundChanged() {
    foregroundGraceUntil_ = Now() + static_cast<uint64_t>(foregroundGraceMs_);
}

void InputStateMachine::ResetBaseline() {
    haveLanguageBaseline_ = true;
    lastEmitKind_         = IndicatorKind::None;
    lastEmitState_        = InputStateSnapshot{};
    lastEmitTick_         = 0;
}

} // namespace imi
