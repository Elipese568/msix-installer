<#
.SYNOPSIS
    End-to-end suite for the native Msixpkg distribution setup
    (artifacts\setup\MsixpkgInstaller-*-setup.exe).

.DESCRIPTION
    This is the "installer of the installer": it puts msixpkg-installer.exe (wizard),
    msixpkg-native.exe (CLI) and Uninstall.exe on the machine and wires up shortcuts,
    the .msixpkg association, PATH and the Apps & features entry.

    The suite runs the same path a user would hit:
      1. silent per-user install into a workspace directory (/D= must be honoured)
      2. payload integrity: installed files are byte-identical to the build output
      3. the setup log records the run
      4. installing again over an existing install (idempotency)
      5. /NOASSOC /NOPATH /NODESKTOP are accepted
      6. negative: /ALLUSERS without elevation -> exit 5, nothing written
      7. negative: an impossible destination -> exit 4
      8. registry effects (record, ARP, association, PATH) when HKCU is writable
      9. uninstall through the real ARP path and through the plain uninstaller
     10. the install records under MSIXPKG_DATA_ROOT are never deleted
     11. the portable zip is intact
     12. the setup runs with an unusable %TEMP% (the old NSIS failure mode)

    This file is deliberately ASCII-only (Windows PowerShell 5.1 reads .ps1 as ANSI
    unless there is a UTF-8 BOM). Chinese documentation lives in installer/setup/README.md.

    Exit codes: 0 all checks passed, 1 failures, 2 environment not ready.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tests\e2e\setup-installer-e2e.ps1

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tests\e2e\setup-installer-e2e.ps1 -KeepWork
#>
[CmdletBinding()]
param(
    [string]$SetupPath,
    [string]$WorkDirectory,
    [string]$PayloadDir,
    [switch]$KeepWork
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Continue'

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path

if (-not $SetupPath) {
    $candidates = Get-ChildItem -Path (Join-Path $repoRoot 'artifacts\setup') -Filter 'MsixpkgInstaller-*-setup.exe' -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending
    if (@($candidates).Count -eq 0) {
        Write-Host 'no setup exe found under artifacts\setup' -ForegroundColor Yellow
        Write-Host 'build it first: powershell -ExecutionPolicy Bypass -File scripts\build-setup.ps1'
        exit 2
    }
    $SetupPath = @($candidates)[0].FullName
}
$setup = (Resolve-Path $SetupPath).Path
if (-not (Test-Path $setup)) {
    Write-Host "not found: $setup" -ForegroundColor Yellow
    exit 2
}

# The product version has a single source of truth: res/msixpkg.rc FILEVERSION.
# Read it instead of hardcoding, so a version bump cannot silently rot this suite.
$rcPath = Join-Path $repoRoot 'src\MsixpkgInstaller.Native\res\msixpkg.rc'
if (-not (Test-Path $rcPath)) {
    Write-Host "not found: $rcPath" -ForegroundColor Yellow
    exit 2
}
$rcText = [IO.File]::ReadAllText($rcPath)
if ($rcText -notmatch 'FILEVERSION\s+(\d+)\s*,\s*(\d+)\s*,\s*(\d+)') {
    Write-Host "no FILEVERSION x, y, z in $rcPath" -ForegroundColor Yellow
    exit 2
}
$expectedVersion = "$($Matches[1]).$($Matches[2]).$($Matches[3])"

$payloadDir = if ($PayloadDir) { $PayloadDir } else { Join-Path $repoRoot 'src\MsixpkgInstaller.Native\build\x64\Release' }
$sourceGui = Join-Path $payloadDir 'msixpkg-installer.exe'
$sourceCli = Join-Path $payloadDir 'msixpkg-native.exe'
$sourceUninstaller = Join-Path $payloadDir 'msixpkg-uninstall.exe'
foreach ($file in @($sourceGui, $sourceCli, $sourceUninstaller)) {
    if (-not (Test-Path $file)) {
        Write-Host "not found: $file (build the native project first)" -ForegroundColor Yellow
        exit 2
    }
}

$work = if ($WorkDirectory) { $WorkDirectory } else { Join-Path $repoRoot 'artifacts\e2e-setup' }
if (Test-Path $work) { Remove-Item $work -Recurse -Force }
$logDir = Join-Path $work 'logs'
$appDir = Join-Path $work 'app'
$appDir2 = Join-Path $work 'app-second'
$appDirTemp = Join-Path $work 'app-temp'
$blockedParent = Join-Path $work 'blocked-parent'
$dataRoot = Join-Path $work 'data'
foreach ($dir in @($work, $logDir, $dataRoot)) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }

# Logs go to the workspace so the suite can read them regardless of where
# %LOCALAPPDATA% points (and so a failed install still has evidence).
$env:MSIXPKG_SETUP_LOG_DIR = $logDir
$env:MSIXPKG_DATA_ROOT = $dataRoot
$logPath = Join-Path $logDir 'setup.log'

$script:checks = 0
$script:failures = 0
$script:skips = 0

function Check {
    param([bool]$Condition, [string]$Message)
    $script:checks++
    if ($Condition) {
        Write-Host ('  PASS  ' + $Message)
    } else {
        $script:failures++
        Write-Host ('  FAIL  ' + $Message) -ForegroundColor Red
    }
}

function Skip {
    param([string]$Message)
    $script:skips++
    Write-Host ('  SKIP  ' + $Message) -ForegroundColor DarkYellow
}

function Check-Contains {
    param([string]$Text, [string]$Pattern, [string]$Message)
    Check ($Text -match $Pattern) ($Message + ' (expected /' + $Pattern + '/)')
}

function Get-Log {
    if (-not (Test-Path $logPath)) { return '' }
    return (Get-Content -Path $logPath -Encoding Unicode -Raw -ErrorAction SilentlyContinue)
}

function Clear-Logs {
    if (Test-Path $logPath) { Remove-Item $logPath -Force }
}

function Invoke-Setup {
    param([string[]]$Arguments)
    Clear-Logs
    $process = Start-Process -FilePath $setup -ArgumentList $Arguments -PassThru -Wait
    return [pscustomobject]@{ ExitCode = $process.ExitCode; Log = (Get-Log) }
}

function Invoke-Uninstall {
    param([string]$Uninstaller, [string[]]$Arguments)
    Clear-Logs
    $process = Start-Process -FilePath $Uninstaller -ArgumentList $Arguments -PassThru -Wait
    return [pscustomobject]@{ ExitCode = $process.ExitCode; Log = (Get-Log) }
}

function Invoke-Native {
    param([string]$FilePath, [string[]]$Arguments)
    $output = & $FilePath @Arguments 2>&1 | Out-String
    return [pscustomobject]@{ ExitCode = $LASTEXITCODE; Output = $output }
}

function Get-FileHashSafe {
    param([string]$Path)
    if (-not (Test-Path $Path)) { return '' }
    return (Get-FileHash -Algorithm SHA256 -Path $Path).Hash
}

function Wait-ForPathGone {
    param([string]$Path, [int]$TimeoutSeconds = 30)
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    while ((Test-Path $Path) -and ((Get-Date) -lt $deadline)) { Start-Sleep -Milliseconds 250 }
    return (-not (Test-Path $Path))
}

function Get-RegKey {
    param([string]$Path)
    if (-not (Test-Path -LiteralPath $Path)) { return $null }
    return (Get-Item -LiteralPath $Path -ErrorAction SilentlyContinue)
}

function Get-RegValue {
    param([string]$Path, [string]$Name)
    $key = Get-RegKey $Path
    if ($null -eq $key) { return $null }
    return $key.GetValue($Name, $null)
}

function Get-RegDefault {
    param([string]$Path)
    return (Get-RegValue $Path '')
}

function Get-RegKind {
    param([string]$Path, [string]$Name)
    $key = Get-RegKey $Path
    if ($null -eq $key) { return $null }
    try { return $key.GetValueKind($Name).ToString() } catch { return $null }
}

$cfgKey = 'HKCU:\Software\MsixpkgInstaller'
$arpKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\MsixpkgInstaller'
$assocKey = 'HKCU:\Software\Classes\.msixpkg'
$progIdCommandKey = 'HKCU:\Software\Classes\Msixpkg.Package\shell\open\command'
$registeredAppsKey = 'HKCU:\Software\RegisteredApplications'
$userEnvKey = 'HKCU:\Environment'

