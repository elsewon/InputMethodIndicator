// resource.h — app.rc용 리소스 id.
#pragma once

// 트레이 컨텍스트 메뉴의 메뉴 명령 id(WM_COMMAND). id는 안정적으로 유지된다 —
// 중간의 빈 번호는 제거된 명령의 흔적이며 의도적으로 재사용하지 않는다.
#define IDM_TRAY_SHOW_DIAGNOSTICS 40002
#define IDM_TRAY_OPEN_LOGFILE     40006
#define IDM_TRAY_START_WITH_WIN   40007
#define IDM_TRAY_ABOUT            40008
#define IDM_TRAY_EXIT             40009
#define IDM_TRAY_TOGGLE_LANG      40010
#define IDM_TRAY_TOGGLE_CAPS      40011
#define IDM_TRAY_TOGGLE_FOCUS     40012
#define IDM_TRAY_RESTART_EXPLORER 40013
#define IDM_TRAY_REPORT_ISSUE     40014

// 선택적 애플리케이션 아이콘. 트레이 아이콘은 런타임에 그리므로 이것은 EXE/윈도우
// 아이콘일 뿐이며, .ico가 존재할 때만 컴파일에 포함된다(app.rc 참고).
#define IDI_APP                   101
