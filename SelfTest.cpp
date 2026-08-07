// SelfTest.cpp — 순수 로직에 대한 인바이너리 테스트: 트레이 Name 파서(모드 칩 /
// 입력기 전환 칩 판별 포함)와 상태 기계의 억제 규칙(기준선, 중복 제거, 디바운스,
// 유예 구간 + 언어 키 무시 규칙, 입력기 전환(methodSwitch) 규칙, 게이트, 조용한
// caps 재동기화).
//
// `InputMethodIndicator.exe --selftest`로 실행하며, 종료 코드는 실패 개수다.
// 출력은 부모 콘솔이 있으면(터미널에서 실행한 경우) 그쪽으로 가고, 언제나
// OutputDebugStringW로도 나간다.
#include "SelfTest.h"

#include <windows.h>
#include <cstdio>
#include <cwchar>
#include <vector>

#include "InputState.h"
#include "InputStateMachine.h"

namespace imi {

namespace {

int  g_failures = 0;
bool g_console  = false;

// 타이밍 규칙을 결정적으로 테스트하기 위해 주입하는 시계.
uint64_t g_now = 0;
uint64_t TestNow() { return g_now; }

void Emit(const wchar_t* line) {
    OutputDebugStringW(line);
    OutputDebugStringW(L"\n");
    if (g_console) {
        wprintf(L"%s\n", line);
    }
}

void Fail(const wchar_t* expr, int line) {
    ++g_failures;
    wchar_t buf[512];
    _snwprintf_s(buf, _TRUNCATE, L"[selftest] FAIL  %s  (SelfTest.cpp:%d)", expr, line);
    Emit(buf);
}

#define CHECK(expr) \
    do { if (!(expr)) Fail(L"" #expr, __LINE__); } while (0)

// 상태 기계가 방출하는 모든 것을 모은다.
struct EmitLog {
    std::vector<IndicatorDecision> items;
    void Attach(InputStateMachine& sm) {
        sm.SetEmit([this](const IndicatorDecision& d) { items.push_back(d); });
    }
    size_t Count() const { return items.size(); }
    const IndicatorDecision& Last() const { return items.back(); }
};

void TestParser() {
    CHECK(ParseTrayIndicatorLanguage(nullptr) == Language::Unknown);
    CHECK(ParseTrayIndicatorLanguage(L"") == Language::Unknown);
    CHECK(ParseTrayIndicatorLanguage(L"battery status") == Language::Unknown);

    // 한국어 Windows UI.
    CHECK(ParseTrayIndicatorLanguage(L"트레이 입력 표시기 한국어 입력 모드") == Language::Korean);
    CHECK(ParseTrayIndicatorLanguage(L"트레이 입력 표시기 영어 입력 모드") == Language::English);
    CHECK(ParseTrayIndicatorLanguage(L"일본어 입력 모드") == Language::Japanese);
    CHECK(ParseTrayIndicatorLanguage(L"중국어(간체) 입력 모드") == Language::ChineseS);
    CHECK(ParseTrayIndicatorLanguage(L"중국어(번체) 입력 모드") == Language::ChineseT);

    // 영어 Windows UI.
    CHECK(ParseTrayIndicatorLanguage(L"Tray input indicator Korean input mode") == Language::Korean);
    CHECK(ParseTrayIndicatorLanguage(L"Tray input indicator English input mode") == Language::English);
    CHECK(ParseTrayIndicatorLanguage(L"Alphanumeric input mode") == Language::English);
    CHECK(ParseTrayIndicatorLanguage(L"Chinese (Traditional) input mode") == Language::ChineseT);
    CHECK(ParseTrayIndicatorLanguage(L"Chinese (Simplified) input mode") == Language::ChineseS);

    // 글리프 매핑.
    CHECK(wcscmp(LanguageGlyph(Language::Korean, true),  L"가") == 0);
    CHECK(wcscmp(LanguageGlyph(Language::Korean, false), L"A") == 0);
    CHECK(wcscmp(LanguageGlyph(Language::Unknown, false), L"A") == 0);

    // 모드 칩 / 입력기 전환 칩 판별. 전환 칩 이름은 Win11에서 실측한 값이다
    // (입력기가 2개 이상일 때 트레이에 함께 존재하며, 둘 다 언어로 파싱된다).
    CHECK(IsTrayModeIndicatorName(L"트레이 입력 표시기 한국어 입력 모드"));
    CHECK(IsTrayModeIndicatorName(L"트레이 입력 표시기 영어 입력 모드"));
    CHECK(IsTrayModeIndicatorName(L"Tray input indicator English input mode"));
    const wchar_t* kSwitcherKo =
        L"트레이 입력 표시기 한국어\nMicrosoft 입력기\n\n"
        L"입력 방법을 전환하려면 Windows 키 + 스페이스를 누르세요.";
    const wchar_t* kSwitcherEn =
        L"트레이 입력 표시기 영어(미국)\nUS\n\n"
        L"입력 방법을 전환하려면 Windows 키 + 스페이스를 누르세요.";
    CHECK(!IsTrayModeIndicatorName(kSwitcherKo));
    CHECK(!IsTrayModeIndicatorName(kSwitcherEn));
    CHECK(!IsTrayModeIndicatorName(
        L"Tray input indicator English (United States)\nUS keyboard\n\n"
        L"To switch input methods, press the Windows key + Space."));
    CHECK(!IsTrayModeIndicatorName(nullptr));
    // 전환 칩도 언어로는 파싱된다(대체 후보로 쓰인다).
    CHECK(ParseTrayIndicatorLanguage(kSwitcherKo) == Language::Korean);
    CHECK(ParseTrayIndicatorLanguage(kSwitcherEn) == Language::English);
}

void TestBaselineAndDedupe() {
    g_now = 1000;
    InputStateMachine sm;
    sm.SetClockForTest(&TestNow);
    EmitLog log;
    log.Attach(sm);

    sm.OnLanguage(Language::Korean);          // 첫 보고: 조용한 기준선,
    CHECK(log.Count() == 0);                  // 시작 후 몇 초 뒤라도 마찬가지
    CHECK(sm.Current().language == Language::Korean);

    g_now += 1000;
    sm.OnLanguage(Language::Korean);          // 중복: 조용함
    CHECK(log.Count() == 0);

    g_now += 1000;
    sm.OnLanguage(Language::English);         // 진짜 변화: 방출
    CHECK(log.Count() == 1);
    CHECK(log.Last().kind == IndicatorKind::LanguageChanged);
    CHECK(log.Last().state.language == Language::English);
    CHECK(!log.Last().state.imeOpen);
}

void TestDebounceIsStateAware() {
    g_now = 1000;
    InputStateMachine sm;
    sm.SetClockForTest(&TestNow);
    sm.SetDebounceMs(60);
    EmitLog log;
    log.Attach(sm);

    sm.OnLanguage(Language::Korean);          // 기준선
    g_now += 500;
    sm.OnLanguage(Language::English);         // 방출 #1
    CHECK(log.Count() == 1);

    // 디바운스 구간 안이라도 종류는 같고 상태가 다른(DIFFERENT) 경우에는 반드시
    // 방출해야 한다. 그래야 빠른 가→A→가 연타 뒤 마지막 캡슐이 실제와 일치한다.
    g_now += 10;
    sm.OnLanguage(Language::Korean);
    CHECK(log.Count() == 2);
    CHECK(log.Last().state.language == Language::Korean);

    // Caps: 실제 변화만 방출한다. 상태를 확인해 주는 재동기화는 조용하다.
    g_now += 500;
    sm.OnCapsLock(true);                      // 방출 #3
    CHECK(log.Count() == 3);
    g_now += 10;
    sm.OnCapsLock(true);                      // 변화 없음: 조용함
    CHECK(log.Count() == 3);
    g_now += 10;
    sm.OnCapsLock(false);                     // 변화 있음(다른 종류): 방출
    CHECK(log.Count() == 4);
    CHECK(log.Last().kind == IndicatorKind::CapsLockOff);
}

void TestGraceWindow() {
    g_now = 1000;
    InputStateMachine sm;
    sm.SetClockForTest(&TestNow);
    sm.SetForegroundGraceMs(400);
    EmitLog log;
    log.Attach(sm);

    sm.OnLanguage(Language::Korean);          // 기준선
    g_now += 1000;
    sm.OnForegroundChanged();
    g_now += 100;
    sm.OnLanguage(Language::English);         // 앱 전환 동기화: 억제됨...
    CHECK(log.Count() == 0);
    CHECK(sm.Current().language == Language::English);   // ...하지만 상태는 반영

    g_now += 1000;                            // 유예 구간은 이미 오래전에 만료
    sm.OnLanguage(Language::Korean);
    CHECK(log.Count() == 1);
}

void TestGraceOverriddenByLangKey() {
    g_now = 1000;
    InputStateMachine sm;
    sm.SetClockForTest(&TestNow);
    sm.SetForegroundGraceMs(400);
    EmitLog log;
    log.Attach(sm);

    sm.OnLanguage(Language::Korean);          // 기준선
    g_now += 1000;

    // 50ms 전에 한/영을 눌렀다 -> 유예 구간에도 불구하고 진짜 전환이다.
    sm.OnForegroundChanged();
    g_now += 100;
    sm.OnLanguage(Language::English, g_now - 50);
    CHECK(log.Count() == 1);

    // 오래된 키 입력은 유예 구간을 무시하지 못한다.
    sm.OnForegroundChanged();
    g_now += 100;
    sm.OnLanguage(Language::Korean, g_now - 5000);
    CHECK(log.Count() == 1);
}

void TestMethodSwitch() {
    g_now = 1000;
    InputStateMachine sm;
    sm.SetClockForTest(&TestNow);
    sm.SetForegroundGraceMs(400);
    EmitLog log;
    log.Attach(sm);

    sm.OnLanguage(Language::Korean);          // 기준선
    g_now += 1000;

    // 입력기 전환(한국어 IME -> 영어 자판): 보이는 변화 -> 방출.
    sm.OnLanguage(Language::English, 0, /*methodSwitch*/ true);
    CHECK(log.Count() == 1);

    // 되돌아오는 전환(영어 자판 -> 한국어 IME). IME는 A 모드로 리셋되므로 보고
    // 언어는 그대로 English다 — 그래도 입력기 전환이면 방출해야 한다(사용자는
    // 방금 자판을 바꿨고 현재 상태를 확인해 주는 캡슐을 기대한다).
    g_now += 1000;
    sm.OnLanguage(Language::English, 0, /*methodSwitch*/ true);
    CHECK(log.Count() == 2);
    CHECK(log.Last().state.language == Language::English);

    // 전환이 아닌 같은 언어 보고(주기 재검증)는 여전히 조용하다.
    g_now += 1000;
    sm.OnLanguage(Language::English);
    CHECK(log.Count() == 2);

    // 입력기 전환은 유예 구간도 무시한다(전환 칩은 입력기가 실제로 바뀔 때만
    // rename되므로 구조상 사용자 행동이다).
    g_now += 1000;
    sm.OnForegroundChanged();
    g_now += 100;
    sm.OnLanguage(Language::Korean, 0, /*methodSwitch*/ true);
    CHECK(log.Count() == 3);
    CHECK(log.Last().state.language == Language::Korean);

    // 대조: 유예 구간 안의 일반 보고(앱 전환 동기화)는 계속 억제된다.
    g_now += 1000;
    sm.OnForegroundChanged();
    g_now += 100;
    sm.OnLanguage(Language::English);
    CHECK(log.Count() == 3);
}

void TestGates() {
    g_now = 1000;
    InputStateMachine sm;
    sm.SetClockForTest(&TestNow);
    EmitLog log;
    log.Attach(sm);

    sm.SetGates(/*lang*/ true, /*caps*/ false);
    sm.OnCapsLock(true);                      // caps 게이트 꺼짐: 조용함...
    CHECK(log.Count() == 0);
    CHECK(sm.Current().capsLock);             // ...하지만 상태는 계속 추적된다

    sm.SetGates(/*lang*/ false, /*caps*/ false);
    sm.OnLanguage(Language::Korean);          // 기준선(어차피 조용하다)
    g_now += 100;
    sm.OnLanguage(Language::English);         // 언어 게이트 꺼짐: 조용함
    CHECK(log.Count() == 0);
    CHECK(sm.Current().language == Language::English);

    sm.SetGates(/*lang*/ true, /*caps*/ true);
    g_now += 100;
    sm.OnCapsLock(false);                     // 다시 켬: 진짜 변화는 방출된다
    CHECK(log.Count() == 1);
    CHECK(log.Last().kind == IndicatorKind::CapsLockOff);

    g_now += 100;
    sm.OnLanguage(Language::Korean);          // 언어도 다시 방출된다
    CHECK(log.Count() == 2);
    CHECK(log.Last().kind == IndicatorKind::LanguageChanged);
}

} // namespace

int RunSelfTests() {
    // GUI 서브시스템이다: 실행한 콘솔이 있으면 거기에 붙어 결과가 보이게 한다.
    // 기계가 읽는 계약은 종료 코드다.
    g_console = (AttachConsole(ATTACH_PARENT_PROCESS) != FALSE);
    if (g_console) {
        FILE* out = nullptr;
        _wfreopen_s(&out, L"CONOUT$", L"w", stdout);
    }

    TestParser();
    TestBaselineAndDedupe();
    TestDebounceIsStateAware();
    TestGraceWindow();
    TestGraceOverriddenByLangKey();
    TestMethodSwitch();
    TestGates();

    wchar_t buf[128];
    _snwprintf_s(buf, _TRUNCATE, L"[selftest] %s (%d failure%s)",
                 (g_failures == 0) ? L"PASS" : L"FAIL",
                 g_failures, (g_failures == 1) ? L"" : L"s");
    Emit(buf);

    if (g_console) {
        FreeConsole();
    }
    return g_failures;
}

} // namespace imi