$originalPath = Get-RegValue $userEnvKey 'Path'
$originalPathExisted = ($null -ne $originalPath)
if ($null -eq $originalPath) { $originalPath = '' }
$originalPathKind = Get-RegKind $userEnvKey 'Path'
if ($null -eq $originalPathKind) { $originalPathKind = 'ExpandString' }

# ---------------------------------------------------------------------------
# 1) silent per-user install
# ---------------------------------------------------------------------------
Write-Host ''
Write-Host 'install (silent, current user, /D into the workspace)'

$result = Invoke-Setup @('/S', '/CURRENTUSER', '/NODESKTOP', "/D=$appDir")
Check ($result.ExitCode -eq 0) ('setup exit code 0 (got ' + $result.ExitCode + ')')
Check (Test-Path (Join-Path $appDir 'msixpkg-installer.exe')) 'msixpkg-installer.exe is installed'
Check (Test-Path (Join-Path $appDir 'msixpkg-native.exe')) 'msixpkg-native.exe is installed'
Check (Test-Path (Join-Path $appDir 'Uninstall.exe')) 'Uninstall.exe is installed'

Check-Contains $result.Log ('dir=' + [regex]::Escape($appDir)) 'log records the requested install directory'
Check-Contains $result.Log 'setup start.*mode=currentuser' 'log records the install mode'

$registryEffective = ($null -ne (Get-RegValue $cfgKey 'InstallDir'))
Write-Host ("  info  install record written by the setup: " + $registryEffective)

# ---------------------------------------------------------------------------
# 2) payload integrity
# ---------------------------------------------------------------------------
Write-Host ''
Write-Host 'payload integrity'

Check ((Get-FileHashSafe (Join-Path $appDir 'msixpkg-installer.exe')) -eq (Get-FileHashSafe $sourceGui)) 'installed wizard is byte-identical to the build output'
Check ((Get-FileHashSafe (Join-Path $appDir 'msixpkg-native.exe')) -eq (Get-FileHashSafe $sourceCli)) 'installed CLI is byte-identical to the build output'
Check ((Get-FileHashSafe (Join-Path $appDir 'Uninstall.exe')) -eq (Get-FileHashSafe $sourceUninstaller)) 'installed uninstaller is byte-identical to the build output'

$cliRun = Invoke-Native (Join-Path $appDir 'msixpkg-native.exe') @('help')
Check ($cliRun.ExitCode -eq 0) ('installed CLI runs (exit code ' + $cliRun.ExitCode + ')')
Check-Contains $cliRun.Output 'msixpkg-native' 'installed CLI prints its usage'

$versionInfo = (Get-Item $setup).VersionInfo
Check ($versionInfo.ProductVersion -like "$expectedVersion*") ('setup exe carries the product version (' + $versionInfo.ProductVersion + ', expected ' + $expectedVersion + ')')

$verify = Invoke-Setup @('--verify-payload', '/S')
Check ($verify.ExitCode -eq 0) ('setup verifies its own embedded payload (exit ' + $verify.ExitCode + ')')

# ---------------------------------------------------------------------------
# 3) idempotency
# ---------------------------------------------------------------------------
Write-Host ''
Write-Host 'install again (idempotency)'

$again = Invoke-Setup @('/S', '/CURRENTUSER', '/NODESKTOP', "/D=$appDir")
Check ($again.ExitCode -eq 0) ('second install exits 0 (got ' + $again.ExitCode + ')')
Check ((Get-FileHashSafe (Join-Path $appDir 'msixpkg-native.exe')) -eq (Get-FileHashSafe $sourceCli)) 'payload still intact after reinstall'

# ---------------------------------------------------------------------------
# 4) component switches
# ---------------------------------------------------------------------------
Write-Host ''
Write-Host 'component switches (/NOASSOC /NOPATH /NODESKTOP)'

$noComponents = Invoke-Setup @('/S', '/CURRENTUSER', '/NODESKTOP', '/NOASSOC', '/NOPATH', "/D=$appDir")
Check ($noComponents.ExitCode -eq 0) ('switches accepted (exit code ' + $noComponents.ExitCode + ')')

