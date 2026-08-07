// Config.cpp — HKCU\Software\InputMethodIndicator 아래에 저장되는 사용자 설정.
//
// 레지스트리만 사용하며(advapi32 Reg*), 로그 경로에는 SHGetKnownFolderPath
// (shell32/ole32)를 쓴다. 호스트 타깃이 advapi32/shell32/ole32/shcore를 링크하므로
// 여기서 쓰는 API는 모두 사용 가능하다. Load는 절대 실패하지 않는다 — 없는 값은
// 구조체 헤더의 기본값으로 대체된다. Save는 모든 필드를 쓰므로 저장소가 완전해진다.
#include "Config.h"

#include <windows.h>
#include <shlobj.h>       // SHGetKnownFolderPath, FOLDERID_LocalAppData
#include <cstdio>         // _snwprintf_s, _TRUNCATE
#include <cwchar>         // wcslen

namespace imi {

namespace {

// 레지스트리 위치. Run 항목은 앱과 같은 값 이름을 사용한다.
constexpr const wchar_t* kRegKeyPath   = L"Software\\InputMethodIndicator";
constexpr const wchar_t* kRunKeyPath   = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr const wchar_t* kAppValueName = L"InputMethodIndicator";
constexpr const wchar_t* kAppDirName   = L"InputMethodIndicator";
constexpr const wchar_t* kLogFileName  = L"imi.log";

// 값 이름(고정해서 유지한다 — 이름을 바꾸면 사용자가 저장해 둔 설정이 버려진다).
constexpr const wchar_t* kV_NotifyLanguage    = L"NotifyLanguage";
constexpr const wchar_t* kV_NotifyCapsLock    = L"NotifyCapsLock";
constexpr const wchar_t* kV_NotifyFocusChange = L"NotifyFocusChange";
constexpr const wchar_t* kV_StartWithWindows  = L"StartWithWindows";
constexpr const wchar_t* kV_OverlayHoldMs    = L"OverlayHoldMs";
constexpr const wchar_t* kV_OverlayFadeInMs  = L"OverlayFadeInMs";
constexpr const wchar_t* kV_OverlayFadeOutMs = L"OverlayFadeOutMs";
constexpr const wchar_t* kV_OverlaySize96    = L"OverlaySize96";
constexpr const wchar_t* kV_CaretGap96       = L"CaretGap96";
constexpr const wchar_t* kV_OverlayOpacity   = L"OverlayOpacity";
constexpr const wchar_t* kV_LogLevel         = L"LogLevel";
constexpr const wchar_t* kV_LogToFile        = L"LogToFile";

// REG_DWORD를 읽는다. 값이 없거나 타입/크기가 맞지 않으면 |value|를 건드리지
// 않는다(기본값을 유지한다).
void ReadDword(HKEY key, const wchar_t* name, DWORD& value) {
    DWORD type = 0;
    DWORD data = 0;
    DWORD cb = sizeof(data);
    LONG rc = RegQueryValueExW(key, name, nullptr, &type,
                               reinterpret_cast<BYTE*>(&data), &cb);
    if (rc == ERROR_SUCCESS && type == REG_DWORD && cb == sizeof(data)) {
        value = data;
    }
}

void ReadBool(HKEY key, const wchar_t* name, bool& value) {
    DWORD tmp = value ? 1u : 0u;
    ReadDword(key, name, tmp);
    value = (tmp != 0);
}

void ReadInt(HKEY key, const wchar_t* name, int& value) {
    DWORD tmp = static_cast<DWORD>(value);
    ReadDword(key, name, tmp);
    value = static_cast<int>(tmp);
}

bool WriteDword(HKEY key, const wchar_t* name, DWORD value) {
    LONG rc = RegSetValueExW(key, name, 0, REG_DWORD,
                             reinterpret_cast<const BYTE*>(&value), sizeof(value));
    if (rc != ERROR_SUCCESS) {
        IMI_WARN(L"Config: RegSetValueExW(%s) 실패 rc=%ld", name, rc);
        return false;
    }
    return true;
}

// Run 항목에 쓸, 따옴표로 감싼 실행 파일의 전체 경로. 실패하면(경로가 너무 길거나
// API 오류) false를 반환하며 |out|의 내용은 정의되지 않는다.
bool GetQuotedExePath(wchar_t* out, size_t count) {
    if (out == nullptr || count < 3) {
        return false;
    }
    wchar_t path[MAX_PATH];
    DWORD len = GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
    if (len == 0 || len >= ARRAYSIZE(path)) {
        // 0 = 실패. 크기와 같으면 잘린 것이다(버퍼가 너무 작다).
        IMI_WARN(L"Config: GetModuleFileNameW 실패 gle=%lu", GetLastError());
        return false;
    }
    // "<경로>" — len + 따옴표 2개 + NUL 만큼 필요하다.
    if (static_cast<size_t>(len) + 3 > count) {
        return false;
    }
    out[0] = L'"';
    // 경로 본문을 복사한다(|len|에는 끝의 NUL이 포함되지 않는다).
    for (DWORD i = 0; i < len; ++i) {
        out[1 + i] = path[i];
    }
    out[1 + len] = L'"';
    out[2 + len] = L'\0';
    return true;
}

} // namespace

Config Config::Load() {
    Config cfg;  // 헤더에 정의된 기본값에서 시작한다

    HKEY key = nullptr;
    LONG rc = RegOpenKeyExW(HKEY_CURRENT_USER, kRegKeyPath, 0, KEY_QUERY_VALUE, &key);
    if (rc != ERROR_SUCCESS) {
        // 첫 실행(키 없음)은 정상이다 — 불평 없이 기본값을 반환한다.
        return cfg;
    }

    ReadBool(key, kV_NotifyLanguage,    cfg.notifyLanguage);
    ReadBool(key, kV_NotifyCapsLock,    cfg.notifyCapsLock);
    ReadBool(key, kV_NotifyFocusChange, cfg.notifyFocusChange);
    ReadBool(key, kV_StartWithWindows,  cfg.startWithWindows);

    ReadInt(key, kV_OverlayHoldMs,    cfg.overlayHoldMs);
    ReadInt(key, kV_OverlayFadeInMs,  cfg.overlayFadeInMs);
    ReadInt(key, kV_OverlayFadeOutMs, cfg.overlayFadeOutMs);
    ReadInt(key, kV_OverlaySize96,    cfg.overlaySize96);
    ReadInt(key, kV_CaretGap96,       cfg.caretGap96);

    DWORD opacity = cfg.overlayOpacity;
    ReadDword(key, kV_OverlayOpacity, opacity);
    cfg.overlayOpacity = static_cast<uint8_t>(opacity & 0xFFu);

    DWORD level = static_cast<DWORD>(cfg.logLevel);
    ReadDword(key, kV_LogLevel, level);
    if (level <= static_cast<DWORD>(LogLevel::Error)) {
        cfg.logLevel = static_cast<LogLevel>(static_cast<int32_t>(level));
    }

    ReadBool(key, kV_LogToFile, cfg.logToFile);

    RegCloseKey(key);
    cfg.Sanitize();
    return cfg;
}

namespace {
int ClampInt(int v, int lo, int hi) {
    return (v < lo) ? lo : ((v > hi) ? hi : v);
}
} // namespace

void Config::Sanitize() {
    overlayHoldMs    = ClampInt(overlayHoldMs,    100, 10000);
    overlayFadeInMs  = ClampInt(overlayFadeInMs,    0,  2000);
    overlayFadeOutMs = ClampInt(overlayFadeOutMs,   0,  2000);
    overlaySize96    = ClampInt(overlaySize96,     16,   200);
    caretGap96       = ClampInt(caretGap96,         0,   100);
    if (overlayOpacity < 30) {
        overlayOpacity = 30;   // 완전히 안 보이는 상태는 허용하지 않는다
    }
}

bool Config::Save() const {
    HKEY key = nullptr;
    DWORD disposition = 0;
    LONG rc = RegCreateKeyExW(HKEY_CURRENT_USER, kRegKeyPath, 0, nullptr,
                              REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr,
                              &key, &disposition);
    if (rc != ERROR_SUCCESS) {
        IMI_ERROR(L"Config::Save RegCreateKeyExW 실패 rc=%ld", rc);
        return false;
    }

    bool ok = true;
    ok &= WriteDword(key, kV_NotifyLanguage,    notifyLanguage ? 1u : 0u);
    ok &= WriteDword(key, kV_NotifyCapsLock,    notifyCapsLock ? 1u : 0u);
    ok &= WriteDword(key, kV_NotifyFocusChange, notifyFocusChange ? 1u : 0u);
    ok &= WriteDword(key, kV_StartWithWindows,  startWithWindows ? 1u : 0u);
    ok &= WriteDword(key, kV_OverlayHoldMs,    static_cast<DWORD>(overlayHoldMs));
    ok &= WriteDword(key, kV_OverlayFadeInMs,  static_cast<DWORD>(overlayFadeInMs));
    ok &= WriteDword(key, kV_OverlayFadeOutMs, static_cast<DWORD>(overlayFadeOutMs));
    ok &= WriteDword(key, kV_OverlaySize96,    static_cast<DWORD>(overlaySize96));
    ok &= WriteDword(key, kV_CaretGap96,       static_cast<DWORD>(caretGap96));
    ok &= WriteDword(key, kV_OverlayOpacity,   static_cast<DWORD>(overlayOpacity));
    ok &= WriteDword(key, kV_LogLevel,         static_cast<DWORD>(logLevel));
    ok &= WriteDword(key, kV_LogToFile,        logToFile ? 1u : 0u);

    RegCloseKey(key);
    return ok;
}

bool Config::ApplyStartWithWindows() const {
    HKEY key = nullptr;
    LONG rc = RegOpenKeyExW(HKEY_CURRENT_USER, kRunKeyPath, 0, KEY_SET_VALUE, &key);
    if (rc != ERROR_SUCCESS) {
        IMI_ERROR(L"Config: Run 키 열기 실패 rc=%ld", rc);
        return false;
    }

    bool ok = true;
    if (startWithWindows) {
        wchar_t quoted[MAX_PATH + 2];
        if (!GetQuotedExePath(quoted, ARRAYSIZE(quoted))) {
            RegCloseKey(key);
            return false;
        }
        // REG_SZ의 바이트 길이에는 끝을 알리는 NUL이 포함되어야 한다.
        DWORD cb = static_cast<DWORD>((wcslen(quoted) + 1) * sizeof(wchar_t));
        rc = RegSetValueExW(key, kAppValueName, 0, REG_SZ,
                            reinterpret_cast<const BYTE*>(quoted), cb);
        if (rc != ERROR_SUCCESS) {
            IMI_ERROR(L"Config: Run 값 쓰기 실패 rc=%ld", rc);
            ok = false;
        }
    } else {
        rc = RegDeleteValueW(key, kAppValueName);
        // 값이 없는 것은 사용자 관점에서 성공이다.
        if (rc != ERROR_SUCCESS && rc != ERROR_FILE_NOT_FOUND) {
            IMI_ERROR(L"Config: Run 값 삭제 실패 rc=%ld", rc);
            ok = false;
        }
    }

    RegCloseKey(key);
    return ok;
}

bool Config::GetLogFilePath(wchar_t* out, size_t count) {
    if (out == nullptr || count == 0) {
        return false;
    }

    PWSTR base = nullptr;
    HRESULT hr = SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &base);
    if (FAILED(hr) || base == nullptr) {
        IMI_ERROR(L"Config: SHGetKnownFolderPath(LocalAppData) 실패 hr=0x%08lX",
                  static_cast<unsigned long>(hr));
        if (base != nullptr) {
            CoTaskMemFree(base);
        }
        return false;
    }

    // "<LocalAppData>\InputMethodIndicator" 경로를 만들고 디렉터리가 있는지 확인한다.
    wchar_t dir[MAX_PATH];
    int dirLen = _snwprintf_s(dir, ARRAYSIZE(dir), _TRUNCATE, L"%s\\%s",
                              base, kAppDirName);
    CoTaskMemFree(base);
    if (dirLen < 0) {
        return false;  // 잘렸다 — 경로가 너무 길어 사용할 수 없다
    }

    // 이미 존재하면 CreateDirectoryW 실패는 문제없다. 그 밖의 오류는 치명적이다.
    if (!CreateDirectoryW(dir, nullptr)) {
        DWORD gle = GetLastError();
        if (gle != ERROR_ALREADY_EXISTS) {
            IMI_ERROR(L"Config: CreateDirectoryW(%s) 실패 gle=%lu", dir, gle);
            return false;
        }
    }

    int len = _snwprintf_s(out, count, _TRUNCATE, L"%s\\%s", dir, kLogFileName);
    if (len < 0) {
        return false;  // 호출자의 버퍼가 너무 작다
    }
    return true;
}

} // namespace imi
