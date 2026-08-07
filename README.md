# InputMethodIndicator

**Windows 11용 입력 상태 표시기**다 — macOS에서 입력 캐럿 아래에 표시되는 입력
상태 표시기의 복제품이다. 작업 표시줄 트레이의 입력 상태가 바뀌면(한국어 ↔
영어(한/영)나 Caps Lock) 작은 파란 캡슐을 캐럿 바로 아래에 띄운다. 트레이 아이콘은
설정 메뉴와 내장 진단 도구를 담고 있다.

<p align="center">
  <img src="docs/pill.svg" alt="캐럿 아래로 튀어나오는 파란 입력 표시 캡슐 — 한국어(가)/영어(A)/Caps Lock(⇪)" width="620">
</p>

> 네이티브 C++ (Win32 + Direct2D + UI Automation). **단일 자립형
> `InputMethodIndicator.exe`** — 설치 관리자도, 의존성도 없다.

---

## 동작 원리

Windows는 이미 참 입력 상태를 알고 있고, 그것을 **트레이 입력 표시기**(가/A)로
보여 준다. 그 표시기는 UI Automation 요소이며, 접근성 Name이 한/영을 토글할
때마다 "…한국어 입력 모드"와 "…영어 입력 모드"(현지화된 문자열) 사이를 오간다.
그리고 바뀔 때 property-changed 이벤트를 일으킨다. 앱은 그저 **그 요소를 지켜
보다가**, 변경이 있을 때마다 캐럿을 해석하고 캡슐을 보여 줄 뿐이다.

OS 자신의 표면을 읽기 때문에 UWP/스토어 앱과 권한 상승된 창을 포함한 **모든**
앱에서 동작하며, 백신 프로그램과의 마찰도 없다.

| 구성 요소 | 역할 |
|---|---|
| `TrayIndicatorWatcher` | UIA로 입력 표시기를 찾고 Name 변경을 구독한다 → 현재 언어 |
| `CaretResolver` | 캡슐을 놓을 캐럿 위치를 찾는다(GetGUIThreadInfo → MSAA → IA2 → UIA). 캐럿이 없으면 ⇒ 캡슐도 없다 |
| `OverlayWindow` | 페이드인/유지/페이드아웃을 하는 레이어드 Direct2D 캡슐 |
| `CapsLockMonitor` | Caps Lock 표시기를 위한 `WH_KEYBOARD_LL`(트레이에는 Caps 상태가 없다) |
| `TrayIcon` | 고정된 "IMI" 트레이 아이콘 + 컨텍스트 메뉴(상태는 여기가 아니라 캡슐로 보여 준다) |
| `InputStateMachine` | 디바운스 + 앱 전환 동기화를 억제해 실제 토글만 보여 준다 |

전체 데이터 흐름은 [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)를 참고한다.

---

## 빌드

**전제 조건**: *C++를 사용한 데스크톱 개발* 워크로드가 설치된 Visual Studio
2022 이상. (네이티브 ARM64 빌드를 원하면 선택적으로 *MSVC ARM64 빌드 도구*도.)
CMake도, 다른 의존성도 필요 없습니다.

**Visual Studio에서**: **`InputMethodIndicator.sln`**을 열고 `x64`를 고른 뒤 F5를
누릅니다.

**명령줄에서**(vswhere로 MSBuild를 찾습니다):

```powershell
.\build.ps1                # Release x64 (해당 도구 집합이 설치되어 있으면 ARM64 포함) -> .\dist
.\build.ps1 -Config Debug
.\build.ps1 -SkipArm64
```

결과물은 **자립형** `dist\InputMethodIndicator.exe`입니다 — 아무 데나 복사해서
실행하면 됩니다.

**자체 테스트**: `InputMethodIndicator.exe --selftest`는 인바이너리 로직
테스트(트레이 Name 파서 + 상태 머신 규칙)를 실행하고 실패 개수(0 = 통과)로
종료합니다 — 창이나 트레이 아이콘은 만들지 않습니다.

---

## 실행

`InputMethodIndicator.exe`를 실행한다. 창 없이 트레이에 자리 잡는다. 아무 앱에서나
타이핑하며 한/영이나 Caps Lock을 토글하면 — 캡슐이 캐럿 아래로 튀어나온다.

