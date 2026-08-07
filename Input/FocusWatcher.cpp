// FocusWatcher.cpp — FocusWatcher.h 참고.
//
// 전역 UIA 포커스 변경 구독. 핸들러는 의도적으로 PostMessage 외에는 아무것도
// 하지 않는다. 핸들러는 UIA 스레드 풀 스레드에서 실행되며, UIA 콜백 안에서
// UIA를 호출하면 공급자에 재진입할 위험이 있기 때문이다.
//
// 구독은 이 클래스가 소유한 MTA 작업자 스레드에서 걸고 푼다.
// AddFocusChangedEventHandler는 시스템 전역의 공급자들을 구체화하는 무거운
// 호출로, 병든 explorer 접근성 트리에서는 수 분(실측 337초)까지 걸린 것이
// 측정되었다(KNOWN-LIMITS의 "explorer 접근성 트리가 병듦" 참고). 호스트 UI
// 스레드는 WH_KEYBOARD_LL 훅을 소유하므로 거기서 그만큼 블로킹하면 시스템
// 전역 입력이 언다. RemoveFocusChangedEventHandler도 같은 이유로 작업자에서
// 실행한다.
#include "FocusWatcher.h"

#include <windows.h>
#include <objbase.h>          // uiautomation.h보다 먼저 와야 한다(COM 기반 / `interface`)
#include <uiautomation.h>
#include <new>                // std::nothrow

#include "Log.h"

