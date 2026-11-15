@echo off
setlocal enabledelayedexpansion

:: ================================================
:: install_procmon.bat
:: Same logic as install_procmon.ps1
:: WDM kernel service (sc create type= kernel)
:: ================================================

set "serviceName=ProcMonDrv"
set "sysModuleName=procmon_driver.sys"
set "pdbModuleName=procmon_driver.pdb"
set "displayName=Process Activity Monitor Driver"
set "targetSys=C:\Windows\System32\drivers\%sysModuleName%"
set "targetPdb=C:\Windows\System32\drivers\%pdbModuleName%"

set "relativeDriverPath=..\driver\x64\Debug"
set "sourceSys=%relativeDriverPath%\%sysModuleName%"
set "sourcePdb=%relativeDriverPath%\%pdbModuleName%"

echo.
echo ================================================
echo   ProcessMonitor - Driver Installation
echo   Service : %serviceName%  (WDM kernel)
echo ================================================
echo.

:: 1. Locate binary
if exist "%sourceSys%" goto :found
set "relativeDriverPath=..\driver\x64\Release"
set "sourceSys=%relativeDriverPath%\%sysModuleName%"
set "sourcePdb=%relativeDriverPath%\%pdbModuleName%"
if exist "%sourceSys%" goto :found
set "relativeDriverPath=..\driver"
set "sourceSys=%relativeDriverPath%\%sysModuleName%"
set "sourcePdb=%relativeDriverPath%\%pdbModuleName%"
if exist "%sourceSys%" goto :found
echo [ERROR] %sysModuleName% not found. Build with Build-Driver.ps1 first.
pause
exit /b 1

:found
echo [OK] Found driver binary: %sourceSys%
echo.

:: 2. Stop / delete
echo Stopping / deleting old service (if any)...
sc.exe stop %serviceName% >nul 2>&1
timeout /t 1 /nobreak >nul
sc.exe delete %serviceName% >nul 2>&1
timeout /t 1 /nobreak >nul
reg delete "HKLM\SYSTEM\CurrentControlSet\Services\%serviceName%" /f >nul 2>&1

:: 3. Copy
echo Copying %sysModuleName% to %targetSys%...
copy /Y "%sourceSys%" "%targetSys%" >nul
if errorlevel 1 (
    echo [ERROR] Failed to copy driver. Run as Administrator.
    pause
    exit /b 1
)
echo [SUCCESS] Driver binary copied.

if exist "%sourcePdb%" (
    copy /Y "%sourcePdb%" "%targetPdb%" >nul
    echo [SUCCESS] PDB copied.
) else (
    echo [WARNING] PDB not found.
)
echo.

:: 4. Create service
echo Creating kernel service...
sc.exe create %serviceName% type= kernel binPath= "%targetSys%" DisplayName= "%displayName%" start= demand
if errorlevel 1 (
    echo [ERROR] sc create failed.
    pause
    exit /b 1
)
echo [SUCCESS] Service created.

:: 5. Start
echo Starting driver...
sc.exe start %serviceName%
if errorlevel 1 (
    echo.
    echo [ERROR] sc start failed.
    echo Common causes:
    echo   - Driver is unsigned         -^> rebuild with test-signing
    echo   - Test-signing not enabled   -^> bcdedit /set testsigning on  ^(reboot^)
    echo   - Check Event Viewer -^> System
    echo.
    pause
    exit /b 1
)

echo [SUCCESS] Driver started.
echo.

:: 6. Verify
echo === Verification ===
sc.exe query %serviceName%
echo.

echo ================================================
echo   INSTALLATION COMPLETE
echo   Run procmon_app.exe as Administrator.
echo ================================================
echo.
pause
