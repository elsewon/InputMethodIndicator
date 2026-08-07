// InputState.h — "지금 입력 상태가 무엇인가"에 대한 앱의 정규화된 모델과,
// 글리프 매핑, 그리고 OS 입력 표시기의 (지역화된) 접근성 Name을 Language로
// 바꾸는 파서.
//
// 상태의 출처(SOURCE)는 UI Automation으로 읽는 Windows 입력 표시기다
// (Input/TrayIndicatorWatcher). 언어를 추가하려면 Language와 파서,
// LanguageGlyph를 확장하면 된다 — 캐럿/오버레이 계층은 바뀌지 않는다.
#pragma once

#include <cstdint>
#include <cwchar>   // wcsstr

namespace imi {

// 표시기가 보고하는 실효 입력 언어. English == 레이아웃이 영숫자("A") 모드라는
// 뜻이고, 이름이 있는 언어 == 해당 IME가 활성이며 고유 문자
// ("가"/"あ"/"中")라는 뜻이다. 트레이가 영숫자 상태의 한국어를 "영어/English"로
// 보고하므로 — 정확히 A 상태다 — 변환 하위 모드는 이미 여기에 반영되어 있다.
enum class Language : uint8_t {
    Unknown = 0,
    English,
    Korean,      // 한글
    Japanese,    // 日本語 (최선 노력)
    ChineseS,    // 简体中文 (최선 노력)
    ChineseT,    // 繁體中文 (최선 노력)
};

// 어떤 종류의 변화가 오버레이를 띄울 만한지. Caps Lock은 언어와 직교한다.
enum class IndicatorKind : uint8_t {
    None = 0,
    LanguageChanged,   // 가 / A / あ / 中
    CapsLockOn,        // ⇪ 켬
    CapsLockOff,       // ⇪ 끔
};

// 이름이 있는 IME 언어(즉 "A"가 아니라 고유 문자를 표시하는 언어).
inline bool IsNativeLanguage(Language l) {
    return l == Language::Korean || l == Language::Japanese ||
           l == Language::ChineseS || l == Language::ChineseT;
}

// 눈에 보이는 입력 상태의 비교 가능한 스냅샷.
struct InputStateSnapshot {
    Language language = Language::Unknown;
    bool     imeOpen  = false;   // 고유 문자 IME 활성(language에서 파생)
    bool     capsLock = false;
    uint64_t timestampTick = 0;

    bool SameVisibleState(const InputStateSnapshot& o) const {
        return language == o.language && imeOpen == o.imeOpen && capsLock == o.capsLock;
    }
};

// 오버레이/트레이가 언어 차원에 대해 그리는 짧은 글리프.
inline const wchar_t* LanguageGlyph(Language lang, bool imeOpen) {
    switch (lang) {
        case Language::Korean:   return imeOpen ? L"가" : L"A";
        case Language::Japanese: return imeOpen ? L"あ" : L"A";
        case Language::ChineseS: return imeOpen ? L"中" : L"A";
        case Language::ChineseT: return imeOpen ? L"中" : L"A";
        case Language::English:  return L"A";
        case Language::Unknown:
        default:                 return L"A";
    }
}

// 입력 표시기의 접근성 Name을 Language로 파싱한다. Name은 지역화(LOCALIZED)
// 되어 있다. 예를 들어 한국어 UI에서는:
//   "트레이 입력 표시기 한국어 입력 모드"  -> Korean   (가)
//   "트레이 입력 표시기 영어 입력 모드"    -> English  (A)
// 영어 UI에서는: "... Korean input mode" / "... English input mode".
// 인식 가능한 언어 토큰이 없으면 Language::Unknown을 반환하므로, 호출자는 그것을
// "입력 표시기가 아님"으로 보고 무시할 수 있다.
inline Language ParseTrayIndicatorLanguage(const wchar_t* name) {
    if (name == nullptr) return Language::Unknown;
    auto has = [name](const wchar_t* token) { return wcsstr(name, token) != nullptr; };

    // 영숫자 / 영어 모드를 먼저 본다(이것이 "A" 상태다).
    if (has(L"영어") || has(L"영문") || has(L"English") || has(L"Alphanumeric"))
        return Language::English;
    if (has(L"한국어") || has(L"Korean"))   return Language::Korean;
    if (has(L"일본어") || has(L"Japanese")) return Language::Japanese;
    // "번체/Traditional"이 이기도록 일반 중국어보다 번체를 먼저 검사한다.
    if (has(L"번체") || has(L"Traditional")) return Language::ChineseT;
    if (has(L"간체") || has(L"Simplified"))  return Language::ChineseS;
    if (has(L"중국어") || has(L"Chinese"))   return Language::ChineseS;
    return Language::Unknown;
}

// 입력기가 2개 이상이면 트레이에는 언어로 파싱되는 칩이 **둘** 있다:
//   변환 모드 칩   "트레이 입력 표시기 한국어/영어 입력 모드"   — 한/영 토글마다 rename
//   입력기 전환 칩 "트레이 입력 표시기 한국어\nMicrosoft 입력기…" — 입력기 변경 시 rename
// 한/영 상태의 출처는 모드 칩이므로 감시자는 이를 우선해야 한다. 전환 칩의
// Name("…한국어…")은 IME의 현재 모드와 무관하게 언어로 파싱되기 때문이다.
// 이 판별자는 모드 칩을 알아본다(한국어/영어 UI 로케일).
inline bool IsTrayModeIndicatorName(const wchar_t* name) {
    if (name == nullptr) return false;
    return wcsstr(name, L"입력 모드") != nullptr ||
           wcsstr(name, L"input mode") != nullptr;
}

} // namespace imi
