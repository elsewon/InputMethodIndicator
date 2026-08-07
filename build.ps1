<#
.SYNOPSIS
  InputMethodIndicator.exe 빌드(Visual Studio / MSBuild).

.DESCRIPTION
  단일 자체 완결형 GUI 앱(입력 상태는 UI Automation으로 OS 입력 표시기에서
  읽는다). 기본적으로 x64를 빌드하고, ARM64 툴셋이 설치되어 있으면 ARM64도
  빌드한다. MSBuild는 vswhere로 찾으므로 개발자 명령 프롬프트가 필요 없다.
  똑같이 Visual Studio에서 InputMethodIndicator.sln을 열고 F5를 눌러도 된다.

.PARAMETER Config
  Debug 또는 Release(기본값 Release).

.PARAMETER SkipArm64
  ARM64 툴셋이 설치되어 있어도 ARM64를 빌드하지 않는다.

.EXAMPLE
  .\build.ps1
  .\build.ps1 -Config Debug -SkipArm64
#>
[CmdletBinding()]
param(
    [ValidateSet('Debug','Release')] [string] $Config = 'Release',
    [switch] $SkipArm64
)

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$sln  = Join-Path $root 'InputMethodIndicator.sln'
$dist = Join-Path $root 'dist'

function Get-MSBuild {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere)) {
        throw "vswhere.exe를 찾을 수 없습니다. Visual Studio 2022 이상을 'C++를 사용한 데스크톱 개발' 워크로드와 함께 설치하세요."
    }
    $vsPath = & $vswhere -latest -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property installationPath
    if (-not $vsPath) { throw "C++ 툴셋을 갖춘 Visual Studio를 찾을 수 없습니다." }
    $msbuild = Join-Path $vsPath 'MSBuild\Current\Bin\MSBuild.exe'
    if (-not (Test-Path $msbuild)) { throw "$vsPath 아래에서 MSBuild.exe를 찾을 수 없습니다." }
    return $msbuild
}

function Test-Arm64Toolset {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    $p = & $vswhere -latest -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.ARM64 `
        -property installationPath
    return [bool]$p
}

function Build-Platform {
    param([Parameter(Mandatory)] [string] $Platform)
    Write-Host "==> MSBuild $Platform | $Config" -ForegroundColor Cyan
    & $script:msbuild $sln /p:Configuration=$Config /p:Platform=$Platform /m /nologo /verbosity:minimal
    if ($LASTEXITCODE -ne 0) { throw "$Platform|$Config 빌드에 실패했습니다." }
}

$script:msbuild = Get-MSBuild
Write-Host "MSBuild: $script:msbuild" -ForegroundColor DarkGray

if (Test-Path $dist) { Remove-Item $dist -Recurse -Force }
New-Item -ItemType Directory -Path $dist | Out-Null

Build-Platform -Platform 'x64'
Copy-Item (Join-Path $root "out\x64\$Config\InputMethodIndicator.exe") -Destination $dist -Force

if ($SkipArm64) {
    Write-Warning "요청에 따라 ARM64를 건너뜁니다."
} elseif (-not (Test-Arm64Toolset)) {
    Write-Warning "ARM64 툴셋이 설치되어 있지 않아 건너뜁니다(x64 빌드는 완료됨)."
} else {
    Build-Platform -Platform 'ARM64'
    $armDir = Join-Path $dist 'arm64'
    New-Item -ItemType Directory -Path $armDir -Force | Out-Null
    Copy-Item (Join-Path $root "out\ARM64\$Config\InputMethodIndicator.exe") -Destination $armDir -Force
}

Write-Host ""
Write-Host "완료. InputMethodIndicator.exe(단독 실행형)가 $dist 에 있습니다" -ForegroundColor Green
Write-Host "실행: .\dist\InputMethodIndicator.exe" -ForegroundColor Green
