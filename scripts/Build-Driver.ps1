# ================================================
# Build-Driver.ps1
# Builds the ProcSentinel kernel driver
# (procsentinel_driver.sys)
#
# After a successful build the script also generates:
#   - driver\debug.wds
#   - driver\WinDbgWorkspace.xml
#   - scripts\KDNet-ProcSentinelDrv.ps1   (Port + Key: parameters or prompt)
#
# Requirements:
#   - Visual Studio 2019 / 2022 with "Desktop development with C++"
#   - Windows Driver Kit (WDK) matching the Windows SDK version
#     (10.0.19041 / Windows 10 2004 or newer: the driver uses ExAllocatePool2)
#   - The project links with /INTEGRITYCHECK and wdmsec.lib (set in the vcxproj)
#   - A Visual Studio project (*.vcxproj / *.sln) created from the
#     official "Empty WDM Driver" template, with the source replaced by
#     driver\procsentinel_driver.c  OR  an existing solution that
#     already contains the driver project.
#
# Usage:
#   .\Build-Driver.ps1
#   .\Build-Driver.ps1 -Configuration Release -Platform x64
#   .\Build-Driver.ps1 -Clean
#   .\Build-Driver.ps1 -KdnetPort 50008 -KdnetKey a.b.c.d -KdnetHostIp 192.168.1.10
#
# If -KdnetPort / -KdnetKey / -KdnetHostIp are omitted you are prompted
# (press Enter to accept the default shown).
# ================================================

[CmdletBinding()]
param(
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Debug",

    # The vcxproj only defines x64 configurations.
    [ValidateSet("x64")]
    [string]$Platform = "x64",

    [switch]$Clean,

    # KDNET settings written into WinDbgWorkspace.xml / the KDNet launcher.
    [string]$KdnetPort,
    [string]$KdnetKey,
    [string]$KdnetHostIp
)

$ErrorActionPreference = "Stop"
$ScriptRoot   = $PSScriptRoot
$ProjectRoot  = Split-Path $ScriptRoot -Parent
$DriverDir    = Join-Path $ProjectRoot "driver"

# Fixed names used throughout generation
$driverName      = "ProcSentinelDrv"
$sysModuleName   = "procsentinel_driver.sys"
$symbolModule    = "procsentinel_driver"          # name used in breakpoints (no .sys)

Write-Host "================================================" -ForegroundColor Cyan
Write-Host "  ProcSentinel - Kernel Driver Builder" -ForegroundColor Cyan
Write-Host "================================================" -ForegroundColor Cyan
Write-Host "Configuration : $Configuration"
Write-Host "Platform      : $Platform"
Write-Host "Driver dir    : $DriverDir"
Write-Host ""

# ------------------------------------------------
# 1. Locate MSBuild via vswhere
# ------------------------------------------------
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) {
    Write-Error "vswhere.exe not found. Install Visual Studio 2019/2022 with C++ workload."
    exit 1
}

$msbuildPath = & $vswhere -latest -products * `
    -requires Microsoft.Component.MSBuild `
    -find "MSBuild\**\Bin\MSBuild.exe" |
    Select-Object -First 1

if (-not $msbuildPath) {
    Write-Error "MSBuild.exe not found."
    exit 1
}

Write-Host "[OK] MSBuild: $msbuildPath" -ForegroundColor Green

# ------------------------------------------------
# 2. Locate solution or project file
# ------------------------------------------------
$sln = Get-ChildItem -Path $DriverDir -Filter "*.sln" -ErrorAction SilentlyContinue |
       Select-Object -First 1
$vcxproj = Get-ChildItem -Path $DriverDir -Filter "*.vcxproj" -ErrorAction SilentlyContinue |
           Select-Object -First 1

if ($sln) {
    $buildTarget = $sln.FullName
    Write-Host "[OK] Solution found: $($sln.Name)" -ForegroundColor Green
}
elseif ($vcxproj) {
    $buildTarget = $vcxproj.FullName
    Write-Host "[OK] Project found: $($vcxproj.Name)" -ForegroundColor Green
}
else {
    Write-Host ""
    Write-Host "No .sln or .vcxproj found in $DriverDir" -ForegroundColor Yellow
    Write-Host ""
    Write-Host "HOW TO CREATE THE PROJECT (one-time):" -ForegroundColor Cyan
    Write-Host "  1. Open Visual Studio → Create a new project"
    Write-Host "  2. Search for 'Empty WDM Driver'"
    Write-Host "  3. Name it procsentinel_driver, place it in the 'driver' folder"
    Write-Host "  4. Replace the generated .c file with procsentinel_driver.c"
    Write-Host "  5. Add the .inf file to the project"
    Write-Host "  6. Re-run this script"
    Write-Host ""
    Write-Error "Build target not found."
    exit 1
}

