// main.cpp — 프로세스 진입점. 수명 주기 로직은 전부 imi::App::Run에 있고
// (App.cpp 참고), 이 파일은 wWinMain 인자를 그대로 넘기고 --selftest 모드를
// 분기하는 일만 한다.

#include <windows.h>
#include <cwchar>   // wcsstr
#include "App.h"
#include "SelfTest.h"

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE /*hPrevInstance*/,
                    LPWSTR lpCmdLine, int nCmdShow) {
    // --selftest: 인바이너리 로직 테스트를 실행하고 실패 개수를 종료 코드로
    // 반환한다. 창도, 트레이 아이콘도, 단일 인스턴스 가드도 만들지 않는다.
    if (lpCmdLine != nullptr && wcsstr(lpCmdLine, L"--selftest") != nullptr) {
        return imi::RunSelfTests();
    }
    return imi::App().Run(hInstance, lpCmdLine, nCmdShow);
}
