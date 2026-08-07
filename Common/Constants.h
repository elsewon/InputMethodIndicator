// Constants.h — 앱의 윈도우 클래스 이름, id, 메시지.
//
// 입력 상태는 UI Automation으로 OS 입력 표시기를 관찰해서 얻는다
// (Input/TrayIndicatorWatcher 참고). 이 파일은 앱 자신의 윈도우 / 타이머 /
// 메시지 식별자를 담는다.
#pragma once

#include <windows.h>

namespace imi {

// 앱이 소유하는 윈도우 클래스.
inline constexpr const wchar_t* kOverlayWindowClass = L"IMI_Overlay_v1";
inline constexpr const wchar_t* kTrayWindowClass    = L"IMI_Tray_v1";

// 단일 인스턴스 가드(세션 로컬이라 Global\\ 권한이 전혀 필요 없다).
inline constexpr const wchar_t* kSingleInstanceMutex = L"Local\\IMI_SingleInstance_v1";

// 타이머 id.
enum : UINT {
    kTimer_OverlayFade = 1,   // 오버레이 페이드 애니메이션 tick (오버레이 윈도우)
    kTimer_CapsIdle    = 2,   // caps 표시 입력 중 숨김 폴링 (트레이 윈도우)
    kTimer_FocusShow   = 3,   // 일회성: 포커스 이벤트를 안정시킨 뒤 캡슐을 표시
    kTimer_AccessibilityPoke = 4,   // 주기적: Chromium 렌더러의 지연 접근성을 깨운다
    kTimer_CaretTimeout = 5,  // 일회성: 느린 캐럿 확인을 포기한다
};

// 트레이 아이콘 콜백 메시지(WM_APP 대역)와 아이콘 id.
inline constexpr UINT kTrayCallbackMessage = WM_APP + 1;
inline constexpr UINT kTrayIconId = 1;

// 트레이 윈도우로 보내는 사용자 정의 메시지. 지연 시간 규칙: OS 콜백(LL 키보드 훅,
// UIA 속성 변경 처리기)은 무거운 작업을 그 자리에서 하면 안 되므로, 값만 넣어 두고
// 이 중 하나를 PostMessage 한다. 비싼 경로(상태 기계 -> 캐럿 -> 오버레이)는 그 뒤
// 일반 메시지 루프에서 실행된다.
enum : UINT {
    kMsg_CapsChanged     = WM_APP + 14, // wParam = capsOn (0/1); LL 훅에서 온다
    kMsg_TrayLangChanged = WM_APP + 16, // wParam = Language;      트레이 감시자에서 온다
    kMsg_SecondInstance  = WM_APP + 17, // 두 번째 인스턴스가 실행되었다(그리고 종료됨)
    kMsg_FocusChanged    = WM_APP + 18, // UIA 포커스가 이동; 포커스 감시자에서 온다
    kMsg_CaretResolved   = WM_APP + 19, // wParam = 요청 id; 캐럿 작업자 스레드에서 온다
    kMsg_TrayWatcherHealth = WM_APP + 20, // wParam = TrayWatcherHealth; 트레이 감시자 작업자에서 온다
    kMsg_WatchdogPing    = WM_APP + 21, // UI 스레드 응답성 프로브; 감시견 스레드가 SendMessageTimeout으로 보낸다
};

} // namespace imi
