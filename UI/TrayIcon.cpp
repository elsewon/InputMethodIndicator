// TrayIcon.cpp — Shell_NotifyIcon(v4) 래퍼, 트레이 컨텍스트 메뉴, 풍선 알림.
//
// 아이콘은 Create에서 32bpp DIB에 한 번 그린 뒤 HICON으로 바꾼다
// (CreateIconIndirect). 덕분에 비트맵 리소스를 함께 배포할 필요가 없다. 여기서
// 아이콘 알파는 **straight**(premultiplied 아님)다 — CreateIconIndirect의 컬러
// 비트맵은 알파 채널을 그대로 쓴다. premultiplied/ULW_ALPHA 규칙은 아이콘이
// 아니라 오버레이에 적용된다.
#include "TrayIcon.h"

#include <windows.h>
#include <shellapi.h>     // Shell_NotifyIconW, NOTIFYICONDATAW, NOTIFYICON_VERSION_4
#include <cstdio>         // _snwprintf_s, _TRUNCATE

#include "Log.h"
#include "Version.h"
#include "resource.h"

// 풍선/토스트 본문 클릭 이벤트. 오래된 SDK 헤더에는 없을 수 있어 방어적으로 정의한다.
#ifndef NIN_BALLOONUSERCLICK
#define NIN_BALLOONUSERCLICK (WM_USER + 5)
#endif