# ------------------------------------------------
# 3. Optional clean
# ------------------------------------------------
if ($Clean) {
    Write-Host "Cleaning previous build artifacts..." -ForegroundColor Yellow
    $x64Dir = Join-Path $DriverDir "x64"
    if (Test-Path $x64Dir) {
        Remove-Item $x64Dir -Recurse -Force -ErrorAction SilentlyContinue
    }
    $vsDir = Join-Path $DriverDir ".vs"
    if (Test-Path $vsDir) {
        Remove-Item $vsDir -Recurse -Force -ErrorAction SilentlyContinue
    }
}

# ------------------------------------------------
# 4. Rebuild
# ------------------------------------------------
Write-Host "Building..." -ForegroundColor Yellow

$msbuildArgs = @(
    "`"$buildTarget`"",
    "/p:Configuration=$Configuration",
    "/p:Platform=$Platform",
    "/t:Rebuild",
    "/m",
    "/nologo",
    "/v:minimal"
)

& $msbuildPath @msbuildArgs
$msbuildExit = $LASTEXITCODE
if ($msbuildExit -ne 0) {
    # Never continue with a stale binary from an earlier build.
    Write-Error "MSBuild failed with exit code $msbuildExit."
    exit $msbuildExit
}

# ------------------------------------------------
# 5. Locate the produced .sys (only this configuration's output folder)
# ------------------------------------------------
$sysSearch = Join-Path $DriverDir "$Platform\$Configuration"
$sysPath   = Join-Path $sysSearch $sysModuleName
$sysFile   = Get-Item -Path $sysPath -ErrorAction SilentlyContinue

if (-not $sysFile) {
    Write-Error "MSBuild reported success but $sysPath was not found."
    exit 1
}

Write-Host ""
Write-Host "================================================" -ForegroundColor Green
Write-Host "  BUILD SUCCESSFUL" -ForegroundColor Green
Write-Host "  Driver : $($sysFile.FullName)" -ForegroundColor Green
Write-Host "================================================" -ForegroundColor Green

# ================================================================
# 6. Generate debug.wds
# ================================================================
Write-Host ""
Write-Host "Generating debug.wds ..." -ForegroundColor Cyan

$driverPathAbs = (Resolve-Path $DriverDir).Path
$debugBinPath  = Join-Path $driverPathAbs "$Platform\$Configuration"
$wdsPath       = Join-Path $driverPathAbs "debug.wds"

$wdsContent = @"
!sym noisy
.sympath $debugBinPath;srv*C:\Symbols*https://msdl.microsoft.com/download/symbols
.reload /f /i $sysModuleName

.echo [READY] Symbol path and reload complete.

bp ${symbolModule}!DriverEntry "echo [BP] === DriverEntry ===; dv; g"
bp ${symbolModule}!ProcessNotifyCallback "echo [BP] === ProcessNotifyCallback ===; dv; g"
bp ${symbolModule}!DispatchDeviceControl "echo [BP] === DispatchDeviceControl ===; g"
bl

.echo [READY] Breakpoints are set. Run user-mode app now.
"@

$utf8NoBom = New-Object System.Text.UTF8Encoding($false)
[System.IO.File]::WriteAllText($wdsPath, $wdsContent, $utf8NoBom)
Write-Host "[OK] debug.wds written to: $wdsPath" -ForegroundColor Green

# ================================================================
# 7. Generate WinDbgWorkspace.xml
# ================================================================
Write-Host "Generating WinDbgWorkspace.xml ..." -ForegroundColor Cyan

$workspacePath = Join-Path $driverPathAbs "WinDbgWorkspace.xml"

# Cleaned workspace: keeps window layout, engine settings, theme and
# KernelConnect placeholders.  RecentTargets / OpenDump lists removed.
$workspaceXml = @"
<?xml version="1.0" encoding="utf-8"?>
<Settings>
  <Setting Name="bf8985ae-1b0c-4fa3-9634-8269e0fe8ec2.DisplayAsGrid" Value="True" />
  <Setting Name="MainWindow.WindowPlacement" Value="44;0;1;-1;-1;-1;-1;108;13;1668;888" />
  <Setting Name="7e8fe6c6-4e5f-4b43-a671-02eb73e59221.DisplayAsGrid" Value="True" />
  <Setting Name="ce27cf04-0f90-438a-b7d5-f74ed71927b3.DisplayAsGrid" Value="True" />
  <Setting Name="51df929c-1f31-4bb5-99e2-01d32350e086.DisplayAsGrid" Value="True" />
  <Setting Name="fe271dd3-4e5b-4989-96fb-5a2786083c8e.DisplayAsGrid" Value="True" />
  <Setting Name="c07822a1-8ba2-4a09-a6ba-b5d9eb445c72.DisplayAsGrid" Value="True" />
  <Setting Name="ea4d6cad-4e96-4965-a648-8bb4f55a42ea.DisplayAsGrid" Value="True" />
  <Setting Name="724213eb-ed4c-473f-9360-8da5e8804386.DisplayAsGrid" Value="True" />
  <Setting Name="KernelConnect.Key" Value="__KDNET_KEY__" />
  <Setting Name="381502f2-c76b-4907-8ae6-923cef6aad4c.DisplayAsGrid" Value="False" />
  <Setting Name="0f950c2a-981e-4c10-baca-217f98fac022.Value.Width" Value="764" />
  <Setting Name="Breakpoints.LocationWidth" Value="516" />
  <Setting Name="EulaState" Value="accepted" />
  <Setting Name="KernelConnect.Target" Value="__KDNET_HOST_IP__" />
  <Setting Name="KernelConnect.InitialBreak" Value="True" />
  <Setting Name="KernelConnect.Port" Value="__KDNET_PORT__" />
  <XmlSetting Name="WindowPreferences">
    <WindowProfile xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml" xmlns="clr-namespace:Microsoft.VisualStudio.PlatformUI.Shell;assembly=Microsoft.Performance.Shell">
      <MainSite IsVisible="True">
        <AutoHideRoot IsVisible="True" DockedHeight="*" DockedWidth="*">
          <AutoHideChannel Orientation="Vertical" />
          <AutoHideChannel Dock="Right" Orientation="Vertical" />
          <AutoHideChannel Dock="Top" />
          <AutoHideChannel Dock="Bottom" />
          <DockRoot IsVisible="True" DockedHeight="*" DockedWidth="*">
            <DockGroup IsVisible="True" DockedHeight="*" DockedWidth="*">
              <ToolWindowInstance IsPersisted="True" ToolWindowName="RegistersToolWindow" Name="6159273f-28c4-4008-88ed-73f7a4f837ea" IsVisible="True" DockedHeight="868" DockedWidth="44.000000000000014" FloatingTop="559" FloatingLeft="112" FloatingHeight="876" FloatingWidth="552" xmlns="clr-namespace:DbgX.Shell;assembly=DbgX.Shell" />
              <DockGroup Orientation="Vertical" IsVisible="True" DockedHeight="*" DockedWidth="*">
                <DocumentGroupContainer IsVisible="True" DockedHeight="*" DockedWidth="*">
                  <DocumentGroup DockedWidth="229.16812098705253">
                    <ViewBookmark ToolWindowName="SourceToolWindow" ViewBookmarkType="OpenLocation" DockedWidth="229.16812098705253" />
                  </DocumentGroup>
                  <DocumentGroup IsVisible="True" DockedHeight="586" DockedWidth="241.27976452830882">
                    <ToolWindowInstance IsPersisted="True" ToolWindowName="CommandToolWindow" Name="aeb4ceb9-413b-45b6-a957-0754cd627f21" IsSelected="True" IsVisible="True" DockedHeight="586" DockedWidth="241.27976452830882" FloatingTop="271" FloatingLeft="1171" FloatingHeight="586" FloatingWidth="1260" xmlns="clr-namespace:DbgX.Shell;assembly=DbgX.Shell" />
                  </DocumentGroup>
                </DocumentGroupContainer>
                <TabGroup IsVisible="True" DockedHeight="185" DockedWidth="350.274943725426">
                  <ViewBookmark Name="0b67f917-a3f3-4d00-8a1c-4ae94dc42a14" DockedHeight="185" DockedWidth="350.274943725426" />
                  <ToolWindowInstance IsPersisted="True" ToolWindowName="WatchToolWindow" Name="ff139909-0942-4f41-b240-5ae037aa8378" IsVisible="True" DockedHeight="185" DockedWidth="350.274943725426" FloatingTop="906" FloatingLeft="357" xmlns="clr-namespace:DbgX.Shell;assembly=DbgX.Shell" />
                  <ToolWindowInstance IsPersisted="True" ToolWindowName="StackToolWindow" Name="438f0046-9bcd-46d5-a2e4-24a7ffe9d760" IsVisible="True" DockedHeight="185" DockedWidth="350.274943725426" FloatingTop="853" FloatingLeft="1223" xmlns="clr-namespace:DbgX.Shell;assembly=DbgX.Shell" />
                  <ToolWindowInstance IsPersisted="True" ToolWindowName="MemoryToolWindow" Name="d4e028e8-4686-4719-90ae-9d11444f73e3" IsVisible="True" DockedHeight="185" DockedWidth="350.274943725426" FloatingTop="856" FloatingLeft="1080" FloatingHeight="876" FloatingWidth="287" xmlns="clr-namespace:DbgX.Shell;assembly=DbgX.Shell" />
                  <ViewBookmark ToolWindowName="BreakpointsToolWindow" ViewBookmarkType="OpenLocation" DockedHeight="185" DockedWidth="350.274943725426" />
                  <ToolWindowInstance IsPersisted="True" ToolWindowName="BreakpointsToolWindow" Name="a6d5e0cd-1520-4c84-86de-a651a40a707e" IsVisible="True" DockedHeight="185" DockedWidth="350.274943725426" FloatingTop="686" FloatingLeft="736" xmlns="clr-namespace:DbgX.Shell;assembly=DbgX.Shell" />
                  <ViewBookmark ToolWindowName="LocalsToolWindow" ViewBookmarkType="OpenLocation" DockedHeight="185" DockedWidth="350.274943725426" />
                  <ToolWindowInstance IsPersisted="True" ToolWindowName="LocalsToolWindow" ToolWindowParameter="cb1ee2d0-b527-46ae-a9c2-49711e91534f;Debugger.Utility.Objects.Aggregate(@`$curframe.LocalVariables, @`$curframe.Parameters);false;False" Name="6f9c442e-53dd-47b6-8ac2-17e01750bd21" IsVisible="True" DockedHeight="185" DockedWidth="350.274943725426" FloatingTop="985" FloatingLeft="637" xmlns="clr-namespace:DbgX.Shell;assembly=DbgX.Shell" />
                  <ViewBookmark ToolWindowName="ModulesToolWindow" ViewBookmarkType="OpenLocation" DockedHeight="185" DockedWidth="350.274943725426" />
                  <ToolWindowInstance IsPersisted="True" ToolWindowName="ModulesToolWindow" ToolWindowParameter="f89e362b-dc50-46ef-ae55-53522c789809;@`$curprocess.Modules;false;True" Name="e7e0e8c1-318e-43ea-9a29-5761d70d2f7f" IsVisible="True" DockedHeight="185" DockedWidth="350.274943725426" FloatingTop="985" FloatingLeft="765" xmlns="clr-namespace:DbgX.Shell;assembly=DbgX.Shell" />
                  <ViewBookmark ToolWindowName="LoggingToolWindow" ViewBookmarkType="OpenLocation" DockedHeight="185" DockedWidth="350.274943725426" />
                  <ToolWindowInstance IsPersisted="True" ToolWindowName="LoggingToolWindow" Name="4d3d25b8-d1e3-4dba-95a0-04cfbf147409" IsSelected="True" IsVisible="True" DockedHeight="185" DockedWidth="350.274943725426" FloatingTop="974" FloatingLeft="862" FloatingHeight="459" FloatingWidth="1254" xmlns="clr-namespace:DbgX.Shell;assembly=DbgX.Shell" />
                </TabGroup>
              </DockGroup>
              <ViewBookmark Name="91d5af15-0d99-4ed4-81d0-8d98e0281c2c" />
              <TabGroup IsSelected="True" IsVisible="True" DockedHeight="730" DockedWidth="458" FloatingTop="617" FloatingLeft="1769" FloatingHeight="730" FloatingWidth="297">
                <ViewBookmark ToolWindowName="DisassemblyToolWindow" ViewBookmarkType="OpenLocation" DockedHeight="730" DockedWidth="458" FloatingHeight="730" FloatingWidth="297" />
                <ToolWindowInstance IsPersisted="True" ToolWindowName="DisassemblyToolWindow" Name="2c684bec-2263-4c79-a1c2-fb17f381eb74" IsSelected="True" IsVisible="True" DockedHeight="730" DockedWidth="458" FloatingTop="617" FloatingLeft="1769" FloatingHeight="730" FloatingWidth="297" xmlns="clr-namespace:DbgX.Shell;assembly=DbgX.Shell" />
              </TabGroup>
            </DockGroup>
          </DockRoot>
        </AutoHideRoot>
      </MainSite>
    </WindowProfile>
  </XmlSetting>
  <XmlSetting Name="EngineSettings">
    <Settings Version="2">
      <Namespace Name="Debug">
        <Namespace Name="Advanced">
          <Setting Name="AutoDetectGuestMachine" Type="VT_BOOL" Value="true"></Setting>
          <Setting Name="AllowHardwareBreakpoints" Type="VT_BOOL" Value="true"></Setting>
          <Setting Name="BreakOnTargetRequestedCommand" Type="VT_BOOL" Value="false"></Setting>
          <Setting Name="MapImageMemoryForKernelDumps" Type="VT_BOOL" Value="false"></Setting>
          <Setting Name="MaxClrModules" Type="VT_INT" Value="1000"></Setting>
          <Setting Name="PackLinuxSignalToExceptionCode" Type="VT_BOOL" Value="true"></Setting>
          <Setting Name="PerThreadKernelBreakpointTracking" Type="VT_BOOL" Value="true"></Setting>
          <Setting Name="PerformSymbolSearchSubstitution" Type="VT_BOOL" Value="true"></Setting>
          <Setting Name="SupportRecursiveStepOver" Type="VT_BOOL" Value="true"></Setting>
          <Setting Name="VadBasedModuleEnumeration" Type="VT_BOOL" Value="true"></Setting>
          <Setting Name="RestartKdNetUsbDebugDevice" Type="VT_BOOL" Value="false"></Setting>
          <Setting Name="DisableKdNetRebootPoke" Type="VT_BOOL" Value="false"></Setting>
        </Namespace>
      </Namespace>
      <Namespace Name="Display">
        <Namespace Name="Disassembly">
          <Setting Name="IgnoreOutputWidth" Type="VT_BOOL" Value="false"></Setting>
          <Setting Name="DisplayRawBytes" Type="VT_BOOL" Value="true"></Setting>
          <Setting Name="DisplaySourceLines" Type="VT_BOOL" Value="false"></Setting>
        </Namespace>
        <Setting Name="StackTraceDepth" Type="VT_INT" Value="256"></Setting>
        <Setting Name="UnicodeStrings" Type="VT_BOOL" Value="false"></Setting>
        <Setting Name="DefaultRecursionDepth" Type="VT_INT" Value="1"></Setting>
        <Setting Name="DefaultDisplayRadix" Type="VT_INT" Value="16"></Setting>
        <Setting Name="PrintIntegersInDefaultRadix" Type="VT_BOOL" Value="false"></Setting>
        <Setting Name="PrintLongIntegersInDefaultRadix" Type="VT_BOOL" Value="false"></Setting>
        <Setting Name="UseDataModelForLocalsAndWatch" Type="VT_BOOL" Value="false"></Setting>
        <Setting Name="MaxSymbolDisplayLength" Type="VT_INT" Value="0"></Setting>
        <Setting Name="MaxStringConversionDisplayLength" Type="VT_INT" Value="512"></Setting>
        <Setting Name="DisplayShadowedVariablesWithSuffixes" Type="VT_BOOL" Value="false"></Setting>
      </Namespace>
      <Namespace Name="EngineInitialization">
        <Setting Name="AllowNetworkPaths" Type="VT_BOOL" Value="true"></Setting>
        <Setting Name="AllowShellCommands" Type="VT_BOOL" Value="true"></Setting>
        <Setting Name="SendTelemetry" Type="VT_BOOL" Value="true"></Setting>
        <Setting Name="VerifyFunctionTableCallbacks" Type="VT_BOOL" Value="true"></Setting>
        <Setting Name="UseReptTargetSupport" Type="VT_BOOL" Value="true"></Setting>
        <Setting Name="AllowSignExtensionMasmEvalExpression" Type="VT_BOOL" Value="false"></Setting>
        <Setting Name="PreferOpenTraceFilesFromDumps" Type="VT_BOOL" Value="false"></Setting>
        <Setting Name="UseRangeDumpFormatForKernelMemDump" Type="VT_BOOL" Value="true"></Setting>
        <Setting Name="SecureLoadDotNetExtensions" Type="VT_BOOL" Value="false"></Setting>
        <Setting Name="UseRangeDumpFormatForFullMemDump" Type="VT_BOOL" Value="true"></Setting>
        <Setting Name="ResolveAmbiguousBreakpoints" Type="VT_BOOL" Value="false"></Setting>
      </Namespace>
      <Namespace Name="Sources">
        <Setting Name="DisplaySourceLines" Type="VT_BOOL" Value="true"></Setting>
        <Setting Name="SourceSearchPath" Type="VT_BSTR" Value=""></Setting>
        <Setting Name="SkipCompilerIndicatedGlue" Type="VT_BOOL" Value="true"></Setting>
        <Setting Name="SkipCrtCode" Type="VT_BOOL" Value="false"></Setting>
        <Setting Name="SkipSCPCode" Type="VT_BOOL" Value="true"></Setting>
        <Setting Name="EnableExperimentalRemappings" Type="VT_INT" Value="0"></Setting>
        <Setting Name="DisabledRemappingTools" Type="VT_BSTR" Value=""></Setting>
        <Setting Name="SourceCodeDebugging" Type="VT_BOOL" Value="true"></Setting>
      </Namespace>
      <Namespace Name="Symbols">
        <Setting Name="Sympath" Type="VT_BSTR" Value="srv*"></Setting>
        <Namespace Name="Advanced">
          <Setting Name="MultithreadedSymbolLoading" Type="VT_BOOL" Value="true"></Setting>
          <Setting Name="PreferLastValidDebugDirectory" Type="VT_BOOL" Value="false"></Setting>
          <Setting Name="Arm64XSymbolFiltering" Type="VT_BSTR" Value="Automatic"></Setting>
        </Namespace>
      </Namespace>
      <Setting Name="AutoSaveSettings" Type="VT_BOOL" Value="false"></Setting>
    </Settings>
  </XmlSetting>
  <XmlSetting Name="ThemeSettings">
    <ThemeSettings>
      <Property name="ThemeMode" value="Dark" />
    </ThemeSettings>
  </XmlSetting>
  <XmlSetting Name="RecentTargetsService">
    <RecentTargetsService>
      <Property name="RecentTargets" />
    </RecentTargetsService>
  </XmlSetting>
  <XmlSetting Name="RecentTargetsDialog">
    <RecentTargetsDialog>
      <Property name="PinnedTargets" />
    </RecentTargetsDialog>
  </XmlSetting>
  <XmlSetting Name="RecentTargetsServiceV2">
    <RecentTargetsServiceV2>
      <Property name="RecentTargets" />
    </RecentTargetsServiceV2>
  </XmlSetting>
  <XmlSetting Name="DebuggingSettings">
    <DebuggingSettings>
      <EnableSymSrv2>
        <Property name="Value" value="Default (enabled)" />
      </EnableSymSrv2>
      <EnableNegativeAuthCacheFeature>
        <Property name="Value" value="Default (disabled)" />
      </EnableNegativeAuthCacheFeature>
    </DebuggingSettings>
  </XmlSetting>
  <XmlSetting Name="ShowTTDTimelines">
    <ShowTTDTimelines>
      <Property name="ShowTTDTimelines" value="true" />
    </ShowTTDTimelines>
  </XmlSetting>
</Settings>
"@

[System.IO.File]::WriteAllText($workspacePath, $workspaceXml, $utf8NoBom)
Write-Host "[OK] WinDbgWorkspace.xml written to: $workspacePath" -ForegroundColor Green

# ================================================================
# 8. Generate KDNet automation script (interactive Port + Key)
# ================================================================
Write-Host "Generating KDNet automation script..." -ForegroundColor Cyan

# Port / Key / host IP: take the parameters, otherwise prompt with defaults.
# The default key is a placeholder; use the key printed by "kdnet.exe" on the target.
$defaultPort   = "50008"
$defaultKey    = "1.2.3.4"
$defaultHostIp = "192.168.223.1"

function Get-KdnetValue([string]$Value, [string]$Prompt, [string]$Default) {
    if (-not [string]::IsNullOrWhiteSpace($Value)) { return $Value }
    $answer = Read-Host "$Prompt [$Default]"
    if ([string]::IsNullOrWhiteSpace($answer)) { return $Default }
    return $answer
}

$portInput   = Get-KdnetValue $KdnetPort   "Enter KDNET Port"    $defaultPort
$keyInput    = Get-KdnetValue $KdnetKey    "Enter KDNET Key "    $defaultKey
$hostIpInput = Get-KdnetValue $KdnetHostIp "Enter KDNET host IP" $defaultHostIp

Write-Host "Using Port=$portInput  Key=$keyInput  HostIp=$hostIpInput" -ForegroundColor Yellow

# Patch the workspace XML with the real Port / Key / host IP
$workspaceXml = $workspaceXml.Replace("__KDNET_PORT__", $portInput).Replace("__KDNET_KEY__", $keyInput).Replace("__KDNET_HOST_IP__", $hostIpInput)
[System.IO.File]::WriteAllText($workspacePath, $workspaceXml, $utf8NoBom)
Write-Host "[OK] WinDbgWorkspace.xml updated with Port/Key/HostIp" -ForegroundColor Green

# Build the KDNet launcher script name: KDNet- + $driverName + .ps1
$kdnetScriptName = "KDNet-$driverName.ps1"
$kdnetScriptPath = Join-Path $ScriptRoot $kdnetScriptName

$kdnetContent = @"
# ================================================
# WinDbg KDNET Launcher - $driverName
# Auto-generated by Build-Driver.ps1
# Loads Workspace + Debug Script and stops at prompt
# ================================================

`$winDbgPath     = "`$env:LOCALAPPDATA\Microsoft\WindowsApps\WinDbgX.exe"
`$workspacePath  = "$workspacePath"
`$wdsPath        = "$wdsPath"
`$symbolPath     = "$debugBinPath"

if (-not (Test-Path `$winDbgPath)) {
    Write-Error "WinDbg Preview not found! Please install it from the Microsoft Store."
    exit 1
}

