# ================================================
# Build-UserMode.ps1  (package ProcessMonitor)
#
# Builds:
#   1) procmon_app.exe          (production user-mode app)
#   2) procmon_unified_tests.exe     (mock / ring / protocol host tests)
#
# Optional: -RunTests runs the unified test binary after a successful build.
# Driver / install scripts are separate (Build-Driver / install_procmon).
# ================================================

[CmdletBinding()]
param(
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Debug",

    [ValidateSet("x64", "x86")]
    [string]$Platform = "x64",

    [switch]$RunTests,

    [switch]$SkipApp,

    [switch]$SkipTests
)

$ErrorActionPreference = "Stop"
$ScriptRoot  = $PSScriptRoot
$ProjectRoot = Split-Path $ScriptRoot -Parent
$UserModeDir = Join-Path $ProjectRoot "usermode"
$MockDir     = Join-Path $ProjectRoot "mock_tests"
$SharedDir   = Join-Path $ProjectRoot "shared"

$AppSource   = Join-Path $UserModeDir "procmon_app.cpp"
$TestSource  = Join-Path $MockDir "main_test_runner.cpp"

$OutDir      = Join-Path (Join-Path $UserModeDir $Platform) $Configuration
$OutExe      = Join-Path $OutDir "procmon_app.exe"
$TestOutDir  = Join-Path (Join-Path $MockDir $Platform) $Configuration
$TestExe     = Join-Path $TestOutDir "procmon_unified_tests.exe"

Write-Host "================================================" -ForegroundColor Cyan
Write-Host "  ProcessMonitor - User-Mode Builder" -ForegroundColor Cyan
Write-Host "================================================" -ForegroundColor Cyan
Write-Host "Configuration : $Configuration"
Write-Host "Platform      : $Platform"
Write-Host "App           : $(if ($SkipApp) { 'SKIP' } else { 'procmon_app.cpp' })"
Write-Host "Mock tests    : $(if ($SkipTests) { 'SKIP' } else { 'main_test_runner.cpp' })"
Write-Host ""

# --- Locate Visual Studio ---
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) {
    Write-Error "vswhere.exe not found."
    exit 1
}

$vsPath = & $vswhere -latest -products * `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
    -property installationPath
if (-not $vsPath) {
    Write-Error "No Visual Studio installation with C++ tools found."
    exit 1
}

Write-Host "[OK] Visual Studio: $vsPath" -ForegroundColor Green
$devCmd = Join-Path $vsPath "Common7\Tools\VsDevCmd.bat"
if (-not (Test-Path $devCmd)) {
    Write-Error "VsDevCmd.bat not found."
    exit 1
}

$cflags = "/nologo /std:c++latest /EHsc /W4 /D_CRT_SECURE_NO_WARNINGS"
if ($Configuration -eq "Debug") {
    $cflags += " /Od /Zi /MDd /D_DEBUG"
} else {
    $cflags += " /O2 /MD /DNDEBUG"
}

$inc = "/I`"$UserModeDir`" /I`"$SharedDir`""

function Invoke-ClBuild {
    param(
        [string]$WorkDir,
        [string]$ClArgs,
        [string]$Label
    )

    Write-Host "[*] Building $Label ..." -ForegroundColor Yellow

    $cmd = @"
@echo off
call "$devCmd" -arch=$Platform -host_arch=amd64 >nul
cd /d "$WorkDir"
cl.exe $ClArgs
if errorlevel 1 exit /b 1
"@
    $bat = Join-Path $env:TEMP ("build_procmon_" + [guid]::NewGuid().ToString("N") + ".bat")
    [System.IO.File]::WriteAllText($bat, $cmd, [System.Text.Encoding]::ASCII)
    & cmd.exe /c "`"$bat`""
    $code = $LASTEXITCODE
    Remove-Item $bat -Force -ErrorAction SilentlyContinue
    if ($code -ne 0) {
        Write-Error "Compilation failed for $Label (exit $code)."
        exit $code
    }
}

# --- 1) Production user-mode app ---
if (-not $SkipApp) {
    if (-not (Test-Path $AppSource)) {
        Write-Error "Source not found: $AppSource"
        exit 1
    }
    New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

    $appCl = "$cflags $inc `"procmon_app.cpp`" /Fe`"$OutExe`" /Fo`"$OutDir\\`" kernel32.lib user32.lib advapi32.lib"
    Invoke-ClBuild -WorkDir $UserModeDir -ClArgs $appCl -Label "procmon_app.exe"

    if (-not (Test-Path $OutExe)) {
        Write-Error "Expected output missing: $OutExe"
        exit 1
    }
    Write-Host "[+] App: $OutExe" -ForegroundColor Green
}

# --- 2) Unified mock / ring / protocol tests ---
if (-not $SkipTests) {
    if (-not (Test-Path $TestSource)) {
        Write-Error "Test source not found: $TestSource"
        exit 1
    }
    New-Item -ItemType Directory -Force -Path $TestOutDir | Out-Null

    # main_test_runner.cpp is self-contained (includes shared_protocol.h + types.h)
    $testCl = "$cflags $inc `"main_test_runner.cpp`" /Fe`"$TestExe`" /Fo`"$TestOutDir\\`" kernel32.lib user32.lib advapi32.lib"
    Invoke-ClBuild -WorkDir $MockDir -ClArgs $testCl -Label "procmon_unified_tests.exe"

    if (-not (Test-Path $TestExe)) {
        Write-Error "Expected test output missing: $TestExe"
        exit 1
    }
    Write-Host "[+] Tests: $TestExe" -ForegroundColor Green

    if ($RunTests) {
        Write-Host ""
        Write-Host "[*] Running unified tests ..." -ForegroundColor Cyan
        & $TestExe
        if ($LASTEXITCODE -ne 0) {
            Write-Error "Unified tests failed (exit $LASTEXITCODE)."
            exit $LASTEXITCODE
        }
        Write-Host "[+] All unified tests passed." -ForegroundColor Green
    }
}

Write-Host ""
Write-Host "================================================" -ForegroundColor Green
Write-Host "  BUILD SUCCESSFUL" -ForegroundColor Green
if (-not $SkipApp)   { Write-Host "  App   : $OutExe" -ForegroundColor Green }
if (-not $SkipTests) { Write-Host "  Tests : $TestExe" -ForegroundColor Green }
Write-Host "================================================" -ForegroundColor Green
Write-Host ""
Write-Host "Examples:" -ForegroundColor Cyan
Write-Host "  .\Build-UserMode.ps1"
Write-Host "  .\Build-UserMode.ps1 -RunTests"
Write-Host "  .\Build-UserMode.ps1 -SkipApp -RunTests"
Write-Host "  .\Build-UserMode.ps1 -SkipTests"