namespace imi {

namespace {

// 트레이 아이콘의 고정된 외양: 오버레이 캡슐과 같은 파랑(#0A84FF) 위에 앱 이니셜을
// 얹어, 트레이 배지와 캐럿 근처 캡슐이 한 앱으로 읽히게 한다.
constexpr const wchar_t* kIconText  = L"IMI";
constexpr int            kIconChars = 3;
const COLORREF           kIconBlue  = RGB(10, 132, 255);

} // namespace

// 소멸자는 .cpp에 둔다. 호출자가 Destroy()를 잊더라도 트레이 아이콘과 소유한
// HICON이 확실히 해제되도록 보장한다.
TrayIcon::~TrayIcon() {
    Destroy();
}

// currentIcon_을 셸에 등록하고 v4 동작을 선택한다. Create(최초)와 Restore
// (explorer 재시작 후)가 공유한다.
bool TrayIcon::AddToShell() {
    NOTIFYICONDATAW nid{};
    nid.cbSize           = sizeof(nid);
    nid.hWnd             = owner_;
    nid.uID              = iconId_;
    nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
    nid.uCallbackMessage = callbackMsg_;
    nid.hIcon            = currentIcon_;
    _snwprintf_s(nid.szTip, _TRUNCATE, IMI_PRODUCT_NAME_W);

    if (!Shell_NotifyIconW(NIM_ADD, &nid)) {
        IMI_ERROR(L"TrayIcon: Shell_NotifyIcon(NIM_ADD) 실패 gle=%lu", GetLastError());
        return false;
    }

    // v4 동작(풍부한 콜백 lParam, NIF_SHOWTIP)을 선택한다. 실패해도 치명적이지
    // 않다 — 그 경우 OnCallback이 대신 고전 인코딩을 해석한다.
    nid.uVersion = NOTIFYICON_VERSION_4;
    v4_ = Shell_NotifyIconW(NIM_SETVERSION, &nid) != FALSE;
    if (!v4_) {
        IMI_WARN(L"TrayIcon: NIM_SETVERSION(v4) 실패 gle=%lu. 고전 콜백을 사용합니다",
                 GetLastError());
    }
    return true;
}

bool TrayIcon::Create(HWND owner, HINSTANCE hinst, UINT callbackMsg, UINT iconId) {
    owner_       = owner;
    hinst_       = hinst;
    callbackMsg_ = callbackMsg;
    iconId_      = iconId;

    // 아이콘은 프로세스 수명 동안 고정이다 — 여기서 한 번만 그린다.
    currentIcon_ = RenderAppIcon();

    if (!AddToShell()) {
        if (currentIcon_ != nullptr) {
            DestroyIcon(currentIcon_);
            currentIcon_ = nullptr;
        }
        return false;
    }

    created_ = true;
    return true;
}

void TrayIcon::Restore() {
    // explorer가 재시작하면 셸이 우리 트레이 등록을 잃는다(created_와 currentIcon_은
    // 그대로 유효하다). 이미 그려 둔 아이콘으로 다시 등록한다.
    if (!created_ || currentIcon_ == nullptr) {
        return;
    }
    if (AddToShell()) {
        IMI_INFO(L"TrayIcon: explorer 재시작 후 트레이 아이콘을 다시 등록했습니다.");
    }
}

void TrayIcon::Destroy() {
    if (created_) {
        NOTIFYICONDATAW nid{};
        nid.cbSize = sizeof(nid);
        nid.hWnd   = owner_;
        nid.uID    = iconId_;
        Shell_NotifyIconW(NIM_DELETE, &nid);
        created_ = false;
    }
    if (currentIcon_ != nullptr) {
        DestroyIcon(currentIcon_);
        currentIcon_ = nullptr;
    }
}

void TrayIcon::ShowBalloon(const wchar_t* title, const wchar_t* text, bool warning) {
    if (!created_) {
        return;
    }
    NOTIFYICONDATAW nid{};
    nid.cbSize      = sizeof(nid);
    nid.hWnd        = owner_;
    nid.uID         = iconId_;
    nid.uFlags      = NIF_INFO;
    nid.dwInfoFlags = (warning ? NIIF_WARNING : NIIF_INFO) | NIIF_RESPECT_QUIET_TIME;
    _snwprintf_s(nid.szInfoTitle, _TRUNCATE, L"%s", (title != nullptr) ? title : L"");
    _snwprintf_s(nid.szInfo,      _TRUNCATE, L"%s", (text  != nullptr) ? text  : L"");
    if (!Shell_NotifyIconW(NIM_MODIFY, &nid)) {
        IMI_WARN(L"TrayIcon: NIM_MODIFY(풍선 알림) 실패 gle=%lu", GetLastError());
    }
}

UINT TrayIcon::ShowContextMenu(const TrayMenuState& state) {
    // TrackPopupMenu는 이 스레드에서 모달 메시지 루프를 돌리므로, 메뉴가 떠
    // 있는 동안 트레이 아이콘을 다시 클릭하면 그 콜백이 모달 루프 안에서
    // 디스패치되어 이 함수가 재진입한다. 중첩된 TrackPopupMenu는 즉시 실패하고
    // (ERROR_POPUP_ALREADY_ACTIVE) 사용자에게는 "메뉴가 아예 안 뜨는" 것으로
    // 보이므로, 이미 떠 있는 메뉴가 그 클릭을 받도록 두고 조용히 무시한다.
    if (menuActive_) {
        return 0;
    }

    HMENU menu = CreatePopupMenu();
    if (menu == nullptr) {
        IMI_ERROR(L"TrayIcon: CreatePopupMenu 실패 gle=%lu", GetLastError());
        return 0;
    }
    AppendMenuW(menu, MF_STRING, IDM_TRAY_ABOUT, L"정보");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (state.notifyLanguage ? MF_CHECKED : MF_UNCHECKED),
                IDM_TRAY_TOGGLE_LANG, L"한/영 전환 표시");
    AppendMenuW(menu, MF_STRING | (state.notifyCapsLock ? MF_CHECKED : MF_UNCHECKED),
                IDM_TRAY_TOGGLE_CAPS, L"Caps Lock 표시");
    AppendMenuW(menu, MF_STRING | (state.notifyFocusChange ? MF_CHECKED : MF_UNCHECKED),
                IDM_TRAY_TOGGLE_FOCUS, L"입력 포커스 변경 시 표시");
    AppendMenuW(menu, MF_STRING | (state.startWithWindows ? MF_CHECKED : MF_UNCHECKED),
                IDM_TRAY_START_WITH_WIN, L"Windows 시작 시 자동 실행");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_TRAY_SHOW_DIAGNOSTICS, L"진단 보기\x2026");
    AppendMenuW(menu, MF_STRING, IDM_TRAY_OPEN_LOGFILE,  L"로그 파일 열기");
    AppendMenuW(menu, MF_STRING, IDM_TRAY_REPORT_ISSUE,  L"이상 제보\x2026");
    // 이상 상태(표시기를 오래 읽지 못함)일 때만 진단 도구 아래에 조치 항목을 노출한다.
    if (state.explorerRestartHint) {
        AppendMenuW(menu, MF_STRING, IDM_TRAY_RESTART_EXPLORER, L"⚠️ Windows 탐색기 재시작 필요");
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_TRAY_EXIT, L"종료");

    POINT pt{};
    GetCursorPos(&pt);

    // 고전적인 트레이 메뉴 처방: 바깥을 클릭했을 때 메뉴가 닫히도록 owner가
    // 포어그라운드여야 하고, 뒤이은 post가 메뉴의 내부 상태를 비워 준다.
    menuActive_ = true;
    SetForegroundWindow(owner_);
    UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
                              pt.x, pt.y, 0, owner_, nullptr);
    PostMessageW(owner_, WM_NULL, 0, 0);
    menuActive_ = false;

    DestroyMenu(menu);
    return cmd;  // 0 == 취소됨. 그 외에는 IDM_TRAY_* 명령.
}