# ---------------------------------------------------------------------------
# 5) negative: /ALLUSERS without elevation
# ---------------------------------------------------------------------------
Write-Host ''
Write-Host 'negative: /ALLUSERS without elevation'

$elevated = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if ($elevated) {
    Skip '/ALLUSERS negative case (this shell is already elevated)'
} else {
    $env:MSIXPKG_SETUP_NO_ELEVATE = '1'
    try {
        $allUsers = Invoke-Setup @('/S', '/ALLUSERS', "/D=$appDir2")
    }
    finally {
        Remove-Item Env:\MSIXPKG_SETUP_NO_ELEVATE -ErrorAction SilentlyContinue
    }
    Check ($allUsers.ExitCode -eq 5) ('/ALLUSERS without elevation exits 5 (got ' + $allUsers.ExitCode + ')')
    Check (-not (Test-Path $appDir2)) 'nothing was installed by the refused /ALLUSERS run'
}

# ---------------------------------------------------------------------------
# 6) negative: destination that cannot be created
# ---------------------------------------------------------------------------
Write-Host ''
Write-Host 'negative: destination that cannot be created'

if ($elevated) {
    Skip 'impossible destination case (this shell is already elevated)'
} else {
    New-Item -ItemType Directory -Force -Path $blockedParent | Out-Null
    $blockedTarget = Join-Path $blockedParent 'NUL\app'
    $blocked = Invoke-Setup @('/S', '/CURRENTUSER', '/NODESKTOP', "/D=$blockedTarget")
    Check ($blocked.ExitCode -eq 4) ('install into an impossible directory exits 4 (got ' + $blocked.ExitCode + ')')
    Check-Contains $blocked.Log 'FAILED to write the program files' 'log says the program files could not be written'
}

# ---------------------------------------------------------------------------
# 7) registry effects
# ---------------------------------------------------------------------------
Write-Host ''
Write-Host 'registry effects'

if (-not $registryEffective) {
    Skip 'install record / ARP entry / association / PATH assertions (this environment denies HKCU writes to child processes)'
    Check-Contains $result.Log 'WARNING - install records could not be written' 'setup reported the failed install-record write'
    Check-Contains $result.Log 'WARNING - the Apps & features uninstall entry could not be written' 'setup reported the failed uninstall-entry write'
    Check-Contains $result.Log 'assoc: FAILED to register' 'setup reported the failed association write'
    Check-Contains $result.Log 'path:' 'setup reported the PATH step in its log'
    Check ((Get-RegValue $userEnvKey 'Path') -eq $originalPath) 'PATH was left untouched when the write failed'
} else {
    Check ((Get-RegValue $cfgKey 'InstallDir') -eq $appDir) 'install record points at the requested directory'
    Check ((Get-RegValue $cfgKey 'InstallMode') -eq 'currentuser') 'install record says currentuser'
    Check ((Get-RegValue $cfgKey 'Version') -eq $expectedVersion) ('install record carries the version (' + (Get-RegValue $cfgKey 'Version') + ')')

    $displayName = Get-RegValue $arpKey 'DisplayName'
    Check ($null -ne $displayName) 'Apps & features entry exists'
    Check ("$displayName" -match 'Msixpkg') ('ARP DisplayName mentions Msixpkg (' + $displayName + ')')
    Check ((Get-RegValue $arpKey 'UninstallString') -match 'Uninstall\.exe') 'ARP UninstallString points at Uninstall.exe'
    Check ((Get-RegValue $arpKey 'QuietUninstallString') -match '/S') 'ARP QuietUninstallString is silent'
    Check ((Get-RegValue $arpKey 'NoModify') -eq 1) 'ARP entry is marked as not modifiable'

    Check ((Get-RegDefault $assocKey) -eq 'Msixpkg.Package') '.msixpkg is associated with the Msixpkg.Package ProgID'
    Check ((Get-RegDefault $progIdCommandKey) -like ('*' + $appDir + '*')) 'association launches the installed wizard'
    Check ($null -ne (Get-RegValue $registeredAppsKey 'Msixpkg Installer')) 'capabilities are registered for the Default apps UI'

    $pathValue = Get-RegValue $userEnvKey 'Path'
    Check ($pathValue -like ('*' + $appDir + '*')) 'install directory was appended to the user PATH'
    Check ($pathValue.StartsWith($originalPath)) 'the pre-existing PATH entries were preserved'
    Check ((Get-RegKind $userEnvKey 'Path') -eq $originalPathKind) ('PATH value type was preserved (' + (Get-RegKind $userEnvKey 'Path') + ')')
}

