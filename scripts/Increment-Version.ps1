# ================================================
# Increment-Version.ps1
#
# Purpose:
#   Clone this workspace into a sibling ProcSentinel_vN folder so a stuck
#   or still-loaded .sys does not block the next lab build.
#
# How it works:
#   1. Reads the current folder name. If it ends in _vN, the clone is _v(N+1).
#      If there is no suffix, the clone is CurrentName_v1.
#   2. Copies the whole tree into the parent directory.
#   3. When the source already has a version tag, renames files that contain
#      that tag and replaces the tag (and the altitude literal) inside
#      .c .cpp .h .ps1 .bat .inf .md .vcxproj .sln.
#
# What it isolates:
#   - Binary and script names that already contain the old tag
#   - The altitude number in install scripts (only useful if a minifilter
#     altitude is registered; this WDM driver does not use one)
#
# What it does not isolate:
#   - First run from an unversioned folder: no old tag, so procsentinel_driver /
#     Build-Driver.ps1 are not renamed.
#   - \Device\ProcSentinelDrv and \DosDevices\ProcSentinelDrv are not versioned.
#     Starting a second copy fails with STATUS_OBJECT_NAME_COLLISION.
#     Run only one instance at a time.
#   - Text replace is unanchored. Do not treat it as an identifier-only rewrite.
#
# Usage (from the scripts directory):
#   .\Increment-Version.ps1
# ================================================

[CmdletBinding()]
param()

$ErrorActionPreference = "Stop"

$ScriptRoot   = $PSScriptRoot
$ProjectRoot  = Split-Path $ScriptRoot -Parent          # e.g. ...\ProcSentinel
$ParentDir    = Split-Path $ProjectRoot -Parent         # e.g. ...\ProcSentinel
$CurrentName  = Split-Path $ProjectRoot -Leaf           # e.g. ProcSentinel

Write-Host ""
Write-Host "================================================" -ForegroundColor Cyan
Write-Host "  Increment-Version" -ForegroundColor Cyan
Write-Host "================================================" -ForegroundColor Cyan
Write-Host "Current project : $CurrentName"
Write-Host "Parent folder   : $ParentDir"
Write-Host ""

# ---- Parse version suffix ----
$baseName = $CurrentName
$nextVer  = 1

if ($CurrentName -match '^(.*)_v(\d+)$') {
    $baseName = $Matches[1]
    $nextVer  = [int]$Matches[2] + 1
    Write-Host "[*] Detected version suffix _v$($Matches[2]) → next _v$nextVer"
} else {
    Write-Host "[*] No _v## suffix found → will create ${CurrentName}_v1"
    $baseName = $CurrentName
    $nextVer  = 1
}

$NewName = "${baseName}_v$nextVer"
$NewRoot = Join-Path $ParentDir $NewName

Write-Host "[*] Target folder : $NewRoot"
Write-Host ""

$answer = Read-Host "Create new folder '$NewName' under parent directory? [Y/n]"
if ($answer -match '^[Nn]') {
    Write-Host "[*] Cancelled."
    exit 0
}

if (Test-Path $NewRoot) {
    Write-Error "Target already exists: $NewRoot"
    exit 1
}

# ---- Current altitude (best-effort from install script) ----
$currentAltitude = 361001
$installPs1 = Get-ChildItem -Path $ScriptRoot -Filter "install_procsentinel_v*.ps1" -ErrorAction SilentlyContinue |
              Select-Object -First 1
if ($installPs1) {
    $m = Select-String -Path $installPs1.FullName -Pattern 'altitude\s*=\s*"?(\d+)"?' -AllMatches
    if ($m -and $m.Matches.Count -gt 0) {
        $currentAltitude = [int]$m.Matches[0].Groups[1].Value
    }
}
$newAltitude = $currentAltitude + 1
Write-Host "[*] Altitude $currentAltitude → $newAltitude"
Write-Host ""

# ---- Copy tree ----
Write-Host "[*] Copying project tree..." -ForegroundColor Yellow
Copy-Item -Path $ProjectRoot -Destination $NewRoot -Recurse -Force

