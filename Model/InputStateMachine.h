// InputStateMachine.h — 트레이 언어 이벤트와 Caps Lock 이벤트를 "지금 이 표시를
// 띄워라"라는 결정으로 바꾼다. 현재 스냅샷을 소유하고, 디바운스하며, 이벤트가
// 아닌 것들을 억제한다: 시작 시의 최초 동기화, 중복 상태, 그리고 앱 전환 시
// 트레이가 새로 포커스된 창의 모드로 동기화하는 것(유예 구간). 호출자가 보고한
// 언어 전환 키 입력(한/영, Win+Space — LL 키보드 훅이 관찰)은 유예 구간을
// 무시하므로, 앱 전환 직후의 진짜 전환은 그대로 표시된다.
#pragma once

#include <functional>
#include <cstdint>
#include "InputState.h"

namespace imi {

// *눈에 보이는* 변화가 오버레이를 띄울 만할 때 방출된다.
struct IndicatorDecision {
    IndicatorKind kind = IndicatorKind::None;
    InputStateSnapshot state;
};

class InputStateMachine {
public:
    using Emit = std::function<void(const IndicatorDecision&)>;

    void SetEmit(Emit cb) { emit_ = std::move(cb); }

    // 기능 게이트(Config에서 온다). 게이트가 꺼져 있어도 피드는 내부 상태를 계속
    // 갱신하지만(다음 활성화 때 올바른 기준선을 갖도록) 방출은 하지 않는다.
    void SetGates(bool notifyLanguage, bool notifyCapsLock);

    // 이 밀리초 안에 들어온 동일한 반복(같은 종류이면서 보이는 상태도 같음)은
    // 버린다. 상태가 다르면 항상 방출하므로, 빠른 가→A→가 연타는 실제와 일치하는
    // 글리프를 표시하며 끝난다.
    void SetDebounceMs(uint32_t ms) { debounceMs_ = ms; }

    // 포어그라운드 변경 이후의 유예 구간. 이 동안의 언어 갱신은 새로 포커스된 창의
    // 모드로 조용히 동기화된 것으로 취급한다(앱 전환 시 캡슐을 띄우지 않는다).
    void SetForegroundGraceMs(uint32_t ms) { foregroundGraceMs_ = ms; }

    // --- 피드 지점(모두 UI 스레드에서 실행) ---

    // 트레이 감시자가 현재 실효 입력 언어를 보고했다. LanguageChanged를 방출할 수
    // 있다. |lastLangKeyTick|은 가장 최근의 언어 전환 키 입력 tick이다
    // (0 = 없음/알 수 없음). kLangKeyOverrideMs 이내의 키 입력은 이 갱신을,
    // 포어그라운드 유예 구간 안이더라도, 진짜 전환으로 표시한다.
    // |methodSwitch| = 이 보고가 입력기 전환(Win+Space 등, 트레이 전환 칩의
    // rename)에서 왔다는 뜻이다. 입력기 전환은 언제나 의도적인 사용자 행동이므로
    // 유예 구간을 무시하고, 보이는 언어가 같더라도 방출한다 — 한국어 IME는 입력기
    // 왕복 후 영어(A) 모드로 리셋되므로, A→A라도 "지금 이 상태"를 확인해 주는
    // 캡슐이 필요하다(macOS 동작).
    void OnLanguage(Language lang, uint64_t lastLangKeyTick = 0,
                    bool methodSwitch = false);

    // Caps Lock이 토글되었다(LL 훅에서 온다). 실제 변화가 있을 때만 방출하므로,
    // 현재 상태를 확인해 주는 피드는 조용히 넘어간다.
    void OnCapsLock(bool capsOn);

    // 포어그라운드 창이 바뀌었다: 곧 이어질 트레이의 새 창 모드 동기화가 캡슐을
    // 번쩍이지 않도록 짧은 유예 구간을 연다.
    void OnForegroundChanged();

    // 방출 없이 기준선을 "현재 == 표시됨"으로 다시 세운다(활성화 직후에 사용하여
    // 다음의 진짜 전환이 사용자가 보게 되는 것이 되도록 한다).
    void ResetBaseline();

    InputStateSnapshot Current() const { return current_; }

    // 테스트 훅: tick 공급원을 교체한다(기본값은 GetTickCount64).
    void SetClockForTest(uint64_t (*clock)()) { clockOverride_ = clock; }

    // 언어 전환 키 입력 후 이만큼 이내에 온 언어 이벤트는, 포어그라운드 유예 구간
    // 안이더라도 진짜 전환이다.
    static constexpr uint32_t kLangKeyOverrideMs = 500;

private:
    uint64_t Now() const;
    void EmitIfGated(IndicatorKind kind);
    bool WithinDebounce(uint64_t nowTick) const;

    Emit emit_;
    InputStateSnapshot current_{};
    InputStateSnapshot lastEmitState_{};
    bool     haveLanguageBaseline_ = false;
    bool     notifyLanguage_ = true;
    bool     notifyCapsLock_ = true;
    uint32_t debounceMs_ = 60;
    uint32_t foregroundGraceMs_ = 400;
    uint64_t foregroundGraceUntil_ = 0;
    uint64_t lastEmitTick_ = 0;
    IndicatorKind lastEmitKind_ = IndicatorKind::None;
    uint64_t (*clockOverride_)() = nullptr;
};

} // namespace imi