TrayAction TrayIcon::OnCallback(WPARAM wParam, LPARAM lParam) {
    // v4 인코딩: LOWORD(lParam) = 이벤트, HIWORD(lParam) = 아이콘 id.
    // 고전 인코딩(NIM_SETVERSION 실패 시): lParam = 이벤트, wParam = 아이콘 id.
    UINT event, id;
    if (v4_) {
        event = LOWORD(lParam);
        id    = HIWORD(lParam);
    } else {
        event = static_cast<UINT>(lParam);
        id    = static_cast<UINT>(wParam);
    }
    if (id != iconId_) {
        return TrayAction::None;  // 우리 아이콘이 아니다(방어적. owner는 우리 것만 보낸다)
    }

    switch (event) {
        case WM_CONTEXTMENU:        // v4 우클릭 / Shift+F10
        case WM_RBUTTONUP:          // 고전 우클릭 뗌
        case NIN_KEYSELECT:         // 키보드(Enter/Space) 선택
            return TrayAction::ContextMenu;
        case NIN_BALLOONUSERCLICK:  // 풍선/토스트 본문 클릭
            return TrayAction::NotificationClick;
        default:
            return TrayAction::None;
    }
}

HICON TrayIcon::RenderAppIcon() const {
    const int w = GetSystemMetrics(SM_CXSMICON);
    const int h = GetSystemMetrics(SM_CYSMICON);
    if (w <= 0 || h <= 0) {
        return nullptr;
    }

    HDC screen = GetDC(nullptr);
    if (screen == nullptr) {
        return nullptr;
    }
    HDC dc = CreateCompatibleDC(screen);
    ReleaseDC(nullptr, screen);
    if (dc == nullptr) {
        return nullptr;
    }

    // 위에서 아래로 배치된 32bpp DIB라, 픽셀 (x,y)는 bits[(y*w + x)*4]에
    // B,G,R,A 순으로 놓인다.
    BITMAPINFO bi{};
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = w;
    bi.bmiHeader.biHeight      = -h;  // 음수 == 위에서 아래
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP color = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (color == nullptr || bits == nullptr) {
        if (color != nullptr) {
            DeleteObject(color);
        }
        DeleteDC(dc);
        return nullptr;
    }

    HGDIOBJ oldBmp = SelectObject(dc, color);

    // 캡슐 띠: 전체 폭에 걸친 가운데 정렬된 가로 줄무늬. GDI는 알파 채널을 절대
    // 쓰지 않으므로, 비트맵 전체를 파랗게 칠한 뒤 아래에서 알파로 캡슐을 깎아
    // 낸다 — 그 바깥 픽셀은 결국 완전히 투명해진다.
    const float capH = static_cast<float>(h) * 0.78f;
    const float rad  = capH * 0.5f;
    const float cy   = static_cast<float>(h) * 0.5f;

    RECT full{0, 0, w, h};
    HBRUSH bgBrush = CreateSolidBrush(kIconBlue);
    if (bgBrush != nullptr) {
        FillRect(dc, &full, bgBrush);
        DeleteObject(bgBrush);
    }

    // "IMI"를 캡슐의 둥근 양 끝 사이에 들어갈 때까지 줄인다.
    const int innerW = w - 2;
    int fontH = static_cast<int>(capH * 0.80f);
    if (fontH < 4) {
        fontH = 4;
    }
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    for (int attempt = 0; attempt < 12 && fontH >= 4; ++attempt) {
        LOGFONTW lf{};
        lf.lfHeight  = -fontH;
        lf.lfWeight  = FW_BOLD;
        lf.lfCharSet = DEFAULT_CHARSET;
        lf.lfQuality = ANTIALIASED_QUALITY;  // CLEARTYPE은 알파에서 색 번짐이 생긴다
        wcscpy_s(lf.lfFaceName, L"Segoe UI");

        HFONT font = CreateFontIndirectW(&lf);
        if (font == nullptr) {
            break;
        }
        HGDIOBJ oldFont = SelectObject(dc, font);
        SIZE ext{};
        const bool measured =
            GetTextExtentPoint32W(dc, kIconText, kIconChars, &ext) != FALSE;
        if (!measured || ext.cx <= innerW) {
            RECT band{0, static_cast<LONG>(cy - rad), w, static_cast<LONG>(cy + rad)};
            DrawTextW(dc, kIconText, kIconChars, &band,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_NOCLIP);
            SelectObject(dc, oldFont);
            DeleteObject(font);
            break;
        }
        SelectObject(dc, oldFont);
        DeleteObject(font);
        --fontH;
    }

    // 바이트를 읽고 고치기 전에, 배치된 GDI 드로잉이 DIB에 반영되도록 보장한다.
    GdiFlush();

    // 알파 = 캡슐(중심 선분에서 |rad| 이내인 점들의 집합)의 커버리지. 16 px에서도
    // 가장자리가 매끄럽도록 4x4 슈퍼샘플링한다.
    const float cx0 = rad;
    const float cx1 = static_cast<float>(w) - rad;
    uint8_t* px = static_cast<uint8_t*>(bits);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            int cover = 0;
            for (int sy = 0; sy < 4; ++sy) {
                for (int sx = 0; sx < 4; ++sx) {
                    const float fx = x + (sx + 0.5f) * 0.25f;
                    const float fy = y + (sy + 0.5f) * 0.25f;
                    const float qx = (fx < cx0) ? cx0 : ((fx > cx1) ? cx1 : fx);
                    const float dx = fx - qx;
                    const float dy = fy - cy;
                    if (dx * dx + dy * dy <= rad * rad) {
                        ++cover;
                    }
                }
            }
            px[(y * w + x) * 4 + 3] = static_cast<uint8_t>(cover * 255 / 16);
        }
    }

    // CreateIconIndirect는 32bpp 알파 아이콘이라도 마스크 비트맵을 요구한다. 전부
    // 0인 AND 마스크는 "컬러 픽셀을 취하라"는 뜻이라 알파가 블렌딩을 주도한다.
    HBITMAP mask = CreateBitmap(w, h, 1, 1, nullptr);
    if (mask != nullptr) {
        HDC mdc = CreateCompatibleDC(nullptr);
        if (mdc != nullptr) {
            HGDIOBJ oldMask = SelectObject(mdc, mask);
            PatBlt(mdc, 0, 0, w, h, BLACKNESS);  // 1bpp -> 전부 0(불투명)
            SelectObject(mdc, oldMask);
            DeleteDC(mdc);
        }
    }

    // CreateIconIndirect에 넘기기 전에 컬러 DIB를 선택 해제한다 — GDI는 아직 DC에
    // 선택된 비트맵을 읽어서는 안 된다.
    SelectObject(dc, oldBmp);

    HICON icon = nullptr;
    if (mask != nullptr) {
        ICONINFO ii{};
        ii.fIcon    = TRUE;
        ii.hbmMask  = mask;
        ii.hbmColor = color;
        icon = CreateIconIndirect(&ii);  // 두 비트맵 모두 복사한다
        if (icon == nullptr) {
            IMI_WARN(L"TrayIcon: CreateIconIndirect 실패 gle=%lu", GetLastError());
        }
    }

    // CreateIconIndirect가 복사본을 가져갔으므로 우리 GDI 객체는 해제한다.
    if (mask != nullptr) {
        DeleteObject(mask);
    }
    DeleteObject(color);
    DeleteDC(dc);

    return icon;
}

} // namespace imi