# ---------------------------------------------------------------------------
# 8) install records are kept
# ---------------------------------------------------------------------------
Write-Host ''
Write-Host 'install records under MSIXPKG_DATA_ROOT'

$stateDir = Join-Path $dataRoot 'state'
New-Item -ItemType Directory -Force -Path $stateDir | Out-Null
$fakeRecord = Join-Path $stateDir 'e2e-fake.json'
Set-Content -Path $fakeRecord -Value '{"installId":"e2e-fake"}' -Encoding ASCII

# ---------------------------------------------------------------------------
# 9) uninstall through the real ARP path
# ---------------------------------------------------------------------------
Write-Host ''
Write-Host 'uninstall (ARP path, plain /S)'

$uninstaller = Join-Path $appDir 'Uninstall.exe'
$uninstall = Invoke-Uninstall $uninstaller @('/S')
$gone = Wait-ForPathGone $appDir 30
Check $gone ('the install directory is removed by the uninstaller (exit ' + $uninstall.ExitCode + ')')
Check ($uninstall.ExitCode -eq 0) ('uninstaller exits 0 (got ' + $uninstall.ExitCode + ')')
Check-Contains $uninstall.Log 'uninstall done' 'uninstaller logged completion'
Check-Contains $uninstall.Log 'state: records kept' 'uninstaller noticed the installation records'
Check (Test-Path $fakeRecord) 'installation records were NOT deleted by the uninstaller'

if ($registryEffective) {
    Check ($null -eq (Get-RegValue $cfgKey 'InstallDir')) 'install record was removed'
    Check ($null -eq (Get-RegValue $arpKey 'DisplayName')) 'ARP entry was removed'
    $assocDefault = Get-RegDefault $assocKey
    Check ($null -eq $assocDefault) ('.msixpkg association was removed (got ' + $assocDefault + ')')
    $pathAfter = Get-RegValue $userEnvKey 'Path'
    if ($null -eq $pathAfter) { $pathAfter = '' }
    if ($originalPathExisted) {
        Check ($pathAfter -eq $originalPath) 'PATH was restored exactly (byte for byte)'
        Check ((Get-RegKind $userEnvKey 'Path') -eq $originalPathKind) 'PATH value type survived the round trip'
    } else {
        Check ($null -eq (Get-RegValue $userEnvKey 'Path')) 'PATH value created by the setup was removed again'
    }
} else {
    Skip 'uninstall registry cleanup assertions (this environment denies HKCU writes)'
}

# ---------------------------------------------------------------------------
# 10) second round: the compatibility _?= argument
# ---------------------------------------------------------------------------
Write-Host ''
Write-Host 'uninstall (_?= compatibility argument)'

$second = Invoke-Setup @('/S', '/CURRENTUSER', '/NODESKTOP', "/D=$appDir2")
Check ($second.ExitCode -eq 0) ('second install exits 0 (got ' + $second.ExitCode + ')')
if (Test-Path (Join-Path $appDir2 'Uninstall.exe')) {
    $selfDelete = Invoke-Uninstall (Join-Path $appDir2 'Uninstall.exe') @('/S', "_?=$appDir2")
    Check ($selfDelete.ExitCode -eq 0) ('uninstaller with _?= exits 0 (got ' + $selfDelete.ExitCode + ')')
    Check (Wait-ForPathGone $appDir2 30) 'the payload and directory are gone with _?='
} else {
    Check $false 'second install produced an uninstaller'
}

# ---------------------------------------------------------------------------
# 11) portable package
# ---------------------------------------------------------------------------
Write-Host ''
Write-Host 'portable package'

$portableZips = Get-ChildItem -Path (Split-Path -Parent $setup) -Filter 'MsixpkgInstaller-*-portable.zip' -ErrorAction SilentlyContinue |
    Sort-Object LastWriteTime -Descending
