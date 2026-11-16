@echo off
setlocal
set "serviceName=ProcSentinelDrv"
set "sysModuleName=procsentinel_driver.sys"
set "pdbModuleName=procsentinel_driver.pdb"
set "driversDir=%SystemRoot%\System32\drivers"

net session >nul 2>&1
if errorlevel 1 (
    echo [ERROR] Run this script from an elevated ^(Administrator^) prompt.
    pause
    exit /b 1
)

echo.
echo ================================================
echo   ProcSentinel - Uninstall
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

if exist "%driversDir%\%sysModuleName%" (
    del /f "%driversDir%\%sysModuleName%" 2>nul
    if exist "%driversDir%\%sysModuleName%" (
        echo [-] Could not delete .sys ^(in use?^)
    ) else (
        echo [+] Removed .sys
    )
)
if exist "%driversDir%\%pdbModuleName%" (
    del /f "%driversDir%\%pdbModuleName%" 2>nul
)

echo.
echo === Verification ===
sc.exe query %serviceName% 2>nul
echo.
echo [+] Uninstall finished.
echo.
pause
