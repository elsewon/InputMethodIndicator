// TrayIcon.h — Shell_NotifyIcon(v4) 래퍼 + 트레이 컨텍스트 메뉴 + 간단한
// 풍선 알림 헬퍼.
//
// 아이콘과 툴팁은 둘 다 **고정**이다. 아이콘은 Create에서 한 번 그리는 "IMI"
// 캡슐이고, 툴팁은 제품명뿐이다. 입력 상태는 여기가 아니라 캐럿 근처 캡슐이
// 보여주므로 갱신할 것이 없다. 다만 explorer가 재시작되면 셸이 트레이 등록을
// 잃으므로, 호스트가 TaskbarCreated를 받아 Restore()로 다시 등록해야 한다.
#pragma once

#include <windows.h>

namespace imi {

// 컨텍스트 메뉴의 체크박스 상태(Config를 반영한다).
struct TrayMenuState {
    bool notifyLanguage    = true;
    bool notifyCapsLock    = true;
    bool notifyFocusChange = false;
    bool startWithWindows  = false;
    // 표시기를 오래 읽지 못하는 이상 상태다. 켜지면 진단 도구(로그 파일 열기)
    // 아래에 "탐색기 재시작 필요" 조치 항목을 노출한다(토스트가 억제/만료됐을
    // 때의 지속적 대안).
    bool explorerRestartHint = false;
};

// 트레이 콜백이 요청하는 동작(owner가 수행한다).
enum class TrayAction {
    None,
    ContextMenu,        // 우클릭 / 키보드 선택 → 메뉴를 띄운다
    NotificationClick,  // 풍선/토스트 본문 클릭 → 호스트가 문맥에 따라 처리한다
};

class TrayIcon {
public:
    TrayIcon() = default;
    ~TrayIcon();

    // |owner|가 소유하는 트레이 아이콘을 만든다. |callbackMsg|는 마우스 이벤트 시
    // owner의 WndProc으로 전달되고, |iconId|가 이 아이콘을 식별한다(재설치 후에도
    // 이식성을 유지하려고 NIF_GUID는 의도적으로 쓰지 않는다).
    bool Create(HWND owner, HINSTANCE hinst, UINT callbackMsg, UINT iconId);
    void Destroy();

    // explorer 재시작 등으로 셸이 트레이 등록을 잃은 뒤 다시 등록한다(호스트가
    // TaskbarCreated 메시지를 받아 호출한다). 이미 만들어진 아이콘을 재사용한다.
    void Restore();

    // 풍선 알림. |warning|이면 경고 아이콘(NIIF_WARNING), 아니면 정보 아이콘.
    void ShowBalloon(const wchar_t* title, const wchar_t* text, bool warning = false);

    // 커서 위치에 컨텍스트 메뉴를 만들어 추적한다. 선택된 명령 id(IDM_TRAY_*)를
    // 반환하며 취소되면 0을 반환한다. 실제 동작은 호출자가 수행한다.
    UINT ShowContextMenu(const TrayMenuState& state);

    // 트레이 콜백 메시지를 처리한다(owner의 WndProc이 호출). 요청된 동작을
    // 반환하며, owner가 그에 맞게 처리한다.
    TrayAction OnCallback(WPARAM wParam, LPARAM lParam);

private:
    // 앱의 고정 트레이 아이콘: "IMI"가 들어간 파란 캡슐. 한 번만 만든다.
    HICON RenderAppIcon() const;
    // currentIcon_을 셸에 등록한다(NIM_ADD + v4 선택). Create와 Restore가 공유한다.
    bool AddToShell();

    HWND      owner_ = nullptr;
    HINSTANCE hinst_ = nullptr;
    UINT      callbackMsg_ = 0;
    UINT      iconId_ = 0;
    bool      created_ = false;
    bool      v4_ = false;              // NIM_SETVERSION(v4) 성공 여부
    bool      menuActive_ = false;      // TrackPopupMenu 모달 루프 재진입 가드
    HICON     currentIcon_ = nullptr;   // 소유한다. Destroy에서 파괴
};

} // namespace imi
