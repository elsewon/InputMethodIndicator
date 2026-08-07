// SelfTest.h — 인바이너리 로직 테스트. 실행 방법:
//   InputMethodIndicator.exe --selftest
// 창/COM 준비 없이 순수 로직(트레이 Name 파서, 상태 기계의 억제 규칙)만 다룬다.
// 실패 개수(0 == 통과)를 종료 코드로 반환한다.
#pragma once

namespace imi {

int RunSelfTests();

} // namespace imi