if (@($portableZips).Count -eq 0) {
    Skip 'portable package checks (no *-portable.zip next to the setup exe)'
} else {
    $unpacked = Join-Path $work 'portable'
    Expand-Archive -Path @($portableZips)[0].FullName -DestinationPath $unpacked -Force
    Check ((Get-FileHashSafe (Join-Path $unpacked 'msixpkg-installer.exe')) -eq (Get-FileHashSafe $sourceGui)) 'portable wizard is byte-identical to the build output'
    Check ((Get-FileHashSafe (Join-Path $unpacked 'msixpkg-native.exe')) -eq (Get-FileHashSafe $sourceCli)) 'portable CLI is byte-identical to the build output'
    Check (Test-Path (Join-Path $unpacked 'README-portable.txt')) 'the portable package ships its own README'
    $portableRun = Invoke-Native (Join-Path $unpacked 'msixpkg-native.exe') @('help')
    Check ($portableRun.ExitCode -eq 0) ('the unpacked CLI runs (exit code ' + $portableRun.ExitCode + ')')
}

# ---------------------------------------------------------------------------
# 12) an unusable %TEMP% must not stop the setup
# ---------------------------------------------------------------------------
Write-Host ''
Write-Host 'unusable %TEMP% (the old NSIS failure mode)'

$savedTemp = $env:TEMP
$savedTmp = $env:TMP
$env:TEMP = 'X:\msixpkg-temp-does-not-exist'
$env:TMP = $env:TEMP
try {
    $noTemp = Invoke-Setup @('/S', '/CURRENTUSER', '/NODESKTOP', "/D=$appDirTemp")
}
finally {
    $env:TEMP = $savedTemp
    $env:TMP = $savedTmp
}
Check ($noTemp.ExitCode -eq 0) ('setup succeeds with an unusable %TEMP% (exit ' + $noTemp.ExitCode + ')')
Check (Test-Path (Join-Path $appDirTemp 'msixpkg-native.exe')) 'the payload is installed even though %TEMP% is unusable'

# ---------------------------------------------------------------------------
# 13) GUI path: the Wren options page really starts the engine
# ---------------------------------------------------------------------------
Write-Host ''
Write-Host 'GUI install / uninstall (--ui-page + /AUTO)'

$guiApp = Join-Path $work 'gui-app'
$guiReport = Join-Path $work 'gui-install.txt'
$guiInstall = Start-Process -FilePath $setup -ArgumentList @(
    '--ui-page', 'options', '--auto', '/CURRENTUSER', '/NODESKTOP',
    "/D=$guiApp", '--ui-report', $guiReport, '--exit-after', '10000') -PassThru -Wait
Check ($guiInstall.ExitCode -eq 0) ('GUI install exits 0 (got ' + $guiInstall.ExitCode + ')')
Check (Test-Path (Join-Path $guiApp 'msixpkg-native.exe')) 'GUI install writes the payload'
$guiReportText = if (Test-Path $guiReport) { [IO.File]::ReadAllText($guiReport, [Text.Encoding]::UTF8) } else { '' }
Check-Contains $guiReportText 'page=finish' 'GUI install reaches the finish page'

$guiUninstallReport = Join-Path $work 'gui-uninstall.txt'
$guiUninstaller = Join-Path $guiApp 'Uninstall.exe'
$guiUninstall = Start-Process -FilePath $guiUninstaller -ArgumentList @(
    '--ui-page', 'confirm', '--auto', '--ui-report', $guiUninstallReport, '--exit-after', '10000') -PassThru -Wait
Check ($guiUninstall.ExitCode -eq 0) ('GUI uninstall exits 0 (got ' + $guiUninstall.ExitCode + ')')
Check (Wait-ForPathGone $guiApp 30) 'GUI uninstall removes the install directory'
$guiUninstallText = if (Test-Path $guiUninstallReport) { [IO.File]::ReadAllText($guiUninstallReport, [Text.Encoding]::UTF8) } else { '' }
Check-Contains $guiUninstallText 'page=finish' 'GUI uninstall reaches the finish page'

# ---------------------------------------------------------------------------
# summary
# ---------------------------------------------------------------------------
if (-not $KeepWork) {
    Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue
}

Write-Host ''
Write-Host ('checks ' + $script:checks + ', failures ' + $script:failures + ', skipped ' + $script:skips)
if ($script:failures -gt 0) { exit 1 }
exit 0
