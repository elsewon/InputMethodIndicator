// TrayIndicatorWatcher.cpp — TrayIndicatorWatcher.h 참고.
//
// 동작 원리: 트레이의 입력 관련 칩을 찾아 현재 언어를 읽고, Name 변경 이벤트로
// 전환을 실시간으로 알린다. 입력기가 2개 이상이면 언어로 파싱되는 칩이 둘이다
// (InputState.h의 IsTrayModeIndicatorName 참고):
//   변환 모드 칩   "…한국어/영어 입력 모드"      — 한/영 토글마다 rename
//   입력기 전환 칩 "…한국어\nMicrosoft 입력기…"  — 입력기 변경 시 rename
// 한/영 상태의 출처는 모드 칩이므로 탐색은 모드 칩을 우선한다.
//
// 구독은 개별 칩이 아니라 **트레이 루트(Shell_TrayWnd)에 서브트리 범위**로 건다.
// 모드 칩은 입력기 전환마다 파괴되고 다른 runtime id로 재생성되는 것이 관찰되어
// (영어 자판 전환 시 사라졌다가 복귀 시 새 요소로 돌아온다), 칩 단위 구독은 그
// 순간 조용히 죽는다. 더 나쁘게는, 파괴된 칩이 get_CurrentName에 마지막 Name으로
// 계속 답하기 때문에(좀비) "살아 있나" 검사로는 이를 알아챌 수 없다. 그래서
// (a) 이벤트 앵커는 전환에도 살아남는 트레이 루트로 하고, (b) 탐색은 캐시된
// 포인터를 신뢰하지 않고 매번 신선하게 다시 찾는다.
//
// 핸들러 규칙(UIA 스레드 풀에서 실행, UIA 호출 없음): 모드 칩 rename은 새 상태가
// 이벤트 VARIANT에 실려 오므로 즉시 호스트로 post한다. 전환 칩 rename은 입력기
// 변경 신호일 뿐 그 Name의 언어는 IME의 실제 모드가 아니므로(복귀 직후 IME는
// 영어(A) 모드로 리셋된다) post하지 않고 작업자에게 재탐색을 신호한다 —
// 작업자는 잠시 기다렸다가(새 모드 칩이 생성될 시간) 최선 칩을 다시 찾아 그
// 상태를 methodSwitch로 post한다. 입력기 전환은 보이는 글리프가 같아도 캡슐을
// 띄워야 하기 때문이다(상태 머신의 OnLanguage 참고).
//
// 모든 UIA 작업(클라이언트 생성, 탐색, 구독, 재검증)은 이 클래스가 소유한 MTA
// 작업자 스레드에서 실행된다. 탐색은 explorer를 상대로 한 동기 프로세스 간
// 호출의 연속이라 수백 ms~수 초가 걸릴 수 있고, 호스트 UI 스레드는
// WH_KEYBOARD_LL 훅과 트레이 메뉴를 소유하므로 거기서 블로킹하면 시스템 전역
// 입력 지연과 메뉴 멈춤이 된다.
//
// 작업자의 저빈도 재검증 틱은 구독이 살아 있는지 확인하고(explorer 재시작 대비)
// 최선 칩을 다시 찾아 상태 표류를 바로잡되, 표시기가 계속 없는 동안에는
// 백오프한다. 그래서 구독이 실패해도 침묵이 아니라 폴링으로 강등된다.
#include "TrayIndicatorWatcher.h"

#include <windows.h>
#include <objbase.h>          // uiautomation.h보다 먼저 와야 한다(COM 기반 / `interface`)
#include <uiautomation.h>
#include <oleauto.h>          // VARIANT / SysAllocString
#include <new>                // std::nothrow

#include "ComRef.h"
#include "Log.h"