namespace imi {

// ---------------------------------------------------------------------------
// UIA 포커스 변경 핸들러. UIA 스레드 풀 스레드에서 실행되며 상태를 건드리지 않는다.
// ---------------------------------------------------------------------------
class FocusHandler : public IUIAutomationFocusChangedEventHandler {
public:
    FocusHandler(HWND wnd, UINT msg) : wnd_(wnd), msg_(msg) {}

    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&ref_); }
    ULONG STDMETHODCALLTYPE Release() override {
        const LONG r = InterlockedDecrement(&ref_);
        if (r == 0) delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (ppv == nullptr) return E_POINTER;
        if (riid == __uuidof(IUnknown) ||
            riid == __uuidof(IUIAutomationFocusChangedEventHandler)) {
            *ppv = static_cast<IUIAutomationFocusChangedEventHandler*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE HandleFocusChangedEvent(
        IUIAutomationElement* /*sender*/) override {
        PostMessageW(wnd_, msg_, 0, 0);
        return S_OK;
    }

private:
    LONG ref_ = 1;
    HWND wnd_;
    UINT msg_;
};

// ---------------------------------------------------------------------------
struct FocusWatcherImpl {
    // 작업자 스레드가 소유하는 UIA 상태(생성부터 해제까지 전부 그 스레드에서만).
    IUIAutomation* uia = nullptr;
    FocusHandler*  handler = nullptr;
    bool           subscribed = false;
    HWND           notifyWnd = nullptr;
    UINT           notifyMsg = 0;

    // 스레딩. UI 스레드가 Start에서 만들고 작업자와 공유한다.
    HANDLE         thread = nullptr;
    HANDLE         quitEvent = nullptr;   // 수동 리셋: 작업자 종료 신호

    static DWORD WINAPI ThreadThunk(LPVOID param);
    void ThreadLoop();   // 작업자(MTA) 스레드에서 실행된다
};

DWORD WINAPI FocusWatcherImpl::ThreadThunk(LPVOID param) {
    static_cast<FocusWatcherImpl*>(param)->ThreadLoop();
    return 0;
}

void FocusWatcherImpl::ThreadLoop() {
    // 다른 UIA 작업자들과 같은 규칙: 클라이언트는 MTA에 두고, 콜백은 UIA
    // 스레드 풀에서 오므로 메시지 펌프가 필요 없다.
    const HRESULT hrInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool coInited = SUCCEEDED(hrInit);
    if (!coInited) {
        IMI_ERROR(L"FocusWatcher: CoInitializeEx(MTA) 실패 hr=0x%08lX",
                  static_cast<unsigned long>(hrInit));
    }

    if (coInited) {
        const HRESULT hr = CoCreateInstance(CLSID_CUIAutomation, nullptr,
                                            CLSCTX_INPROC_SERVER, __uuidof(IUIAutomation),
                                            reinterpret_cast<void**>(&uia));
        if (FAILED(hr) || uia == nullptr) {
            IMI_ERROR(L"FocusWatcher: CoCreateInstance(CUIAutomation) hr=0x%08lX",
                      static_cast<unsigned long>(hr));
            uia = nullptr;
        }
    }

    if (uia != nullptr) {
        handler = new (std::nothrow) FocusHandler(notifyWnd, notifyMsg);
        if (handler != nullptr) {
            // 소요 시간을 기록한다: 건강한 시스템은 수백 ms, 병든 explorer는 수
            // 분까지 측정되었다. 이 호출은 중단할 수 없으므로, 그 사이 Stop()이
            // 오면 Stop 쪽이 기다리다 타임아웃하고 누수를 택한다 — 호출이
            // 끝나면 아래에서 종료 신호를 보고 스스로 정리한다.
            const uint64_t subStart = GetTickCount64();
            const HRESULT hrSub = uia->AddFocusChangedEventHandler(nullptr, handler);
            const unsigned long long subMs = GetTickCount64() - subStart;
            subscribed = SUCCEEDED(hrSub);
            if (subscribed) {
                IMI_INFO(L"FocusWatcher: UIA 포커스 구독이 활성화되었습니다 (%llums).",
                         subMs);
            } else {
                IMI_ERROR(L"FocusWatcher: AddFocusChangedEventHandler hr=0x%08lX (%llums)",
                          static_cast<unsigned long>(hrSub), subMs);
            }
        } else {
            IMI_ERROR(L"FocusWatcher: 핸들러 할당 실패. 포커스 구독이 비활성화됩니다.");
        }
    }

    WaitForSingleObject(quitEvent, INFINITE);

    if (subscribed && uia != nullptr && handler != nullptr) {
        uia->RemoveFocusChangedEventHandler(handler);   // 이 해지도 느릴 수 있다
        subscribed = false;
    }
    if (handler != nullptr) { handler->Release(); handler = nullptr; }
    if (uia     != nullptr) { uia->Release();     uia     = nullptr; }
    if (coInited) CoUninitialize();
}

FocusWatcher::~FocusWatcher() { Stop(); }

bool FocusWatcher::Start(HWND notifyWnd, UINT notifyMsg) {
    if (impl_ != nullptr) return true;

    impl_ = new (std::nothrow) FocusWatcherImpl();
    if (impl_ == nullptr) return false;
    impl_->notifyWnd = notifyWnd;
    impl_->notifyMsg = notifyMsg;

    impl_->quitEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);  // 수동 리셋
    if (impl_->quitEvent != nullptr) {
        impl_->thread = CreateThread(nullptr, 0, &FocusWatcherImpl::ThreadThunk, impl_, 0, nullptr);
    }
    if (impl_->thread == nullptr) {
        IMI_ERROR(L"FocusWatcher: 작업자 스레드 생성 실패 gle=%lu", GetLastError());
        if (impl_->quitEvent != nullptr) CloseHandle(impl_->quitEvent);
        delete impl_;
        impl_ = nullptr;
        return false;
    }
    return true;
}

void FocusWatcher::Stop() {
    FocusWatcherImpl* d = impl_;
    if (d == nullptr) return;
    impl_ = nullptr;

    SetEvent(d->quitEvent);
    // 작업자는 AddFocusChangedEventHandler 안에서 수 분씩 블로킹되어 있을 수
    // 있다(병든 explorer). 제때 끝나지 않으면 다른 작업자들과 같은 규칙으로
    // use-after-free 대신 누수를 택한다. 이 함수는 설정 토글로도 호출되므로
    // 누수가 반복될 수 있지만, 스레드는 느린 호출이 끝나는 즉시 종료 신호를
    // 보고 스스로 구독을 해지하고 종료하므로 남는 것은 핸들 두 개와 impl
    // 메모리뿐이다.
    if (WaitForSingleObject(d->thread, 2000) != WAIT_OBJECT_0) {
        IMI_WARN(L"FocusWatcher::Stop: 작업자 스레드가 종료되지 않았습니다."
                 L" use-after-free를 피하기 위해 누수시킵니다.");
        return;
    }
    CloseHandle(d->thread);
    CloseHandle(d->quitEvent);
    delete d;
    IMI_INFO(L"FocusWatcher: 중지되었습니다.");
}

} // namespace imi