- **한/영**: 캡슐(가 / A)이 튀어나와 잠깐 유지되었다가 페이드아웃한다.
- **Caps Lock**: Caps가 켜져 있는 동안 ⇪ 캡슐이 **머무른다** — 타이핑하는 동안에는
  숨었다가 약 1초간 입력이 없으면 다시 튀어나오고, Caps가 꺼지면 사라진다.

**트레이 메뉴**(트레이 아이콘을 우클릭. UI는 한국어다):

- **정보** — 정보 창.
- **한/영 전환 표시 / Caps Lock 표시** — 종류별 켜기/끄기.
- **입력 포커스 변경 시 표시** — 기본값은 꺼짐. 켜면 **텍스트 필드**가 포커스를
  얻는 즉시 현재 모드가 튀어나와, 타이핑 전에 자신이 가인지 A인지 알 수 있다.
  버튼·목록·타일에 포커스가 내려앉으면 아무것도 보여 주지 않는다.
- **Windows 시작 시 자동 실행** — HKCU Run 항목을 추가/제거한다.
- **진단 보기…** — 실시간 상태 + 최근 로그.
- **로그 파일 열기** — 디스크에 저장된 로그(파일 로깅이 켜져 있을 때. 약 1 MB에서
  회전된다).
- **종료**.

각 토글의 상태는 메뉴에 체크 표시로 보인다. 설정은
`HKCU\Software\InputMethodIndicator` 아래에 유지된다.

**단일 인스턴스 규칙**: **Release**에서는 실행 중인 인스턴스가 이긴다 — 두 번째
실행은 그 위에 풍선 알림을 띄우고 종료한다. **Debug**에서는 새 빌드가 이긴다 —
실행 중인 인스턴스를 (정상적으로, 멈춰 있을 때만 강제 종료하여) 닫고 자리를
넘겨받는다. 그래서 갓 만든 디버그 빌드를 실행하면 트레이에 있던 것을 항상
대체한다.

---

## 프로젝트 구조

단일 프로젝트 솔루션: 프로젝트가 저장소 루트에 있으므로, 불필요하게 중첩된
폴더가 없다.

```
InputMethodIndicator.sln
InputMethodIndicator.vcxproj (+ .filters)
app.manifest  app.rc  resource.h
main.cpp  App.*  Config.*  Diagnostics.*  SelfTest.*
Common/   Log.*  DpiUtil.*  ComRef.h  Constants.h  Version.h
Model/    InputState.h  InputStateMachine.*
Input/    TrayIndicatorWatcher.*  CapsLockMonitor.*
UI/       TrayIcon.*  OverlayWindow.*  CaretResolver.*
docs/     build.ps1
out/      (빌드 중간 산출물 + 바이너리)   dist/  (패키지된 exe)
```

루트가 유일한 include 루트이고, 그 소스 하위 폴더들이 include 경로에 있으므로,
헤더는 파일명만으로 포함된다(`#include "InputState.h"`).

---

## 참고 및 제한 사항

- 입력 표시기는 **2개 이상의 입력 방법**(예: 한국어 Microsoft IME)이 있고
  표시기가 설정에서 숨겨져 있지 않을 때만 존재한다 — 그렇지 않으면 보여 줄 한/영
  상태 자체가 없다. explorer 재시작이나 입력 방법 변경 이후에는 워처가 표시기를
  자동으로 다시 찾는다.
- Name은 현지화되어 있다. 파서는 한국어·영어 Windows UI 양쪽에서 한국어/일본어/
  중국어와 영어 토큰을 인식한다(더 추가하려면 [InputState.h](Model/InputState.h)를
  확장한다).
- 접근성 캐럿이 없는 앱(직접 그리는 에디터, 일부 게임)에서는 **캡슐이 나오지 않는다**:
  표시기는 정확히 놓을 수 있는 자리에만 그려지기 때문이다.
  [docs/KNOWN-LIMITS.md](docs/KNOWN-LIMITS.md)를 참고한다.

## 라이선스

MIT — [LICENSE](LICENSE) 참고.
