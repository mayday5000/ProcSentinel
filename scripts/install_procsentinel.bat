@echo off
setlocal enabledelayedexpansion

:: ================================================
:: install_procsentinel.bat
:: Same logic as install_procsentinel.ps1
:: WDM kernel service (sc create type= kernel)
:: Paths are relative to this script (%~dp0), so it also works from
:: "Run as administrator" (which starts in C:\Windows\System32).
:: ================================================

:: 0. Require an elevated prompt
net session >nul 2>&1
if errorlevel 1 (
    echo [ERROR] Run this script from an elevated ^(Administrator^) prompt.
    pause
    exit /b 1
)

set "serviceName=ProcSentinelDrv"
set "sysModuleName=procsentinel_driver.sys"
set "pdbModuleName=procsentinel_driver.pdb"
set "displayName=Process Activity Monitor Driver"
set "targetSys=%SystemRoot%\System32\drivers\%sysModuleName%"
set "targetPdb=%SystemRoot%\System32\drivers\%pdbModuleName%"
set "scriptDir=%~dp0"

set "relativeDriverPath=%scriptDir%..\driver\x64\Debug"
set "sourceSys=%relativeDriverPath%\%sysModuleName%"
set "sourcePdb=%relativeDriverPath%\%pdbModuleName%"

echo.
echo ================================================
echo   ProcSentinel - Driver Installation
echo   Service : %serviceName%  (WDM kernel)
echo ================================================
echo.

:: 1. Locate binary
if exist "%sourceSys%" goto :found
set "relativeDriverPath=%scriptDir%..\driver\x64\Release"
set "sourceSys=%relativeDriverPath%\%sysModuleName%"
set "sourcePdb=%relativeDriverPath%\%pdbModuleName%"
if exist "%sourceSys%" goto :found
set "relativeDriverPath=%scriptDir%..\driver"
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
    echo   - Access denied ^(5^)          -^> driver not linked with /INTEGRITYCHECK
    echo   - Windows older than 10 2004 -^> ExAllocatePool2 import cannot be resolved
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
echo   Run procsentinel_app.exe from an elevated prompt.
echo ================================================
echo.
pause