# ---- Rewrite version / altitude inside the new tree ----
Write-Host "[*] Updating version strings and altitude in new tree..." -ForegroundColor Yellow

$oldVerTag = if ($CurrentName -match '_v(\d+)$') { "v$($Matches[1])" } else { $null }
$newVerTag = "v$nextVer"

$oldSys = if ($oldVerTag) { "procsentinel_driver_$oldVerTag" } else { "procsentinel_driver" }
$newSys = "procsentinel_driver_$newVerTag"
$oldApp = if ($oldVerTag) { "procsentinel_app_$oldVerTag" } else { "procsentinel_app" }
$newApp = "procsentinel_app_$newVerTag"
$oldInstall = if ($oldVerTag) { "install_procsentinel_$oldVerTag" } else { "install_procsentinel" }
$newInstall = "install_procsentinel_$newVerTag"
$oldUninstall = if ($oldVerTag) { "uninstall_procsentinel_$oldVerTag" } else { "uninstall_procsentinel" }
$newUninstall = "uninstall_procsentinel_$newVerTag"
$oldBuild = if ($oldVerTag) { "Build-Driver_$oldVerTag" } else { "Build-Driver" }
$newBuild = "Build-Driver_$newVerTag"
$oldBuildUm = if ($oldVerTag) { "Build-UserMode_$oldVerTag" } else { "Build-UserMode" }
$newBuildUm = "Build-UserMode_$newVerTag"

# Rename files that contain old version in the name
Get-ChildItem -Path $NewRoot -Recurse -File | ForEach-Object {
    $newFileName = $_.Name
    if ($oldVerTag) {
        $newFileName = $newFileName -replace [regex]::Escape($oldVerTag), $newVerTag
        $newFileName = $newFileName -replace [regex]::Escape($CurrentName), $NewName
    }
    if ($newFileName -ne $_.Name) {
        Rename-Item -Path $_.FullName -NewName $newFileName -Force
    }
}

# Text replace inside files
$textExt = @('.c','.cpp','.h','.ps1','.bat','.inf','.MD','.md','.wds','.vcxproj','.sln','.filters')
Get-ChildItem -Path $NewRoot -Recurse -File | Where-Object { $textExt -contains $_.Extension } | ForEach-Object {
    $content = Get-Content -Path $_.FullName -Raw -ErrorAction SilentlyContinue
    if ($null -eq $content) { return }
    $orig = $content
    if ($oldVerTag) {
        $content = $content -replace [regex]::Escape($CurrentName), $NewName
        $content = $content -replace [regex]::Escape($oldVerTag), $newVerTag
        $content = $content -replace [regex]::Escape("$currentAltitude"), "$newAltitude"
    } else {
        $content = $content -replace [regex]::Escape($CurrentName), $NewName
        $content = $content -replace [regex]::Escape("$currentAltitude"), "$newAltitude"
    }
    if ($content -ne $orig) {
        Set-Content -Path $_.FullName -Value $content -NoNewline
    }
}

Write-Host "[+] Created: $NewRoot" -ForegroundColor Green
Write-Host "[+] Version : $newVerTag"
Write-Host "[+] Altitude: $newAltitude"
Write-Host ""

$cdAnswer = Read-Host "Change directory to new scripts folder now? [Y/n]"
$newScripts = Join-Path $NewRoot "scripts"
if (-not ($cdAnswer -match '^[Nn]')) {
    if (Test-Path $newScripts) {
        Set-Location $newScripts
        Write-Host "[+] Working directory is now:" -ForegroundColor Green
        Write-Host "    $newScripts"
        Write-Host ""
        Write-Host "Next steps:" -ForegroundColor Cyan
        Write-Host "  .\Build-Driver_$newVerTag.ps1"
        Write-Host "  .\Build-UserMode_$newVerTag.ps1"
        Write-Host "  .\install_procsentinel_$newVerTag.ps1"
    }
} else {
    Write-Host "[*] New scripts folder: $newScripts"
}

Write-Host ""
