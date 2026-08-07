// CaretResolver.cpp — 캐럿 사각형 대체 체인 (CaretResolver.h 참고).
//
// 1단계(GetGUIThreadInfo)만 호출자의 UI 스레드에서 인라인으로 실행된다 —
// 대상의 협조가 필요 없는 커널 호출이다. 나머지 전부 — MSAA 시스템 캐럿,
// IA2 캐럿 소유자, UIA 단계(TextPattern/TextPattern2, 포커스 요소 경계) — 는
// 전용 MTA 작업자 스레드에서 실행된다. 이들은 모두 포커스된 앱의 UI 스레드가
// 응답해야 돌아오는 프로세스 간 동기 호출이고, 호스트 UI 스레드는
// STA(App.cpp의 CoInitialize(APARTMENTTHREADED))이며 LL 키보드 훅을 소유하므로
// 거기서 기다리면 시스템 전역 입력이 언다. 작업자가 완료 메시지를 post하며,
// UI 스레드는 결코 그것을 기다리지 않는다.
//
// 여기서 만들어지는 모든 사각형은 물리(PHYSICAL) 화면 픽셀이다(앱 전체가
// Per-Monitor-V2를 인식한다). 따라서 논리<->물리 변환은 적용하지 않는다.
// 모니터 DPI는 오버레이 자체의 기하 배율을 위해서만 조회한다.
#include "CaretResolver.h"

#include <windows.h>
// objbase.h는 UIA 헤더보다 반드시 앞서야 한다. 이 프로젝트는
// WIN32_LEAN_AND_MEAN을 정의하므로 windows.h가 ole2.h를 끌어오지 않고, 그것이
// 없으면 `interface` 매크로와 COM 기본 타입이 정의되지 않는다 — 그러면
// UIAutomationCore.h가 `typedef interface IFoo IFoo;`를 default-int로 파싱하며
// 무너진다.
#include <objbase.h>
#include <uiautomation.h>   // IUIAutomation, CLSID_CUIAutomation, UIA_*Id, TextUnit_*
#include <oleacc.h>         // AccessibleObjectFromWindow, IAccessible, OBJID_CARET
#include <servprov.h>       // IServiceProvider (IAccessible2 탐색)
#include <oleauto.h>        // SafeArray*
#include <cwchar>           // wcsrchr
#include <new>              // std::nothrow

#include "ComRef.h"
#include "DpiUtil.h"
#include "Log.h"

