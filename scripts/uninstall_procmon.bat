@echo off
setlocal
set "serviceName=ProcMonDrv"
set "sysModuleName=procmon_driver.sys"
set "pdbModuleName=procmon_driver.pdb"

echo.
echo ================================================
echo   ProcessMonitor - Uninstall
echo ================================================
echo.

echo [*] Stopping service...
sc.exe stop %serviceName% >nul 2>&1
timeout /t 1 /nobreak >nul

echo [*] Deleting service...
sc.exe delete %serviceName% >nul 2>&1
timeout /t 1 /nobreak >nul

echo [*] Removing registry...
reg delete "HKLM\SYSTEM\CurrentControlSet\Services\%serviceName%" /f >nul 2>&1

if exist "C:\Windows\System32\drivers\%sysModuleName%" (
    del /f "C:\Windows\System32\drivers\%sysModuleName%" 2>nul
    if exist "C:\Windows\System32\drivers\%sysModuleName%" (
        echo [-] Could not delete .sys ^(in use?^)
    ) else (
        echo [+] Removed .sys
    )
)
if exist "C:\Windows\System32\drivers\%pdbModuleName%" (
    del /f "C:\Windows\System32\drivers\%pdbModuleName%" 2>nul
)

echo.
echo === Verification ===
sc.exe query %serviceName% 2>nul
echo.
echo [+] Uninstall finished.
echo.
pause