namespace imi {

namespace {
constexpr UINT kRevalidateMs = 4000;
// 전환 칩 rename(입력기 변경) 후 재탐색까지의 대기. 새 모드 칩은 전환 칩
// rename보다 약 200-400ms 늦게 생성되는 것이 관찰되었다.
constexpr DWORD kRescanDelayMs = 450;
// 연속으로 이만큼 탐색에 실패하면 N번째 틱에만 시도한다(탐색은 트레이를 프로세스
// 간으로 순회하므로, 그러지 않으면 입력기가 하나뿐인 기기는 그 비용을 매 틱마다
// 영원히 치르게 된다).
constexpr UINT kAbsentBackoffAfter  = 5;
constexpr UINT kAbsentBackoffEvery  = 8;   // kRevalidateMs = 4초 기준 약 32초
// 단일 탐색이 이만큼 걸리면 이상(대개 explorer 접근성 트리 병듦)으로 본다.
// 건강한 트레이 순회는 수십~수백 ms이고, 정상적으로 부재한 경우(입력기 1개,
// 표시기 숨김)도 빠르게 실패한다. 지속 실패 스트릭 안에서 이만큼 느린 탐색을 한
// 번이라도 보면, 사용자에게 알려야 하는 이상 상태로 판정한다.
constexpr uint64_t kSickSearchMs    = 3000;
}

// ---------------------------------------------------------------------------
// UIA 속성 변경 핸들러. UIA 스레드 풀 스레드에서 실행되며 watcher 상태를 전혀
// 건드리지 않는다 — VARIANT의 새 Name만 보고 호스트에 post하거나 작업자에게
// 재탐색을 신호한다(파일 상단의 핸들러 규칙 참고).
// ---------------------------------------------------------------------------
class TrayNameHandler : public IUIAutomationPropertyChangedEventHandler {
public:
    // |rescanSource|는 복제해서 소유한다. UIA가 제거 후에도 늦은 콜백을 전달할
    // 수 있으므로, 원본 핸들이 닫힌 뒤에도 유효한 핸들이 필요하다.
    TrayNameHandler(HWND wnd, UINT msg, HANDLE rescanSource) : wnd_(wnd), msg_(msg) {
        DuplicateHandle(GetCurrentProcess(), rescanSource, GetCurrentProcess(),
                        &rescan_, 0, FALSE, DUPLICATE_SAME_ACCESS);
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&ref_); }
    ULONG STDMETHODCALLTYPE Release() override {
        const LONG r = InterlockedDecrement(&ref_);
        if (r == 0) delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (ppv == nullptr) return E_POINTER;
        if (riid == __uuidof(IUnknown) ||
            riid == __uuidof(IUIAutomationPropertyChangedEventHandler)) {
            *ppv = static_cast<IUIAutomationPropertyChangedEventHandler*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE HandlePropertyChangedEvent(
        IUIAutomationElement* /*sender*/, PROPERTYID propertyId, VARIANT newValue) override {
        if (propertyId != UIA_NamePropertyId || newValue.vt != VT_BSTR || newValue.bstrVal == nullptr)
            return S_OK;
        const Language lang = ParseTrayIndicatorLanguage(newValue.bstrVal);
        if (lang == Language::Unknown) return S_OK;   // 시계/볼륨 등 — 표시기 이름이 아니다
        if (IsTrayModeIndicatorName(newValue.bstrVal)) {
            // 모드 칩 rename(한/영 토글): 새 상태가 손에 있다. 즉시 알린다.
            PostMessageW(wnd_, msg_, static_cast<WPARAM>(static_cast<uint8_t>(lang)), 0);
        } else if (rescan_ != nullptr) {
            // 전환 칩 rename(입력기 변경): 작업자가 잠시 뒤 최선 칩을 다시 찾아
            // 실제 상태를 methodSwitch로 post한다.
            SetEvent(rescan_);
        }
        return S_OK;
    }

private:
    ~TrayNameHandler() { if (rescan_ != nullptr) CloseHandle(rescan_); }

    LONG   ref_ = 1;
    HWND   wnd_;
    UINT   msg_;
    HANDLE rescan_ = nullptr;
};

// ---------------------------------------------------------------------------
struct TrayWatcherImpl {
    // 작업자 스레드가 소유하는 UIA 상태. 생성부터 해제까지 전부 그 스레드에서만
    // 접근한다(|foundFlag|만 예외 — UI 스레드가 진단용으로 읽는다).
    IUIAutomation*             uia = nullptr;
    IUIAutomationTreeWalker*   walker = nullptr;      // raw 뷰 워커(깊이 우선 탐색 대체 경로)
    IUIAutomationElement*      trayRoot = nullptr;    // 구독 앵커(Shell_TrayWnd 루트)
    HWND                       trayRootHwnd = nullptr;
    IUIAutomationElement*      indicator = nullptr;   // 마지막으로 찾은 최선 칩
    TrayNameHandler*           handler = nullptr;
    HWND                       notifyWnd = nullptr;
    UINT                       notifyMsg = 0;         // 언어 보고
    UINT                       healthMsg = 0;         // 건강 상태 전이(이상/회복)
    bool                       subscribed = false;    // 트레이 루트 서브트리 구독이 살아 있다
    Language                   lastPosted = Language::Unknown; // 재탐색 경로 중복 제거
    UINT                       absentFails = 0;       // 연속 탐색 실패 횟수
    UINT                       tickCount = 0;         // 재검증 틱(백오프용)
    bool                       sickNotified = false;  // 이상 알림을 이미 보냈다(전이당 1회 래치)
    uint64_t                   streakMaxSearchMs = 0; // 현재 실패 스트릭에서 본 최장 탐색

    // 스레딩. UI 스레드가 Start에서 만들고 작업자와 공유한다.
    HANDLE                     thread = nullptr;
    HANDLE                     quitEvent = nullptr;   // 수동 리셋: 작업자 루프 종료 신호
    HANDLE                     rescanEvent = nullptr; // 자동 리셋: 핸들러 -> 작업자 재탐색
    volatile LONG              foundFlag = 0;         // IndicatorFound()가 UI 스레드에서 읽는다

    // CreateThread 진입 thunk.
    static DWORD WINAPI ThreadThunk(LPVOID param);
    // 작업자(MTA) 스레드에서 실행된다. quitEvent가 올 때까지 재검증 틱을 돈다.
    void ThreadLoop();
};

namespace {

// 탐색 결과: AddRef된 최선 칩과 그 분류.
struct FoundIndicator {
    IUIAutomationElement* el = nullptr;   // 소유 참조(호출자가 Release)
    Language              lang = Language::Unknown;
    bool                  isMode = false; // 변환 모드 칩인가(전환 칩 대비 우선)
};

// 빠른 경로: 입력 표시기의 AutomationId("SystemTrayIcon")를 가진 하위 요소들을
// 대상으로 프로세스 간 FindAll을 한 번 수행하며, 같은 호출에서 Name을 캐시한다.
// 후보들을 분류해 모드 칩을 우선하고, 없으면 첫 파싱 가능한 칩(전환 칩)을 쓴다.
void FindIndicatorFast(IUIAutomation* uia, IUIAutomationElement* root,
                       FoundIndicator& out) {
    ComRef<IUIAutomationCacheRequest> cache;
    if (FAILED(uia->CreateCacheRequest(&cache)) || !cache) return;
    cache->AddProperty(UIA_NamePropertyId);

    VARIANT idVal;
    VariantInit(&idVal);
    idVal.vt = VT_BSTR;
    idVal.bstrVal = SysAllocString(L"SystemTrayIcon");
    if (idVal.bstrVal == nullptr) return;
    ComRef<IUIAutomationCondition> cond;
    const HRESULT hrCond =
        uia->CreatePropertyCondition(UIA_AutomationIdPropertyId, idVal, &cond);
    VariantClear(&idVal);
    if (FAILED(hrCond) || !cond) return;

    ComRef<IUIAutomationElementArray> found;
    if (FAILED(root->FindAllBuildCache(TreeScope_Descendants, cond.p, cache.p, &found)) ||
        !found) {
        return;
    }
    int len = 0;
    found->get_Length(&len);
    for (int i = 0; i < len; ++i) {
        ComRef<IUIAutomationElement> el;
        if (FAILED(found->GetElement(i, &el)) || !el) continue;
        BSTR name = nullptr;
        if (SUCCEEDED(el->get_CachedName(&name)) && name != nullptr) {
            const Language lang = ParseTrayIndicatorLanguage(name);
            const bool isMode = IsTrayModeIndicatorName(name);
            SysFreeString(name);
            if (lang == Language::Unknown) continue;
            if (isMode) {
                if (out.el != nullptr) out.el->Release();
                out.el = el.Detach();
                out.lang = lang;
                out.isMode = true;
                return;   // 모드 칩이 최우선이다 — 더 볼 것 없음
            }
            if (out.el == nullptr) {
                out.el = el.Detach();
                out.lang = lang;
                out.isMode = false;
            }
        } else if (name != nullptr) {
            SysFreeString(name);
        }
    }
}

// 느린 대체 경로(셸 빌드에 따라 AutomationId가 바뀌는 경우): 트레이 하위 트리를
// 깊이 우선으로 탐색하며 같은 우선순위 규칙을 적용한다. 모드 칩을 만나면 즉시
// 멈추고, 아니면 첫 파싱 가능한 칩을 대체 후보로 들고 끝까지 간다.
void WalkCollect(IUIAutomationElement* el, IUIAutomationTreeWalker* walker,
                 int depth, FoundIndicator& best) {
    if (el == nullptr || depth > 16 || best.isMode) return;

    BSTR name = nullptr;
    if (SUCCEEDED(el->get_CurrentName(&name)) && name != nullptr) {
        const Language lang = ParseTrayIndicatorLanguage(name);
        if (lang != Language::Unknown) {
            if (IsTrayModeIndicatorName(name)) {
                if (best.el != nullptr) best.el->Release();
                el->AddRef();
                best.el = el;
                best.lang = lang;
                best.isMode = true;
                SysFreeString(name);
                return;
            }
            if (best.el == nullptr) {
                el->AddRef();
                best.el = el;
                best.lang = lang;
            }
        }
        SysFreeString(name);
    } else if (name != nullptr) {
        SysFreeString(name);
    }

    IUIAutomationElement* child = nullptr;
    if (SUCCEEDED(walker->GetFirstChildElement(el, &child)) && child != nullptr) {
        while (child != nullptr && !best.isMode) {
            WalkCollect(child, walker, depth + 1, best);
            IUIAutomationElement* next = nullptr;
            walker->GetNextSiblingElement(child, &next);
            child->Release();
            child = next;
        }
        if (child != nullptr) child->Release();
    }
}

void PostLanguage(TrayWatcherImpl* d, Language lang, bool methodSwitch) {
    PostMessageW(d->notifyWnd, d->notifyMsg,
                 static_cast<WPARAM>(static_cast<uint8_t>(lang)),
                 methodSwitch ? 1 : 0);
}

// 건강 상태 전이를 호스트로 알린다(UI 스레드가 트레이 풍선 알림으로 surface한다).
void PostHealth(TrayWatcherImpl* d, TrayWatcherHealth state) {
    if (d->healthMsg != 0) {
        PostMessageW(d->notifyWnd, d->healthMsg,
                     static_cast<WPARAM>(static_cast<uint8_t>(state)), 0);
    }
}

void DropSubscription(TrayWatcherImpl* d) {
    if (d->trayRoot != nullptr) {
        if (d->subscribed && d->handler != nullptr)
            d->uia->RemovePropertyChangedEventHandler(d->trayRoot, d->handler);
        d->trayRoot->Release();
        d->trayRoot = nullptr;
    }
    d->subscribed = false;
    d->trayRootHwnd = nullptr;
}

// 트레이 루트에 대한 서브트리 Name 구독을 유지한다(파일 상단 참고: 칩 단위
// 구독은 입력기 전환마다 죽는다). explorer가 재시작하면 루트가 죽으므로 다시
// 건다. 구독이 실패해도 주기 재탐색이 폴링으로 감당한다.
void EnsureSubscription(TrayWatcherImpl* d) {
    if (d->handler == nullptr) return;
    HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (tray == nullptr) {
        DropSubscription(d);
        return;
    }
    if (d->subscribed && tray == d->trayRootHwnd && d->trayRoot != nullptr) {
        // explorer 재시작 후 HWND 값이 재사용될 수 있으므로, 싼 속성 읽기로
        // 루트 요소 자체의 생존을 확인한다(hwnd 기반 루트는 프로세스가 죽으면
        // 호출이 실패한다 — 칩과 달리 좀비가 되지 않는다).
        UIA_HWND h = nullptr;
        if (SUCCEEDED(d->trayRoot->get_CurrentNativeWindowHandle(&h)) &&
            h == reinterpret_cast<UIA_HWND>(tray)) {
            return;   // 살아 있다
        }
    }
    DropSubscription(d);

    ComRef<IUIAutomationElement> root;
    if (FAILED(d->uia->ElementFromHandle(tray, &root)) || !root) return;

    PROPERTYID props[] = { UIA_NamePropertyId };
    const HRESULT hr = d->uia->AddPropertyChangedEventHandlerNativeArray(
        root.p, TreeScope_Subtree, nullptr, d->handler, props, ARRAYSIZE(props));
    d->trayRoot = root.Detach();
    d->trayRootHwnd = tray;
    d->subscribed = SUCCEEDED(hr);
    if (!d->subscribed) {
        IMI_WARN(L"TrayIndicatorWatcher: AddPropertyChangedEventHandler(루트) hr=0x%08lX"
                 L" — 폴링으로 대체합니다", static_cast<unsigned long>(hr));
    }
}

// 최선 칩을 신선하게 다시 찾아 상태를 post한다. |force|는 백오프를 무시한다
// (초기 탐색과 전환 재탐색). |methodSwitch|는 이 재탐색이 입력기 전환에서
// 비롯되었다는 뜻으로, 언어가 같아도 post한다(상태 머신이 캡슐을 띄우도록).
//
// 로그는 상태 전이에서만 남긴다. 실패는 4초마다 반복되므로 매번 찍으면
// 스팸이고, 전이만 찍으면 로그가 곧 이야기가 된다: 잃음/못 찾음(스트릭의 첫
// 실패) → 백오프 진입 → 다시 찾음. 탐색 소요 시간을 함께 기록하는 이유:
// 건강한 트레이는 수십~수백 ms에 답하지만, explorer 접근성 트리가 병들면
// 탐색이 수십 초씩 걸리며 실패한다(실측: FindAll 36초에 0개, explorer 재시작
// 후 47ms에 8개 — KNOWN-LIMITS 참고). 이 숫자가 로그만으로 두 상태를
// 구별하게 해 준다.
void RelocateAndPost(TrayWatcherImpl* d, bool force, bool methodSwitch) {
    if (!force) {
        ++d->tickCount;
        if (d->absentFails >= kAbsentBackoffAfter &&
            (d->tickCount % kAbsentBackoffEvery) != 0) {
            return;   // 백오프 중 — 표시기가 한동안 없는 상태다
        }
    }

    const bool hadIndicator = (d->indicator != nullptr);
    const uint64_t searchStart = GetTickCount64();

    FoundIndicator best;
    if (d->trayRoot != nullptr) {
        FindIndicatorFast(d->uia, d->trayRoot, best);
        if (best.el == nullptr && d->walker != nullptr) {
            WalkCollect(d->trayRoot, d->walker, 0, best);
        }
    }
    const unsigned long long searchMs = GetTickCount64() - searchStart;

    if (d->indicator != nullptr) {
        d->indicator->Release();
        d->indicator = nullptr;
    }

    if (best.el == nullptr) {
        InterlockedExchange(&d->foundFlag, 0);
        ++d->absentFails;
        if (searchMs > d->streakMaxSearchMs) d->streakMaxSearchMs = searchMs;
        if (d->absentFails == 1) {
            if (hadIndicator) {
                IMI_WARN(L"TrayIndicatorWatcher: 표시기를 잃었습니다 (탐색 %llums)."
                         L" explorer 재시작이나 입력기 목록 변경일 수 있습니다."
                         L" 약 %u초마다 재탐색합니다.",
                         searchMs, kRevalidateMs / 1000);
            } else {
                IMI_WARN(L"TrayIndicatorWatcher: 표시기를 찾지 못했습니다 (탐색 %llums)."
                         L" 약 %u초마다 재시도합니다.",
                         searchMs, kRevalidateMs / 1000);
            }
        } else if (d->absentFails == kAbsentBackoffAfter) {
            IMI_WARN(L"TrayIndicatorWatcher: 연속 %u회 탐색 실패 (마지막 %llums, 최장 %llums)."
                     L" 이제 약 %u초마다 재시도합니다. 탐색이 수십 초씩 걸리면"
                     L" explorer 접근성 트리가 병든 상태일 수 있습니다 — explorer"
                     L" 재시작으로 회복됩니다 (KNOWN-LIMITS 참고).",
                     kAbsentBackoffAfter, searchMs, d->streakMaxSearchMs,
                     (kRevalidateMs / 1000) * kAbsentBackoffEvery);
        }
        // 지속 실패 + 느린 탐색 = 사용자에게 알려야 하는 이상 상태(대개 explorer
        // 접근성 트리 병듦). 백오프 진입 시점에 한 번만 알린다. 정상적으로 부재한
        // 경우(입력기 1개, 표시기 숨김)는 탐색이 빠르므로 여기 걸리지 않는다.
        if (!d->sickNotified && d->absentFails >= kAbsentBackoffAfter &&
            d->streakMaxSearchMs >= kSickSearchMs) {
            d->sickNotified = true;
            PostHealth(d, TrayWatcherHealth::Unreadable);
        }
        return;
    }

    d->indicator = best.el;   // 소유권을 가져간다(진단 + '찾음' 상태)
    InterlockedExchange(&d->foundFlag, 1);
    d->absentFails = 0;
    d->streakMaxSearchMs = 0;

    // 없음 → 찾음 전이(시작 첫 발견과 잃은 뒤 재획득 모두)는 INFO로 남긴다.
    if (!hadIndicator) {
        IMI_INFO(L"TrayIndicatorWatcher: 표시기를 찾았습니다"
                 L" (언어=%d, 모드칩=%d, 이벤트=%d, %llums).",
                 static_cast<int>(best.lang), best.isMode ? 1 : 0,
                 d->subscribed ? 1 : 0, searchMs);
    }

    // 이상 알림을 보냈던 상태에서 회복되면, 회복도 한 번 알린다(사용자가 조치한
    // 뒤의 확인).
    if (d->sickNotified) {
        d->sickNotified = false;
        PostHealth(d, TrayWatcherHealth::Recovered);
    }

    if (best.lang != Language::Unknown &&
        (best.lang != d->lastPosted || methodSwitch)) {
        d->lastPosted = best.lang;
        PostLanguage(d, best.lang, methodSwitch);
        if (hadIndicator) {
            IMI_DEBUG(L"TrayIndicatorWatcher: 재탐색 언어=%d 모드칩=%d 전환=%d %llums",
                      static_cast<int>(best.lang), best.isMode ? 1 : 0,
                      methodSwitch ? 1 : 0, searchMs);
        }
    }
}

} // namespace

DWORD WINAPI TrayWatcherImpl::ThreadThunk(LPVOID param) {
    static_cast<TrayWatcherImpl*>(param)->ThreadLoop();
    return 0;
}

void TrayWatcherImpl::ThreadLoop() {
    // UIA 클라이언트는 MTA 스레드에 둔다(CaretResolver 작업자와 같은 규칙이다).
    // 이벤트 콜백은 어차피 UIA 스레드 풀에서 오고, MTA면 메시지 펌프 없이
    // 대기/틱 루프만으로 충분하다.
    const HRESULT hrInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool coInited = SUCCEEDED(hrInit);
    if (!coInited) {
        IMI_ERROR(L"TrayIndicatorWatcher: CoInitializeEx(MTA) 실패 hr=0x%08lX",
                  static_cast<unsigned long>(hrInit));
    }

    if (coInited) {
        const HRESULT hr = CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                                            __uuidof(IUIAutomation),
                                            reinterpret_cast<void**>(&uia));
        if (FAILED(hr) || uia == nullptr) {
            IMI_ERROR(L"TrayIndicatorWatcher: CoCreateInstance(CUIAutomation) hr=0x%08lX",
                      static_cast<unsigned long>(hr));
            uia = nullptr;
        }
    }

    if (uia != nullptr) {
        uia->get_RawViewWalker(&walker);   // 실패해도 빠른 경로(FindAll)만으로 동작한다
        handler = new (std::nothrow) TrayNameHandler(notifyWnd, notifyMsg, rescanEvent);
        if (handler == nullptr) {
            IMI_WARN(L"TrayIndicatorWatcher: 핸들러 할당 실패. 폴링으로만 동작합니다.");
        }

        EnsureSubscription(this);
        RelocateAndPost(this, /*force*/ true, /*methodSwitch*/ false);   // 초기 탐색 + 기준선

        HANDLE waits[2] = { quitEvent, rescanEvent };
        for (;;) {
            const DWORD w = WaitForMultipleObjects(2, waits, FALSE, kRevalidateMs);
            if (w == WAIT_OBJECT_0) {
                break;   // 종료
            }
            if (w == WAIT_OBJECT_0 + 1) {
                // 입력기 전환 신호다. 새 모드 칩이 생성될 시간을 준 뒤 재탐색한다.
                // 종료 신호는 즉시 존중한다.
                if (WaitForSingleObject(quitEvent, kRescanDelayMs) == WAIT_OBJECT_0) {
                    break;
                }
                // 한 번의 전환이 전환 칩을 여러 번 rename하는 것이 관찰되었다
                // (전이 중 이름이 두 번 설정된다). 지연 중 쌓인 신호를 지금
                // 합쳐서 같은 상태를 두 번 재탐색해 캡슐을 두 번 띄우지 않는다.
                // 재탐색 도중 이후에 도착하는 신호는 그대로 남아 다음 재탐색을
                // 일으킨다 — 그 시점 상태는 다시 읽어야 하기 때문이다.
                WaitForSingleObject(rescanEvent, 0);
                EnsureSubscription(this);
                RelocateAndPost(this, /*force*/ true, /*methodSwitch*/ true);
                continue;
            }
            if (w != WAIT_TIMEOUT) {
                break;   // 대기 실패/버려짐 — 계속 도는 대신 빠져나간다
            }
            EnsureSubscription(this);
            RelocateAndPost(this, /*force*/ false, /*methodSwitch*/ false);
        }
    } else {
        // UIA 없이는 감시할 방법이 없다 — 종료 신호만 기다린다(호스트는
        // IndicatorFound()==false를 본다).
        WaitForSingleObject(quitEvent, INFINITE);
    }

    DropSubscription(this);
    if (indicator != nullptr) { indicator->Release(); indicator = nullptr; }
    InterlockedExchange(&foundFlag, 0);
    if (handler != nullptr) { handler->Release(); handler = nullptr; }
    if (walker  != nullptr) { walker->Release();  walker  = nullptr; }
    if (uia     != nullptr) { uia->Release();     uia     = nullptr; }
    if (coInited) CoUninitialize();
}

TrayIndicatorWatcher::~TrayIndicatorWatcher() { Stop(); }

bool TrayIndicatorWatcher::Start(HWND notifyWnd, UINT langMsg, UINT healthMsg) {
    if (impl_ != nullptr) return true;

    impl_ = new (std::nothrow) TrayWatcherImpl();
    if (impl_ == nullptr) return false;
    impl_->notifyWnd = notifyWnd;
    impl_->notifyMsg = langMsg;
    impl_->healthMsg = healthMsg;

    impl_->quitEvent   = CreateEventW(nullptr, TRUE,  FALSE, nullptr);  // 수동 리셋
    impl_->rescanEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);  // 자동 리셋
    if (impl_->quitEvent != nullptr && impl_->rescanEvent != nullptr) {
        impl_->thread = CreateThread(nullptr, 0, &TrayWatcherImpl::ThreadThunk, impl_, 0, nullptr);
    }
    if (impl_->thread == nullptr) {
        IMI_ERROR(L"TrayIndicatorWatcher: 작업자 스레드 생성 실패 gle=%lu", GetLastError());
        if (impl_->quitEvent   != nullptr) CloseHandle(impl_->quitEvent);
        if (impl_->rescanEvent != nullptr) CloseHandle(impl_->rescanEvent);
        delete impl_;
        impl_ = nullptr;
        return false;
    }
    return true;
}