namespace imi {

namespace {

// ---------------------------------------------------------------------------
// IAccessible2 (IAccessibleText) — 최소한의 로컬 선언.
//
// IA2 인터페이스는 Windows SDK에 없으므로(Linux Foundation이 관리하는 별도의
// IDL에 있다) 여기서 쓰는 인터페이스 하나만 직접 손으로 선언한다. 메서드
// 순서(ORDER)는 ia2_api_all.idl과 정확히 일치해야 한다 — 그것이 vtable
// 레이아웃을 정의하기 때문이다.
// ---------------------------------------------------------------------------
enum Ia2CoordType { kIa2CoordScreen = 0 };

MIDL_INTERFACE("24FD2FFB-3AAD-4A08-8335-A3AD89C0FB4B")
IAccessibleTextIa2 : public IUnknown {
public:
    virtual HRESULT STDMETHODCALLTYPE addSelection(long, long) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_attributes(long, long*, long*, BSTR*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_caretOffset(long* offset) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_characterExtents(long offset, Ia2CoordType coordType,
                                                           long* x, long* y, long* w, long* h) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_nSelections(long*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_offsetAtPoint(long, long, Ia2CoordType, long*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_selection(long, long*, long*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_text(long, long, BSTR*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_textBeforeOffset(long, long, long*, long*, BSTR*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_textAfterOffset(long, long, long*, long*, BSTR*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_textAtOffset(long, long, long*, long*, BSTR*) = 0;
    virtual HRESULT STDMETHODCALLTYPE removeSelection(long) = 0;
    virtual HRESULT STDMETHODCALLTYPE setCaretOffset(long) = 0;
    virtual HRESULT STDMETHODCALLTYPE setSelection(long, long, long) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_nCharacters(long*) = 0;
    virtual HRESULT STDMETHODCALLTYPE scrollSubstringTo(long, long, int) = 0;
    virtual HRESULT STDMETHODCALLTYPE scrollSubstringToPoint(long, long, Ia2CoordType, long, long) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_newText(void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_oldText(void*) = 0;
};

constexpr IID kIID_IAccessibleText =
    {0x24FD2FFB, 0x3AAD, 0x4A08, {0x83, 0x35, 0xA3, 0xAD, 0x89, 0xC0, 0xFB, 0x4B}};

// 명세에 따른 IA2 탐색 경로: IAccessible -> IServiceProvider ->
// QueryService(IID_IAccessible, <ia2 인터페이스>).
IAccessibleTextIa2* Ia2TextOf(IAccessible* acc) {
    if (acc == nullptr) return nullptr;
    ComRef<IServiceProvider> sp;
    if (FAILED(acc->QueryInterface(IID_IServiceProvider, sp.Void())) || !sp) {
        return nullptr;
    }
    IAccessibleTextIa2* txt = nullptr;
    if (FAILED(sp->QueryService(IID_IAccessible, kIID_IAccessibleText,
                                reinterpret_cast<void**>(&txt)))) {
        return nullptr;
    }
    return txt;   // AddRef된 상태로 반환한다. 호출자가 Release한다
}

// exe/클래스 이름을 위한 서수(ordinal) 기반 대소문자 무시 비교(CRT도 로캘도 쓰지 않는다).
bool EqI(const wchar_t* a, const wchar_t* b) {
    return a != nullptr && b != nullptr &&
           CompareStringOrdinal(a, -1, b, -1, TRUE) == CSTR_EQUAL;
}

// accFocus 링크를 따라 포커스된 말단 accessible까지 내려간다. AddRef된 상태로 반환한다.
IAccessible* MsaaFocusedLeaf(IAccessible* root) {
    IAccessible* cur = root;
    cur->AddRef();
    for (int depth = 0; depth < 16; ++depth) {
        VARIANT v;
        VariantInit(&v);
        if (FAILED(cur->get_accFocus(&v))) break;
        if (v.vt == VT_DISPATCH && v.pdispVal != nullptr) {
            IAccessible* next = nullptr;
            v.pdispVal->QueryInterface(IID_IAccessible, reinterpret_cast<void**>(&next));
            VariantClear(&v);
            if (next == nullptr) break;
            cur->Release();
            cur = next;
            continue;
        }
        VariantClear(&v);
        break;   // CHILDID_SELF(또는 아무것도 아님): cur가 포커스된 객체다
    }
    return cur;
}

// IAccessibleText가 실제로 캐럿을 소유(OWN)하는, |acc|의 가장 깊은 자손
// (|acc| 자신일 수도 있다) — get_caretOffset은 거기서만 S_OK로 답한다.
// |outOffset|을 채운 AddRef된 accessible을 반환하거나, nullptr를 반환한다.
IAccessible* Ia2DeepestCaretOwner(IAccessible* acc, int depth, long* outOffset) {
    if (acc == nullptr || depth > 4) return nullptr;

    bool ownsHere = false;
    long offsetHere = -1;
    {
        IAccessibleTextIa2* txt = Ia2TextOf(acc);
        if (txt != nullptr) {
            long off = -1;
            if (txt->get_caretOffset(&off) == S_OK && off >= 0) {
                ownsHere = true;
                offsetHere = off;
            }
            txt->Release();
        }
    }

    // 자식이 주장하는 소유권이 이 노드의 주장보다 더 정밀하다.
    LONG count = 0;
    if (SUCCEEDED(acc->get_accChildCount(&count)) && count > 0 && count <= 32) {
        VARIANT kids[32];
        LONG got = 0;
        if (SUCCEEDED(AccessibleChildren(acc, 0, count, kids, &got))) {
            IAccessible* found = nullptr;
            for (LONG i = 0; i < got; ++i) {
                if (found == nullptr && kids[i].vt == VT_DISPATCH &&
                    kids[i].pdispVal != nullptr) {
                    IAccessible* child = nullptr;
                    kids[i].pdispVal->QueryInterface(IID_IAccessible,
                                                     reinterpret_cast<void**>(&child));
                    if (child != nullptr) {
                        found = Ia2DeepestCaretOwner(child, depth + 1, outOffset);
                        child->Release();
                    }
                }
                VariantClear(&kids[i]);
            }
            if (found != nullptr) {
                return found;
            }
        }
    }

    if (ownsHere) {
        *outOffset = offsetHere;
        acc->AddRef();
        return acc;
    }
    return nullptr;
}

// |acc|의 첫 자식 요소의 왼쪽 가장자리(화면 픽셀). 플레이스홀더 상황에
// 사용한다. 첫 임베드가 정확히 입력이 시작될 자리에 놓여 있기 때문이다.
bool MsaaFirstChildLeftEdge(IAccessible* acc, RECT& out) {
    LONG count = 0;
    if (FAILED(acc->get_accChildCount(&count)) || count <= 0 || count > 32) {
        return false;
    }
    VARIANT kids[32];
    LONG got = 0;
    if (FAILED(AccessibleChildren(acc, 0, count, kids, &got))) {
        return false;
    }
    bool ok = false;
    for (LONG i = 0; i < got; ++i) {
        if (!ok && kids[i].vt == VT_DISPATCH && kids[i].pdispVal != nullptr) {
            IAccessible* child = nullptr;
            kids[i].pdispVal->QueryInterface(IID_IAccessible,
                                             reinterpret_cast<void**>(&child));
            if (child != nullptr) {
                VARIANT self{};
                self.vt = VT_I4;
                self.lVal = CHILDID_SELF;
                LONG x = 0, y = 0, w = 0, h = 0;
                if (SUCCEEDED(child->accLocation(&x, &y, &w, &h, self)) && h > 0) {
                    out = { x, y, x + 1, y + h };
                    ok = true;
                }
                child->Release();
            }
        }
        VariantClear(&kids[i]);
    }
    return ok;
}

// UIA GetBoundingRectangles()가 준 SAFEARRAY에서 사각형 하나를 읽는다. 이는
// 물리 픽셀 단위로 [left, top, width, height, ...] 형태로 배치된 평탄한 VT_R8
// 배열이며, 범위가 걸친 줄마다 항목이 하나씩 들어 있다. |wantLast|는 첫 항목
// 대신 마지막 항목(캐럿이 놓인 줄)을 고른다. 빈 배열이거나 넓이가 0인
// (화면 밖) 사각형이면 false를 반환한다.
bool RectFromSafeArray(SAFEARRAY* sa, bool wantLast, RECT& out) {
    if (sa == nullptr || SafeArrayGetDim(sa) != 1) {
        return false;
    }
    LONG lb = 0, ub = -1;
    if (FAILED(SafeArrayGetLBound(sa, 1, &lb)) ||
        FAILED(SafeArrayGetUBound(sa, 1, &ub))) {
        return false;
    }
    const LONG count = ub - lb + 1;
    if (count < 4) {
        return false;
    }
    double* data = nullptr;
    if (FAILED(SafeArrayAccessData(sa, reinterpret_cast<void**>(&data)))) {
        return false;
    }
    const LONG base = wantLast ? ((count / 4) - 1) * 4 : 0;
    const double left = data[base], top = data[base + 1];
    const double width = data[base + 2], height = data[base + 3];
    SafeArrayUnaccessData(sa);
    if (width <= 0.0 && height <= 0.0) {
        return false;  // 축소되었거나 화면 밖에 있는 범위
    }
    out.left   = static_cast<LONG>(left);
    out.top    = static_cast<LONG>(top);
    out.right  = static_cast<LONG>(left + width);
    out.bottom = static_cast<LONG>(top + height);
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// Worker — IUIAutomation 클라이언트를 소유하고 한 번에 요청 하나씩 처리하는
// MTA 스레드. 호출자는 goEvent를 신호한 뒤 즉시 돌아가고, 작업자 스레드는 답이
// 준비되면 |notifyMsg|를 post한다. 아무도 이것을 기다리지 않는다.
// ---------------------------------------------------------------------------
struct CaretResolver::Worker {
    HANDLE thread = nullptr;
    HANDLE goEvent = nullptr;    // 호출자 -> 작업자: 요청이 준비됨 (자동 리셋)
    HANDLE quitEvent = nullptr;  // 호출자 -> 작업자: 루프 종료      (수동 리셋)

    HWND notifyWnd = nullptr;    // 작업자 -> 호출자: 완료 post
    UINT notifyMsg = 0;

    CRITICAL_SECTION lock;
    HWND     reqFocus = nullptr;
    HWND     reqForeground = nullptr;
    uint64_t reqGen = 0;         // 요청마다 증가한다. 오래된 답은 버려진다
    uint64_t reqStartTick = 0;
    CaretResult resultData;
    uint64_t resultGen = 0;

    // CreateThread 진입 thunk(이 private 중첩 타입을 이름으로 쓸 수 있도록 멤버로 둔다).
    static DWORD WINAPI ThreadThunk(LPVOID param);
    // 작업자(MTA) 스레드에서 실행된다. quitEvent가 올 때까지 루프를 돈다.
    void ThreadLoop();
    // 살아 있는 IUIAutomation 클라이언트로 작업자 스레드에서 UIA를 통해 해석한다.
    CaretResult ResolveUia(IUIAutomation* uia, HWND focus, HWND foreground);
};

DWORD WINAPI CaretResolver::Worker::ThreadThunk(LPVOID param) {
    static_cast<Worker*>(param)->ThreadLoop();
    return 0;
}

void CaretResolver::Worker::ThreadLoop() {
    // UIA 클라이언트는 MTA 스레드에 있어야 한다. 초기화에 성공한 경우에만
    // CoUninitialize를 짝지어 호출한다.
    const HRESULT hrInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool coInited = SUCCEEDED(hrInit);
    if (!coInited) {
        IMI_ERROR(L"CaretResolver worker: CoInitializeEx(MTA) 실패 hr=0x%08lX", hrInit);
    }

    // 클라이언트를 생성하면 UIA 공급자도 함께 깨어난다(Chromium/Electron은
    // 클라이언트가 존재할 때에만 접근성 트리를 활성화한다). 이 객체를 계속 살려
    // 두는 이유가 그것이다.
    IUIAutomation* uia = nullptr;
    if (coInited) {
        const HRESULT hr = CoCreateInstance(CLSID_CUIAutomation, nullptr,
                                            CLSCTX_INPROC_SERVER,
                                            __uuidof(IUIAutomation),
                                            reinterpret_cast<void**>(&uia));
        if (FAILED(hr) || uia == nullptr) {
            IMI_WARN(L"CaretResolver worker: CoCreateInstance(CUIAutomation) hr=0x%08lX", hr);
            uia = nullptr;
        }
    }

    HANDLE waits[2] = { quitEvent, goEvent };
    for (;;) {
        const DWORD w = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
        if (w == WAIT_OBJECT_0) {
            break;  // 종료
        }
        if (w != WAIT_OBJECT_0 + 1) {
            break;  // 대기 실패/버려짐 — 계속 도는 대신 빠져나간다
        }

        EnterCriticalSection(&lock);
        const HWND focus = reqFocus;
        const HWND foreground = reqForeground;
        const uint64_t gen = reqGen;
        LeaveCriticalSection(&lock);

        // 2-3단계(MSAA 시스템 캐럿, IA2 캐럿 소유자)를 UIA보다 먼저 시도한다
        // (체인 순서와 그 이유는 헤더 참고 — 플레이스홀더의 진실은 IA2에만
        // 있다). 이 둘은 UIA 클라이언트가 없어도 동작한다.
        CaretResult r;
        if (!TryMsaaCaret(focus, r) && !TryIa2Caret(focus, r)) {
            r = (uia != nullptr) ? ResolveUia(uia, focus, foreground)
                                 : CaretResult{};
        }

        bool isCurrent = false;
        EnterCriticalSection(&lock);
        // 이 요청이 실행되는 동안 큐에 들어온 요청이 이것을 대체한다 — 낡은
        // 답을 게시하면 캡슐이 이전 창을 기준으로 배치되어 버린다.
        isCurrent = (gen == reqGen);
        if (isCurrent) {
            resultData = r;
            resultGen = gen;
        }
        LeaveCriticalSection(&lock);

        if (isCurrent && notifyWnd != nullptr) {
            PostMessageW(notifyWnd, notifyMsg, static_cast<WPARAM>(gen), 0);
        }
    }

    if (uia != nullptr) {
        uia->Release();
    }
    if (coInited) {
        CoUninitialize();
    }
}

CaretResult CaretResolver::Worker::ResolveUia(IUIAutomation* uia, HWND focus,
                                              HWND foreground) {
    CaretResult out;  // 사각형을 찾기 전까지는 method None / valid false

    // 먼저 시스템 전역의 포커스 요소를 쓴다(프로세스 경계를 넘어서도 정확하다).
    // 실패하면 넘겨받은 포커스 HWND가 호스팅하는 요소로 대체한다.
    ComRef<IUIAutomationElement> element;
    if (FAILED(uia->GetFocusedElement(&element)) || !element) {
        HWND h = (focus != nullptr) ? focus : foreground;
        if (h == nullptr || FAILED(uia->ElementFromHandle(h, &element)) || !element) {
            return out;
        }
    }

    // 이 요소가 애초에 텍스트를 받아들이기는 하는가? 캐럿 위치를 찾기 전에
    // 먼저 묻는다. 빈 상자에는 캐럿도, 측정할 텍스트도 없지만 그래도 사용자가
    // 곧 입력할 자리이기 때문이다. 모든 반환 경로가 이 값을 담고 나가도록
    // |out|에 미리 설정한다.
    //
    // 읽기 전용(READ-ONLY) 요소는 해당하지 않는다. 웹 페이지의 body(그리고
    // 채팅 기록 같은 읽기 전용 창)는 TextPattern을 지원하고 사용자가 필드
    // 바깥을 클릭하면 포커스를 가져가지만, 그것은 탐색이지 입력할 자리가
    // 아니다. 읽기 전용 여부는 LegacyIAccessible 패턴을 통해 MSAA 상태에서
    // 가져온다 — UIA의 IsReadOnly TEXT 속성은 Chromium에서 신뢰할 수 없는
    // 반면(body를 읽기 전용으로 표시하지 않는다), 접근성 상태(STATE)는 화면
    // 낭독기가 기준으로 삼는 값이며 일관되게 채워진다.
    {
        bool textish = false;
        ComRef<IUIAutomationTextPattern> probe;
        if (SUCCEEDED(element->GetCurrentPatternAs(UIA_TextPatternId,
                                                   __uuidof(IUIAutomationTextPattern),
                                                   probe.Void())) && probe) {
            textish = true;
        } else {
            CONTROLTYPEID ct = 0;
            if (SUCCEEDED(element->get_CurrentControlType(&ct)) &&
                (ct == UIA_EditControlTypeId || ct == UIA_DocumentControlTypeId)) {
                textish = true;
            }
        }
        if (textish) {
            bool readOnly = false;
            ComRef<IUIAutomationLegacyIAccessiblePattern> legacy;
            if (SUCCEEDED(element->GetCurrentPatternAs(
                    UIA_LegacyIAccessiblePatternId,
                    __uuidof(IUIAutomationLegacyIAccessiblePattern),
                    legacy.Void())) && legacy) {
                DWORD state = 0;
                if (SUCCEEDED(legacy->get_CurrentState(&state)) &&
                    (state & STATE_SYSTEM_READONLY) != 0) {
                    readOnly = true;
                }
            }
            out.textInput = !readOnly;
        }
    }

    // 4a단계: TextPattern2::GetCaretRange — 지원된다면 가장 정밀한 캐럿이다.
    {
        ComRef<IUIAutomationTextPattern2> tp2;
        if (SUCCEEDED(element->GetCurrentPatternAs(UIA_TextPattern2Id,
                                                   __uuidof(IUIAutomationTextPattern2),
                                                   tp2.Void())) && tp2) {
            BOOL isActive = FALSE;
            ComRef<IUIAutomationTextRange> caret;
            if (SUCCEEDED(tp2->GetCaretRange(&isActive, &caret)) && caret) {
                SAFEARRAY* sa = nullptr;
                bool ok = false;
                if (SUCCEEDED(caret->GetBoundingRectangles(&sa))) {
                    ok = RectFromSafeArray(sa, /*wantLast*/ false, out.rect);
                    if (sa != nullptr) { SafeArrayDestroy(sa); sa = nullptr; }
                }
                if (!ok) {
                    // degenerate 캐럿 범위는 사각형을 하나도 보고하지 않는
                    // 경우가 많다. 그 위치가 화면상의 사각형을 갖도록 감싸는
                    // 문자 단위로 범위를 넓힌다.
                    if (SUCCEEDED(caret->ExpandToEnclosingUnit(TextUnit_Character)) &&
                        SUCCEEDED(caret->GetBoundingRectangles(&sa))) {
                        ok = RectFromSafeArray(sa, /*wantLast*/ false, out.rect);
                    }
                    if (sa != nullptr) { SafeArrayDestroy(sa); sa = nullptr; }
                }
                IMI_DEBUG(L"CaretResolver: uia caretRange 성공=%d 활성=%d 영역=(%ld,%ld)-(%ld,%ld)",
                          ok ? 1 : 0, isActive ? 1 : 0,
                          out.rect.left, out.rect.top, out.rect.right, out.rect.bottom);
                if (ok) {
                    out.method = CaretMethod::UiaTextPattern;
                    return out;
                }
            } else {
                IMI_DEBUG(L"CaretResolver: uia 캐럿 범위 없음");
            }
        }
    }

    // 4b단계: 삽입 지점 앞(BEFORE)의 텍스트를 측정한다.
    //
    // Win11 XAML 텍스트 상자(탐색기의 주소 표시줄, 이름 바꾸기 상자, 검색)는
    // degenerate range에 대해 사각형을 아예 하나도 돌려주지 않는다 — 그곳에서
    // 캐럿 자체는 측정이 불가능하다. 하지만 그 앞의 텍스트 런은 측정할 수
    // 있다. 캐럿이 놓인 줄을 잡아 삽입 지점까지 잘라내면, 남은 부분의
    // 오른쪽(RIGHT) 가장자리가 정확히 캐럿이 있는 자리다. 문서 전체가 아니라
    // 줄 단위로 범위를 좁히므로, 큰 문서에서도 비용이 O(한 줄)로 유지된다.
    //
    // 삽입 지점은 선택 영역의 시작(START)이다 — 선택이 맨 캐럿이든 텍스트를
    // 걸치든, 입력은 그 자리에 떨어진다(주소 표시줄은 포커스를 받으면 경로
    // 전체를 선택한다).
    {
        ComRef<IUIAutomationTextPattern> tp;
        if (SUCCEEDED(element->GetCurrentPatternAs(UIA_TextPatternId,
                                                   __uuidof(IUIAutomationTextPattern),
                                                   tp.Void())) && tp) {
            ComRef<IUIAutomationTextRangeArray> sel;
            int len = 0;
            if (SUCCEEDED(tp->GetSelection(&sel)) && sel &&
                SUCCEEDED(sel->get_Length(&len)) && len > 0) {
                ComRef<IUIAutomationTextRange> caret;
                if (SUCCEEDED(sel->GetElement(0, &caret)) && caret) {
                    ComRef<IUIAutomationTextRange> line;
                    if (SUCCEEDED(caret->Clone(&line)) && line) {
                        line->ExpandToEnclosingUnit(TextUnit_Line);

                        // [줄 시작, 캐럿) — 그 오른쪽 가장자리가 곧 캐럿이다.
                        ComRef<IUIAutomationTextRange> pre;
                        if (SUCCEEDED(line->Clone(&pre)) && pre) {
                            pre->MoveEndpointByRange(TextPatternRangeEndpoint_End,
                                                     caret.p, TextPatternRangeEndpoint_Start);
                            SAFEARRAY* sa = nullptr;
                            RECT run{};
                            bool ok = false;
                            if (SUCCEEDED(pre->GetBoundingRectangles(&sa))) {
                                ok = RectFromSafeArray(sa, /*wantLast*/ true, run);
                            }
                            if (sa != nullptr) { SafeArrayDestroy(sa); }
                            if (ok) {
                                IMI_DEBUG(L"CaretResolver: uia 앞부분 런=(%ld,%ld)-(%ld,%ld) -> 캐럿 x=%ld",
                                          run.left, run.top, run.right, run.bottom, run.right);
                                out.rect   = { run.right, run.top, run.right, run.bottom };
                                out.method = CaretMethod::UiaTextPattern;
                                return out;
                            }
                        }

                        // 캐럿 앞에 아무것도 없다. 캐럿이 줄의 시작에 있다는
                        // 뜻이므로 그 줄의 왼쪽(LEFT) 가장자리에 앵커링한다.
                        SAFEARRAY* sa = nullptr;
                        RECT run{};
                        bool ok = false;
                        if (SUCCEEDED(line->GetBoundingRectangles(&sa))) {
                            ok = RectFromSafeArray(sa, /*wantLast*/ false, run);
                        }
                        if (sa != nullptr) { SafeArrayDestroy(sa); }
                        if (ok) {
                            IMI_DEBUG(L"CaretResolver: uia 줄 시작 런=(%ld,%ld)-(%ld,%ld)",
                                      run.left, run.top, run.right, run.bottom);
                            out.rect   = { run.left, run.top, run.left, run.bottom };
                            out.method = CaretMethod::UiaTextPattern;
                            return out;
                        }
                        IMI_DEBUG(L"CaretResolver: uia 측정 가능한 텍스트 없음");
                    }
                }
            }
        }
    }

    // 5단계: 쓸 만한 텍스트 패턴이 없다 — 포커스 요소의 바운딩 사각형에
    // 앵커링한다. 이 요소는 캐럿이 아니라 상자 전체이므로, 앵커를 왼쪽(LEFT)
    // 끝(텍스트가 시작되는 지점)으로 좁힌다. 그러지 않으면 오버레이가 상자
    // 한가운데 아래에 캡슐을 중앙 정렬해 버린다.
    RECT rc{};
    if (SUCCEEDED(element->get_CurrentBoundingRectangle(&rc)) &&
        rc.right > rc.left && rc.bottom > rc.top) {
        IMI_DEBUG(L"CaretResolver: uia 요소 경계=(%ld,%ld)-(%ld,%ld)",
                  rc.left, rc.top, rc.right, rc.bottom);
        const LONG boxH = rc.bottom - rc.top;
        if (rc.right - rc.left > boxH) {
            rc.right = rc.left + boxH;
        }
        out.rect = rc;
        out.method = CaretMethod::UiaFocusedElement;
        return out;
    }

    return out;
}

// ---------------------------------------------------------------------------
// CaretResolver
// ---------------------------------------------------------------------------

// 소멸 시점에 Worker가 완전한 타입이 되도록 여기에 정의한다.
CaretResolver::~CaretResolver() {
    Stop();
}

bool CaretResolver::Start(HWND notifyWnd, UINT notifyMsg) {
    if (worker_ != nullptr) {
        return true;  // 이미 실행 중이다
    }

    Worker* w = new (std::nothrow) Worker();
    if (w == nullptr) {
        return false;
    }
    InitializeCriticalSection(&w->lock);
    w->notifyWnd = notifyWnd;
    w->notifyMsg = notifyMsg;
    w->goEvent   = CreateEventW(nullptr, FALSE, FALSE, nullptr);  // 자동 리셋
    w->quitEvent = CreateEventW(nullptr, TRUE,  FALSE, nullptr);  // 수동 리셋

    if (w->goEvent == nullptr || w->quitEvent == nullptr) {
        IMI_ERROR(L"CaretResolver::Start: CreateEvent 실패 gle=%lu", GetLastError());
    } else {
        w->thread = CreateThread(nullptr, 0, &Worker::ThreadThunk, w, 0, nullptr);
        if (w->thread == nullptr) {
            IMI_ERROR(L"CaretResolver::Start: CreateThread 실패 gle=%lu", GetLastError());
        }
    }

    if (w->thread == nullptr) {
        // 절반만 만들어진 작업자를 정리한다. 빠른 (인라인) 경로는 여전히 동작한다.
        if (w->goEvent   != nullptr) { CloseHandle(w->goEvent); }
        if (w->quitEvent != nullptr) { CloseHandle(w->quitEvent); }
        DeleteCriticalSection(&w->lock);
        delete w;
        return false;
    }

    worker_ = w;
    return true;
}

void CaretResolver::Stop() {
    Worker* w = worker_;
    if (w == nullptr) {
        return;
    }
    worker_ = nullptr;

    if (w->quitEvent != nullptr) {
        SetEvent(w->quitEvent);
    }
    if (w->thread != nullptr) {
        // 작업자 스레드는 프로세스 경계를 넘는 동기 UIA 호출 안(INSIDE)에서
        // 블로킹되어 있을 수 있으므로(GetFocusedElement / GetCaretRange /
        // GetBoundingRectangles 모두 공급자에서 블로킹된다), quitEvent를
        // 신호했다고 해서 즉시 종료가 보장되지는 않는다 — 멈춘 공급자가
        // 타임아웃을 넘겨 스레드를 붙잡고 있을 수 있다. 스레드가 아직 살아
        // 있는데 Worker(핸들, 크리티컬 섹션, 힙)를 해제하면 use-after-free /
        // use-after-close가 되므로, 타임아웃 시에는 의도적으로 누수(LEAK)시킨다.
        // 이는 한도가 정해진 일회성 누수다. 프로세스 종료 시에만 일어난다.
        if (WaitForSingleObject(w->thread, 2000) != WAIT_OBJECT_0) {
            IMI_WARN(L"CaretResolver::Stop: UIA 작업자 스레드가 종료되지 않았습니다. use-after-free를 피하기 위해 누수시킵니다.");
            return;
        }
        CloseHandle(w->thread);
    }
    if (w->goEvent   != nullptr) { CloseHandle(w->goEvent); }
    if (w->quitEvent != nullptr) { CloseHandle(w->quitEvent); }
    DeleteCriticalSection(&w->lock);
    delete w;
}

// 포커스된 HWND가 캐럿 조회와 예외 처리(quirk) 표를 모두 결정한다.
static HWND FocusedHwndOf(HWND foreground, DWORD threadId) {
    GUITHREADINFO gti{};
    gti.cbSize = sizeof(gti);
    if (GetGUIThreadInfo(threadId, &gti) && gti.hwndFocus != nullptr) {
        return gti.hwndFocus;
    }
    return foreground;
}

void CaretResolver::Finalise(HWND focus, uint64_t startTick, CaretResult& io) {
    ApplyPerAppQuirks(focus, io);
    // 물리 픽셀 사각형을 손에 쥐었으니, 그것이 놓이는 모니터의 DPI를 보고한다.
    io.dpi = DpiForPoint(POINT{ io.rect.left, io.rect.top });
    io.valid = true;
    io.elapsedMs = static_cast<uint32_t>(GetTickCount64() - startTick);
}

bool CaretResolver::TryFast(HWND foreground, DWORD threadId, CaretResult& out) {
    const uint64_t startTick = GetTickCount64();
    const HWND focus = FocusedHwndOf(foreground, threadId);

    // 1단계: Win32 캐럿 (EDIT/RichEdit). 유일한 UI 스레드 인라인 단계다.
    // 2단계부터는 포커스된 앱이 응답해야 돌아오는 프로세스 간 동기 COM이라
    // 작업자 스레드에서 실행된다(RequestAsync) — 바쁜 앱이 우리 UI 스레드와
    // LL 키보드 훅을 얼리지 못하게 하기 위해서다.
    if (TryGuiThreadInfo(foreground, threadId, out)) {
        Finalise(focus, startTick, out);
        return true;
    }
    return false;
}

uint64_t CaretResolver::RequestAsync(HWND foreground, DWORD threadId) {
    Worker* w = worker_;
    if (w == nullptr || w->thread == nullptr || w->goEvent == nullptr) {
        return 0;
    }
    const HWND focus = FocusedHwndOf(foreground, threadId);

    uint64_t myGen = 0;
    EnterCriticalSection(&w->lock);
    w->reqFocus = focus;
    w->reqForeground = foreground;
    w->reqStartTick = GetTickCount64();
    myGen = ++w->reqGen;      // 아직 진행 중인 요청을 모두 대체한다
    LeaveCriticalSection(&w->lock);

    SetEvent(w->goEvent);     // 즉시 반환한다. 작업자 스레드가 끝나면 post한다
    return myGen;
}

bool CaretResolver::TakeResult(uint64_t id, CaretResult& out) {
    Worker* w = worker_;
    if (w == nullptr || id == 0) {
        return false;
    }
    HWND focus = nullptr;
    uint64_t startTick = 0;
    bool ok = false;

    EnterCriticalSection(&w->lock);
    if (w->resultGen == id && w->reqGen == id &&
        w->resultData.method != CaretMethod::None) {
        out = w->resultData;
        focus = w->reqFocus;
        startTick = w->reqStartTick;
        ok = true;
    }
    LeaveCriticalSection(&w->lock);

    if (ok) {
        Finalise(focus, startTick, out);
    }
    return ok;
}


bool CaretResolver::TryGuiThreadInfo(HWND foreground, DWORD threadId, CaretResult& out) {
    (void)foreground;  // 캐럿 자신의 hwnd(gti.hwndCaret)가 정답이다
    GUITHREADINFO gti{};
    gti.cbSize = sizeof(gti);
    if (!GetGUIThreadInfo(threadId, &gti)) {
        return false;
    }
    // 캐럿을 호스팅하는 창과, degenerate하지 않은 캐럿 높이가 필요하다.
    // rcCaret은 캐럿 창의 클라이언트 좌표계다.
    if (gti.hwndCaret == nullptr || gti.rcCaret.bottom <= gti.rcCaret.top) {
        return false;
    }

    POINT tl{ gti.rcCaret.left, gti.rcCaret.top };
    POINT br{ gti.rcCaret.right, gti.rcCaret.bottom };
    if (!ClientToScreen(gti.hwndCaret, &tl) || !ClientToScreen(gti.hwndCaret, &br)) {
        return false;
    }
    out.rect = { tl.x, tl.y, br.x, br.y };
    out.method = CaretMethod::GuiThreadInfo;
    out.textInput = true;   // 살아 있는 Win32 캐럿은 곧 텍스트 컨트롤이라는 뜻이다
    return true;
}

bool CaretResolver::TryMsaaCaret(HWND focus, CaretResult& out) {
    if (focus == nullptr) {
        return false;
    }
    IAccessible* acc = nullptr;
    // OBJID_CARET은 (음수) LONG으로 선언되어 있지만 매개변수는 DWORD다. 계약은
    // 비트 패턴이므로, 경고(C4245)를 내는 대신 명시적으로 캐스팅한다.
    const HRESULT hr = AccessibleObjectFromWindow(focus, static_cast<DWORD>(OBJID_CARET),
                                                  IID_IAccessible,
                                                  reinterpret_cast<void**>(&acc));
    if (FAILED(hr) || acc == nullptr) {
        return false;
    }

    VARIANT self{};
    self.vt = VT_I4;
    self.lVal = CHILDID_SELF;

    // 보이지 않는(INVISIBLE) 캐럿은 낡은 캐럿이다. Chromium은 포커스가 읽기
    // 전용 페이지 body로 옮겨간 뒤에도 시스템 캐럿 객체를 살려 둔다(숨긴 채
    // 마지막 텍스트 필드에 세워 둔다) — 거기에 앵커링하면 이미 포커스를 잃은
    // 필드에 캡슐이 고정되어 버린다.
    VARIANT state{};
    VariantInit(&state);
    if (SUCCEEDED(acc->get_accState(self, &state)) && state.vt == VT_I4 &&
        (state.lVal & STATE_SYSTEM_INVISIBLE) != 0) {
        VariantClear(&state);
        acc->Release();
        return false;
    }
    VariantClear(&state);

    LONG x = 0, y = 0, w = 0, h = 0;
    const HRESULT hrLoc = acc->accLocation(&x, &y, &w, &h, self);
    acc->Release();

    if (FAILED(hrLoc) || h <= 0) {
        return false;  // 노출된 캐럿이 없다(많은 컨트롤이 E_FAIL이나 높이 0을 반환한다)
    }
    // accLocation은 물리 픽셀 단위의 화면 좌표를 준다. 가는 캐럿이면 너비가
    // 0일 수 있지만, 배치에는 문제가 없다.
    out.rect = { x, y, x + (w > 0 ? w : 1), y + h };
    out.method = CaretMethod::MsaaCaret;
    out.textInput = true;   // 노출된 캐럿은 곧 텍스트 컨트롤이라는 뜻이다
    return true;
}

bool CaretResolver::TryIa2Caret(HWND focus, CaretResult& out) {
    if (focus == nullptr) {
        return false;
    }
    ComRef<IAccessible> client;
    if (FAILED(AccessibleObjectFromWindow(focus, static_cast<DWORD>(OBJID_CLIENT),
                                          IID_IAccessible, client.Void())) ||
        !client) {
        return false;
    }

    IAccessible* leaf = MsaaFocusedLeaf(client.p);
    if (leaf == nullptr) {
        return false;
    }

    // 읽기 전용(READ-ONLY) 객체에 포커스가 있는 것은(필드 바깥을 클릭한 뒤의
    // 페이지 body) 입력이 아니라 탐색이다 — 그 "캐럿"은 삽입 지점이 아니라
    // 선택 지점이며, 포커스 캡슐이 거기서 뜨면 안 된다.
    {
        VARIANT self{};
        self.vt = VT_I4;
        self.lVal = CHILDID_SELF;
        VARIANT state{};
        VariantInit(&state);
        if (SUCCEEDED(leaf->get_accState(self, &state)) && state.vt == VT_I4 &&
            (state.lVal & STATE_SYSTEM_READONLY) != 0) {
            VariantClear(&state);
            leaf->Release();
            return false;
        }
        VariantClear(&state);
    }

    bool ok = false;
    long caret = -1;
    IAccessible* owner = Ia2DeepestCaretOwner(leaf, 0, &caret);
    if (owner != nullptr) {
        out.textInput = true;   // 캐럿을 소유한 객체는 텍스트를 받아들인다

        if (owner != leaf) {
            // 진짜 텍스트 노드가 캐럿을 소유한다. 캐럿 오프셋에서의 문자 범위
            // (character extents)가 곧 캐럿 위치다(Chromium은 거기서
            // degenerate한 사각형을 보고한다. 텍스트 끝에서는 앞 문자의
            // 오른쪽 가장자리로 대체한다).
            IAccessibleTextIa2* txt = Ia2TextOf(owner);
            if (txt != nullptr) {
                long x = 0, y = 0, w = 0, h = 0;
                if (txt->get_characterExtents(caret, kIa2CoordScreen,
                                              &x, &y, &w, &h) == S_OK && h > 0) {
                    out.rect = { x, y, x + 1, y + h };
                    ok = true;
                } else if (caret > 0 &&
                           txt->get_characterExtents(caret - 1, kIa2CoordScreen,
                                                     &x, &y, &w, &h) == S_OK && h > 0) {
                    out.rect = { x + w, y, x + w + 1, y + h };
                    ok = true;
                }
                txt->Release();
            }
        } else {
            // 컨테이너(CONTAINER)만 캐럿을 주장하고 어떤 자손 텍스트 노드도
            // 주장하지 않는다 — 그 앞의 내용은 텍스트가 아니라 장식이다.
            // 플레이스홀더가 있는 빈 Chromium 필드 상태가 바로 이것이다.
            // 캐럿은 플레이스홀더 임베드 뒤에 있는 것처럼 읽히지만 입력은
            // 임베드의 왼쪽(LEFT) 가장자리에서 시작되므로, 거기에 앵커링한다.
            RECT r{};
            if (MsaaFirstChildLeftEdge(leaf, r)) {
                out.rect = r;
                ok = true;
            } else {
                // 자식이 아예 없다(정말로 비어 있는 단순 필드). 이때는 캐럿
                // 위치에서의 컨테이너 자신의 문자 범위를 신뢰할 수 있다.
                IAccessibleTextIa2* txt = Ia2TextOf(owner);
                if (txt != nullptr) {
                    long x = 0, y = 0, w = 0, h = 0;
                    if (txt->get_characterExtents(caret, kIa2CoordScreen,
                                                  &x, &y, &w, &h) == S_OK && h > 0) {
                        out.rect = { x, y, x + 1, y + h };
                        ok = true;
                    }
                    txt->Release();
                }
            }
        }
        owner->Release();
    }
    leaf->Release();

    if (ok) {
        out.method = CaretMethod::Ia2Caret;
    }
    return ok;
}

void CaretResolver::ApplyPerAppQuirks(HWND focus, CaretResult& io) {
    if (focus == nullptr) {
        return;
    }

    // 포커스된 창의 프로세스의 exe 파일명(basename).
    wchar_t exe[MAX_PATH] = L"";
    DWORD pid = 0;
    GetWindowThreadProcessId(focus, &pid);
    if (pid != 0) {
        HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (proc != nullptr) {
            wchar_t path[MAX_PATH] = L"";
            DWORD n = MAX_PATH;
            if (QueryFullProcessImageNameW(proc, 0, path, &n) && n > 0) {
                const wchar_t* slash = wcsrchr(path, L'\\');
                const wchar_t* base = (slash != nullptr) ? slash + 1 : path;
                lstrcpynW(exe, base, MAX_PATH);
            }
            CloseHandle(proc);
        }
    }

    wchar_t cls[128] = L"";
    GetClassNameW(focus, cls, 128);

    // 작은 예외 처리(quirk) 표: (exe, 클래스 또는 null) -> 물리 픽셀 보정값.
    // 보고하는 캐럿 사각형이 실제 글리프 위치와 몇 픽셀 어긋나는 앱을 키로 한다.
    struct Quirk { const wchar_t* exe; const wchar_t* cls; int dx; int dy; };
    static const Quirk kQuirks[] = {
        // VS Code / Electron: Chromium의 캐럿 사각형이 render-widget 호스트에서
        // 아주 살짝 위로 읽힌다. 캡슐이 텍스트 줄에 걸리지 않도록 아래로 민다.
        { L"Code.exe",            L"Chrome_RenderWidgetHostHWND", 0, 2 },
        { L"electron.exe",        L"Chrome_RenderWidgetHostHWND", 0, 2 },
        // Windows Terminal: 셀 기반 캐럿이 위쪽 여백을 포함하지 않는다.
        { L"WindowsTerminal.exe", nullptr,                        0, 1 },
    };

    for (const Quirk& q : kQuirks) {
        if (EqI(exe, q.exe) && (q.cls == nullptr || EqI(cls, q.cls))) {
            OffsetRect(&io.rect, q.dx, q.dy);
            break;
        }
    }
}

} // namespace imi
