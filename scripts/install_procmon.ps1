#Requires -RunAsAdministrator
# ================================================
# install_procmon.ps1
# Installs / reinstalls the Process Monitor
# WDM kernel driver (ProcMonDrv) on the TARGET machine.
#
# Service type : kernel (SERVICE_KERNEL_DRIVER)
# Start type   : demand (SERVICE_DEMAND_START)
#
# Usage:
#   .\install_procmon.ps1
#   .\install_procmon.ps1 -Configuration Release
# ================================================

[CmdletBinding()]
param(
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Debug",

    [string]$Platform = "x64"
)

$ErrorActionPreference = "Stop"

$ScriptRoot  = $PSScriptRoot
$ProjectRoot = Split-Path $ScriptRoot -Parent
$DriverDir   = Join-Path $ProjectRoot "driver"

$serviceName   = "ProcMonDrv"
$sysModuleName = "procmon_driver.sys"
$pdbModuleName = "procmon_driver.pdb"
$displayName   = "Process Activity Monitor Driver"
$targetSys     = "C:\Windows\System32\drivers\$sysModuleName"
$targetPdb     = "C:\Windows\System32\drivers\$pdbModuleName"
$svcKey        = "HKLM:\SYSTEM\CurrentControlSet\Services\$serviceName"

Write-Host ""
Write-Host "================================================" -ForegroundColor Cyan
Write-Host "  ProcessMonitor - Driver Installation" -ForegroundColor Cyan
Write-Host "  Service  : $serviceName  (WDM kernel)" -ForegroundColor Cyan
Write-Host "================================================" -ForegroundColor Cyan
Write-Host ""

# ------------------------------------------------
# 1. Locate built binary
# ------------------------------------------------
$candidates = @(
    (Join-Path $DriverDir "$Platform\$Configuration\$sysModuleName"),
    (Join-Path $DriverDir $sysModuleName),
    (Join-Path $ScriptRoot "..\driver\$Platform\$Configuration\$sysModuleName")
)

$sourceSys = $null
foreach ($c in $candidates) {
    if (Test-Path $c) {
        $sourceSys = (Resolve-Path $c).Path
        break
    }
}

if (-not $sourceSys) {
    Write-Error "$sysModuleName not found. Build with Build-Driver.ps1 first."
    exit 1
}

Write-Host "[OK] Found driver binary: $sourceSys" -ForegroundColor Green

# ------------------------------------------------
# 2. Stop / delete existing service
# ------------------------------------------------
Write-Host "Stopping / deleting old service (if any)..." -ForegroundColor Yellow
& sc.exe stop $serviceName 2>$null | Out-Null
Start-Sleep -Seconds 1
& sc.exe delete $serviceName 2>$null | Out-Null
Start-Sleep -Milliseconds 500

# Remove leftover registry tree so we start clean
if (Test-Path $svcKey) {
    Remove-Item -Path $svcKey -Recurse -Force -ErrorAction SilentlyContinue
}

# ------------------------------------------------
# 3. Copy .sys (+ .pdb)
# ------------------------------------------------
Write-Host "Copying $sysModuleName -> $targetSys ..." -ForegroundColor Yellow
Copy-Item -Path $sourceSys -Destination $targetSys -Force
if (-not (Test-Path $targetSys)) {
    Write-Error "Failed to copy driver. Run as Administrator."
    exit 1
}
Write-Host "[SUCCESS] Driver binary copied." -ForegroundColor Green

$pdbSource = [System.IO.Path]::ChangeExtension($sourceSys, ".pdb")
if (Test-Path $pdbSource) {
    Copy-Item -Path $pdbSource -Destination $targetPdb -Force
    Write-Host "[SUCCESS] PDB copied." -ForegroundColor Green
} else {
    Write-Host "[WARNING] PDB not found - source debugging limited." -ForegroundColor Yellow
}

# ------------------------------------------------
# 4. Create kernel service
# ------------------------------------------------
Write-Host "Creating kernel service..." -ForegroundColor Yellow
& sc.exe create $serviceName type= kernel binPath= "$targetSys" DisplayName= "$displayName" start= demand
if ($LASTEXITCODE -ne 0) {
    Write-Error "sc create failed (exit $LASTEXITCODE)."
    exit 1
}
Write-Host "[SUCCESS] Service created." -ForegroundColor Green

# ------------------------------------------------
# 5. Start
# ------------------------------------------------
Write-Host "Starting driver..." -ForegroundColor Yellow
& sc.exe start $serviceName
if ($LASTEXITCODE -ne 0) {
    Write-Host ""
    Write-Host "[ERROR] sc start failed (exit $LASTEXITCODE)." -ForegroundColor Red
    Write-Host "Common causes:" -ForegroundColor Yellow
    Write-Host "  - Driver is unsigned         -> rebuild with test-signing / SignMode=TestSign"
    Write-Host "  - Test-signing not enabled   -> bcdedit /set testsigning on  (reboot)"
    Write-Host "  - Check Event Viewer -> System (Service Control Manager)"
    Write-Host ""
    exit 1
}

Write-Host "[SUCCESS] Driver started." -ForegroundColor Green
Write-Host ""

# ------------------------------------------------
# 6. Verification
# ------------------------------------------------
Write-Host "=== Verification ===" -ForegroundColor Cyan
& sc.exe query $serviceName
Write-Host ""

Write-Host "================================================" -ForegroundColor Green
Write-Host "  INSTALLATION COMPLETE" -ForegroundColor Green
Write-Host "  Run procmon_app.exe as Administrator." -ForegroundColor Green
Write-Host "================================================" -ForegroundColor Green