void TrayIndicatorWatcher::Stop() {
    TrayWatcherImpl* d = impl_;
    if (d == nullptr) return;
    impl_ = nullptr;

    SetEvent(d->quitEvent);
    // 작업자는 프로세스 경계를 넘는 동기 UIA 호출 안(INSIDE)에서 블로킹되어
    // 있을 수 있으므로 즉시 종료가 보장되지 않는다. 제때 끝나지 않으면 —
    // CaretResolver::Stop과 같은 규칙으로 — use-after-free 대신 한도가 정해진
    // 일회성 누수를 택한다(프로세스 종료 시에만 일어난다).
    if (WaitForSingleObject(d->thread, 2000) != WAIT_OBJECT_0) {
        IMI_WARN(L"TrayIndicatorWatcher::Stop: 작업자 스레드가 종료되지 않았습니다."
                 L" use-after-free를 피하기 위해 누수시킵니다.");
        return;
    }
    CloseHandle(d->thread);
    CloseHandle(d->quitEvent);
    CloseHandle(d->rescanEvent);
    delete d;
}

bool TrayIndicatorWatcher::IndicatorFound() const {
    // 작업자 스레드가 갱신하는 플래그의 락 없는 읽기. 진단 표시 전용이라 약간의
    // 지연은 무해하다.
    const TrayWatcherImpl* d = impl_;
    return d != nullptr && d->foundFlag != 0;
}

} // namespace imi