Write-Host "=== Launching WinDbg KDNET (port $portInput) ===" -ForegroundColor Green
Write-Host "After WinDbg connects, type this command manually:" -ForegroundColor Yellow
Write-Host "   ```$`$< `$wdsPath" -ForegroundColor Cyan

# Build the command that will run after WinDbg connects
`$autoCmd = @"
.echo [AUTO] Loading WinDbg Workspace...
.wdf ``"`$workspacePath``"
.echo [READY] Workspace loaded successfully.
.echo [INFO] To load Debug script type: ```$`$< `$wdsPath
`"@

& `$winDbgPath ``
    -k "net:port=$portInput,key=$keyInput" ``
    -y "srv*C:\Symbols*https://msdl.microsoft.com/download/symbols;`$symbolPath" ``
    -c `$autoCmd
"@

# Write without BOM
[System.IO.File]::WriteAllText($kdnetScriptPath, $kdnetContent, $utf8NoBom)
Write-Host "[OK] KDNet script written to: $kdnetScriptPath" -ForegroundColor Green

# Remove the old fixed-name script if it still exists
$oldScript = Join-Path $ScriptRoot "Start-KDNet-WinDbg_v2.ps1"
if (Test-Path $oldScript) {
    Remove-Item $oldScript -Force
    Write-Host "[OK] Removed obsolete Start-KDNet-WinDbg_v2.ps1" -ForegroundColor Green
}

# ------------------------------------------------
# Final summary
# ------------------------------------------------
Write-Host ""
Write-Host "================================================" -ForegroundColor Green
Write-Host "  ALL ARTIFACTS GENERATED" -ForegroundColor Green
Write-Host "================================================" -ForegroundColor Green
Write-Host "  debug.wds            : $wdsPath"
Write-Host "  WinDbgWorkspace.xml  : $workspacePath"
Write-Host "  KDNet launcher       : $kdnetScriptPath"
Write-Host ""
Write-Host "Next steps:" -ForegroundColor Cyan
Write-Host "  1. Install the driver on the target (install_procsentinel.ps1)"
Write-Host "  2. On the host run:  .\$kdnetScriptName"
Write-Host "  3. After KDNET connects type:  `$`$< $wdsPath"
Write-Host ""
