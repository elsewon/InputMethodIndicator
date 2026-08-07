// ComRef.h — UIA를 다루는 코드(CaretResolver, TrayIndicatorWatcher)가 공유하는
// 최소한의 COM 인터페이스 스코프 가드.
//
// 의도적으로 완전한 스마트 포인터가 아니다: 복사/이동도, 암묵적 변환도 없다.
// 이 파일들이 사용하는 CoCreate / out 파라미터 호출 패턴 주변에서 이른 반환이
// 일어날 때마다 Release()를 보장하기 위해서만 존재한다.
#pragma once

#include <unknwn.h>

namespace imi {

template <class T>
struct ComRef {
    T* p = nullptr;

    ComRef() = default;
    ComRef(const ComRef&) = delete;
    ComRef& operator=(const ComRef&) = delete;
    ~ComRef() { if (p != nullptr) { p->Release(); } }

    T*  operator->() const { return p; }
    T** operator&()        { return &p; }            // T** 형태의 out 파라미터용
    void** Void()          { return reinterpret_cast<void**>(&p); }
    explicit operator bool() const { return p != nullptr; }

    // (여전히 AddRef된) 인터페이스를 소유권을 가져가는 호출자에게 넘긴다.
    T* Detach() {
        T* out = p;
        p = nullptr;
        return out;
    }
};

} // namespace imi
