#Requires -RunAsAdministrator
# ================================================
# uninstall_procmon.ps1
# Stops and removes ProcMonDrv completely.
# ================================================

$ErrorActionPreference = "Continue"
$serviceName   = "ProcMonDrv"
$sysModuleName = "procmon_driver.sys"
$pdbModuleName = "procmon_driver.pdb"
$targetSys     = "C:\Windows\System32\drivers\$sysModuleName"
$targetPdb     = "C:\Windows\System32\drivers\$pdbModuleName"
$svcKey        = "HKLM:\SYSTEM\CurrentControlSet\Services\$serviceName"

Write-Host ""
Write-Host "================================================" -ForegroundColor Cyan
Write-Host "  ProcessMonitor - Uninstall" -ForegroundColor Cyan
Write-Host "================================================" -ForegroundColor Cyan
Write-Host ""

# 1. Stop service
Write-Host "[*] Stopping service..." -ForegroundColor Yellow
& sc.exe stop $serviceName 2>$null | Out-Null
Start-Sleep -Seconds 1

# 2. Delete service
Write-Host "[*] Deleting service..." -ForegroundColor Yellow
& sc.exe delete $serviceName 2>$null | Out-Null
Start-Sleep -Milliseconds 500

# 3. Remove registry tree
if (Test-Path $svcKey) {
    Remove-Item -Path $svcKey -Recurse -Force -ErrorAction SilentlyContinue
    Write-Host "[+] Service registry key removed." -ForegroundColor Green
} else {
    Write-Host "[*] No service registry key found." -ForegroundColor DarkYellow
}

# 4. Remove binary files (optional – ignore if locked)
foreach ($f in @($targetSys, $targetPdb)) {
    if (Test-Path $f) {
        try {
            Remove-Item -Path $f -Force -ErrorAction Stop
            Write-Host "[+] Removed $f" -ForegroundColor Green
        } catch {
            Write-Host "[-] Could not delete $f (in use?). Delete after reboot." -ForegroundColor Red
        }
    }
}

Write-Host ""
Write-Host "=== Verification ===" -ForegroundColor Cyan
& sc.exe query $serviceName 2>$null
Write-Host ""
Write-Host "[+] Uninstall finished. You can reinstall with install_procmon.ps1" -ForegroundColor Green
Write-Host ""
