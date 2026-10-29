<#
.SYNOPSIS
    End-to-end suite for the NSIS distribution package of the Msixpkg installer
    (installer/nsis/MsixpkgInstaller.nsi -> artifacts/setup/MsixpkgInstaller-*-setup.exe).

.DESCRIPTION
    This is the "installer of the installer": it puts msixpkg-installer.exe (wizard)
    and msixpkg-native.exe (CLI) on the machine and wires up shortcuts, the .msixpkg
    file association, PATH and the Apps & features uninstall entry.

    What this suite really runs, in the order a user would hit it:
      1. silent per-user install into a workspace directory (/D= must be honoured)
      2. payload integrity: the installed files are byte-identical to the build output,
         and the installed CLI answers `help`
      3. the setup log records the whole run (it is the only evidence a silent install
         leaves behind)
      4. installing again over an existing install (idempotency)
      5. negative: /ALLUSERS without elevation -> exit 5, nothing written
      6. negative: a destination that cannot be created -> exit 4. "Installed nothing
         but exited 0" is the exact bug this check exists for.
      7. uninstall through the real ARP path (plain /S) and through `_?=` (which waits
         for completion and returns the real exit code)
      8. registry effects (install record, uninstall entry, association, PATH entry)
         and their cleanup - when the environment lets the installer write them

    The suite adapts to the environment instead of pretending: this machine's security
    policy (360 with Defender real-time off) denies *unsigned* executables both temp
    files on C: and any registry write, so the installer can copy its files but cannot
    register anything here. Those assertions are reported as SKIP with the reason, and
    the suite instead asserts that the installer *reported* the failed writes in its
    log. Nothing is silently dropped. (Proven with a signed/unsigned probe: pwsh/cmd are
    allowed, a freshly compiled exe gets error 5 / Access denied.)

    NOTE: this file is deliberately ASCII-only (Windows PowerShell 5.1 reads .ps1 as
    ANSI unless there is a UTF-8 BOM). Chinese documentation lives in README.md.

    Exit codes: 0 all checks passed, 1 failures, 2 environment not ready.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tests\e2e\nsis-installer-e2e.ps1

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tests\e2e\nsis-installer-e2e.ps1 -KeepWork
#>
[CmdletBinding()]
param(
    # Explicit setup exe; when omitted the newest artifacts\setup\MsixpkgInstaller-*-setup.exe is used.
    [string]$SetupPath,

    # Working directory (defaults to artifacts\e2e-nsis inside the repo).
    [string]$WorkDirectory,

    # Directory holding the payload the package was built from (msixpkg-installer.exe /
    # msixpkg-native.exe). Omitted -> the local CMakePresets output; CI passes its own.
    [string]$PayloadDir,

    # Keep the working directory for inspection.
    [switch]$KeepWork
)

Set-StrictMode -Version Latest

# Continue, not Stop: native commands writing to stderr would otherwise become
# terminating NativeCommandError on PowerShell 5.1. Exit codes are checked explicitly.
$ErrorActionPreference = 'Continue'

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path

if (-not $SetupPath) {
    $candidates = Get-ChildItem -Path (Join-Path $repoRoot 'artifacts\setup') -Filter 'MsixpkgInstaller-*-setup.exe' -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending
    if (@($candidates).Count -eq 0) {
        Write-Host 'no setup exe found under artifacts\setup' -ForegroundColor Yellow
        Write-Host 'build it first: powershell -ExecutionPolicy Bypass -File scripts\build-nsis-installer.ps1'
        exit 2
    }
    $SetupPath = @($candidates)[0].FullName
}
$setup = (Resolve-Path $SetupPath).Path
if (-not (Test-Path $setup)) {
    Write-Host "not found: $setup" -ForegroundColor Yellow
    exit 2
}

$payloadDir = if ($PayloadDir) { $PayloadDir } else { Join-Path $repoRoot 'src\MsixpkgInstaller.Native\build\x64\Release' }
$sourceGui = Join-Path $payloadDir 'msixpkg-installer.exe'
$sourceCli = Join-Path $payloadDir 'msixpkg-native.exe'
foreach ($file in @($sourceGui, $sourceCli)) {
    if (-not (Test-Path $file)) {
        Write-Host "not found: $file (build the native project first)" -ForegroundColor Yellow
        exit 2
    }
}

$work = if ($WorkDirectory) { $WorkDirectory } else { Join-Path $repoRoot 'artifacts\e2e-nsis' }
if (Test-Path $work) { Remove-Item $work -Recurse -Force }
$workTemp = Join-Path $work 'temp'
$appDir = Join-Path $work 'app'
$appDir2 = Join-Path $work 'app-second'
$blockedParent = Join-Path $work 'blocked-parent'
$dataRoot = Join-Path $work 'data'
foreach ($dir in @($work, $workTemp, $dataRoot)) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }

# The NSIS stub and the uninstaller both extract themselves to %TEMP% before the script
# runs. Here the security policy denies unsigned executables temp files on C:, which makes
# any NSIS installer fail with "Error writing temporary file" - so TEMP/TMP must point
# into the workspace (on D:), which is exactly what makes this suite runnable.
$env:TEMP = $workTemp
$env:TMP = $workTemp
# Give the tool its own state root, so "installation records are kept" is testable
# without ever touching the real %ProgramData%.
$env:MSIXPKG_DATA_ROOT = $dataRoot

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

# Registry access through RegistryKey.GetValue: Get-ItemProperty throws under
# Set-StrictMode when a value is missing, and missing values are exactly what the
# cleanup assertions look for.
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

function Get-NewestLog {
    # The setup writes its log to %LOCALAPPDATA%\MsixpkgInstaller when that is writable
    # for the (child) process, otherwise to %TEMP%\MsixpkgInstaller. Read whichever exists.
    $paths = @(
        (Join-Path $env:LOCALAPPDATA 'MsixpkgInstaller\setup.log'),
        (Join-Path $workTemp 'MsixpkgInstaller\setup.log')
    ) | Where-Object { Test-Path $_ }
    if (@($paths).Count -eq 0) { return '' }
    $newest = @($paths | Sort-Object { (Get-Item $_).LastWriteTime } -Descending)[0]
    # The log is UTF-16LE on purpose (Chinese text must survive any ANSI code page).
    return (Get-Content -Path $newest -Encoding Unicode -Raw -ErrorAction SilentlyContinue)
}

function Clear-Logs {
    foreach ($path in @(
            (Join-Path $env:LOCALAPPDATA 'MsixpkgInstaller\setup.log'),
            (Join-Path $workTemp 'MsixpkgInstaller\setup.log'))) {
        if (Test-Path $path) { Remove-Item $path -Force }
    }
}

function Invoke-Setup {
    param([string[]]$Arguments)
    Clear-Logs
    $process = Start-Process -FilePath $setup -ArgumentList $Arguments -PassThru -Wait
    return [pscustomobject]@{ ExitCode = $process.ExitCode; Log = (Get-NewestLog) }
}

function Get-FileHashSafe {
    param([string]$Path)
    if (-not (Test-Path $Path)) { return '' }
    return (Get-FileHash -Algorithm SHA256 -Path $Path).Hash
}

function Invoke-Native {
    param([string]$FilePath, [string[]]$Arguments)
    $output = & $FilePath @Arguments 2>&1 | Out-String
    return [pscustomobject]@{ ExitCode = $LASTEXITCODE; Output = $output }
}

function Wait-ForFileGone {
    param([string]$Path, [int]$TimeoutSeconds = 30)
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    while ((Test-Path $Path) -and ((Get-Date) -lt $deadline)) { Start-Sleep -Milliseconds 250 }
    return (-not (Test-Path $Path))
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
# A machine without a user PATH value is normal (fresh CI images): the installer creates
# one, and a newly created PATH is REG_EXPAND_SZ by design.
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
Check (Test-Path (Join-Path $appDir 'Uninstall.exe')) 'Uninstall.exe is generated'

# /D= must win over the per-user default: regression test for the first version, where
# .onInit overwrote $INSTDIR and a "successful" silent install went somewhere else.
Check-Contains $result.Log ('dir=' + [regex]::Escape($appDir)) 'log records the requested install directory (/D= is honoured)'
Check-Contains $result.Log 'setup start.*mode=currentuser' 'log records the install mode'

# Does this environment let the installer write HKCU at all? The shell being able to
# write it proves nothing (children may be confined), so ask the installer itself.
$registryEffective = ($null -ne (Get-RegValue $cfgKey 'InstallDir'))
Write-Host ("  info  install record written by the installer: " + $registryEffective)

# ---------------------------------------------------------------------------
# 2) payload integrity
# ---------------------------------------------------------------------------
Write-Host ''
Write-Host 'payload integrity'

Check ((Get-FileHashSafe (Join-Path $appDir 'msixpkg-installer.exe')) -eq (Get-FileHashSafe $sourceGui)) 'installed wizard is byte-identical to the build output'
Check ((Get-FileHashSafe (Join-Path $appDir 'msixpkg-native.exe')) -eq (Get-FileHashSafe $sourceCli)) 'installed CLI is byte-identical to the build output'

$cliRun = Invoke-Native (Join-Path $appDir 'msixpkg-native.exe') @('help')
Check ($cliRun.ExitCode -eq 0) ('installed CLI runs (exit code ' + $cliRun.ExitCode + ')')
Check-Contains $cliRun.Output 'msixpkg-native' 'installed CLI prints its usage'

$versionInfo = (Get-Item $setup).VersionInfo
Check ($versionInfo.ProductVersion -like '0.2.1*') ('setup exe carries the product version (' + $versionInfo.ProductVersion + ')')

# ---------------------------------------------------------------------------
# 3) idempotency: installing over an existing install
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
Write-Host 'component switches (/NOASSOC /NOPATH are accepted)'

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
    $allUsers = Invoke-Setup @('/S', '/ALLUSERS', "/D=$appDir2")
    Check ($allUsers.ExitCode -eq 5) ('/ALLUSERS without elevation exits 5 (got ' + $allUsers.ExitCode + ')')
    Check (-not (Test-Path $appDir2)) 'nothing was installed by the refused /ALLUSERS run'
}

# ---------------------------------------------------------------------------
# 6) negative: destination that cannot be created
# ---------------------------------------------------------------------------
Write-Host ''
Write-Host 'negative: destination that cannot be created'

# A file where a directory should be: SetOutPath/File fail no matter who runs the setup,
# so this case is meaningful on any machine. The installer must report failure.
New-Item -ItemType Directory -Force -Path $blockedParent | Out-Null
$blocker = Join-Path $blockedParent 'blocker.txt'
Set-Content -Path $blocker -Value 'x' -Encoding ASCII
$blockedTarget = Join-Path $blocker 'app'
$blocked = Invoke-Setup @('/S', '/CURRENTUSER', '/NODESKTOP', "/D=$blockedTarget")
Check ($blocked.ExitCode -eq 4) ('install into an impossible directory exits 4 (got ' + $blocked.ExitCode + ')')
Check-Contains $blocked.Log 'FAILED to write the program files' 'log says the program files could not be written'

# ---------------------------------------------------------------------------
# 7) registry effects
# ---------------------------------------------------------------------------
Write-Host ''
Write-Host 'registry effects'

if (-not $registryEffective) {
    Skip 'install record / ARP entry / association / PATH assertions (unsigned executables are denied registry writes here - exempt the setup or sign it to run these)'
    Check-Contains $result.Log 'WARNING - install records could not be written' 'installer reported the failed install-record write instead of pretending success'
    Check-Contains $result.Log 'WARNING - the Apps & features uninstall entry could not be written' 'installer reported the failed uninstall-entry write'
    Check-Contains $result.Log 'assoc: FAILED to register' 'installer reported the failed file-association write'
    Check-Contains $result.Log 'PATH' 'installer reported the PATH step in its log'
    Check ((Get-RegValue $userEnvKey 'Path') -eq $originalPath) 'PATH was left untouched when the write failed'
} else {
    Check ((Get-RegValue $cfgKey 'InstallDir') -eq $appDir) 'install record points at the requested directory'
    Check ((Get-RegValue $cfgKey 'InstallMode') -eq 'currentuser') 'install record says currentuser'
    Check ((Get-RegValue $cfgKey 'Version') -eq '0.2.1') ('install record carries the version (' + (Get-RegValue $cfgKey 'Version') + ')')

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
# 8) PATH logic unit test (NSIS level, no registry needed)
# ---------------------------------------------------------------------------
Write-Host ''
Write-Host 'PATH logic unit test (NSIS level)'

# The PATH bookkeeping is the one piece that can ruin a machine when it is wrong
# (it edits a value the user's shells depend on), and the registry can't be used to
# exercise it in this environment. So the logic lives in installer/nsis/msixpkg-path.nsh
# and tests/nsis/path-logic.nsi drives its macro directly over the tricky inputs.
$makensis = Join-Path $repoRoot 'artifacts\tools\nsis\makensis.exe'
if (-not (Test-Path $makensis)) {
    Skip 'PATH logic unit test (no NSIS toolchain under artifacts\tools\nsis - run scripts\build-nsis-installer.ps1 first)'
} else {
    $unitDir = Join-Path $work 'path-logic'
    New-Item -ItemType Directory -Force -Path $unitDir | Out-Null
    $harness = Join-Path $repoRoot 'tests\nsis\path-logic.nsi'
    # /NOCD keeps makensis from switching its working directory to the script's folder,
    # so the harness exe lands here instead of in the source tree.
    Push-Location $unitDir
    try {
        $compileOutput = & $makensis /V1 /NOCD /INPUTCHARSET UTF8 $harness 2>&1 | Out-String
        $compileExit = $LASTEXITCODE
    } finally {
        Pop-Location
    }
    Check ($compileExit -eq 0) ('the PATH logic harness compiles (exit code ' + $compileExit + ')')
    $unitExe = Join-Path $unitDir 'path-logic.exe'
    if (Test-Path $unitExe) {
        $unit = Start-Process -FilePath $unitExe -ArgumentList '/S' -PassThru -Wait
        Check ($unit.ExitCode -eq 0) ('all PATH logic cases pass (exit code = failures = ' + $unit.ExitCode + ')')
        $report = Get-Content -Path (Join-Path $unitDir 'path-logic-report.txt') -Raw -ErrorAction SilentlyContinue
        Check-Contains $report 'cases 13, failures 0' 'the PATH logic report says 13 cases, 0 failures'
        Check-Contains $report 'PASS leading-empty' 'a leading empty PATH segment survives the round trip'
        Check-Contains $report 'PASS middle-empty' 'a middle empty PATH segment survives the round trip'
        Check-Contains $report 'PASS prefix-trap' 'C:\a does not match C:\ab (whole-segment compare)'
        Check-Contains $report 'PASS duplicate-ours' 'duplicate entries are all removed, not just the first'
    } else {
        Check $false 'the PATH logic harness produced an exe'
    }
}

# ---------------------------------------------------------------------------
# 9) uninstall through the real ARP path (plain /S)
# ---------------------------------------------------------------------------
Write-Host ''
Write-Host 'uninstall (ARP path, plain /S)'

# A fake installation record proves that uninstalling this tool never deletes the
# bookkeeping that allows the publisher certificate to be reclaimed later.
$stateDir = Join-Path $dataRoot 'state'
New-Item -ItemType Directory -Force -Path $stateDir | Out-Null
$fakeRecord = Join-Path $stateDir 'e2e-fake.json'
Set-Content -Path $fakeRecord -Value '{"installId":"e2e-fake"}' -Encoding ASCII

Clear-Logs
$uninstaller = Join-Path $appDir 'Uninstall.exe'
Start-Process -FilePath $uninstaller -ArgumentList '/S' | Out-Null
$gone = Wait-ForFileGone (Join-Path $appDir 'msixpkg-native.exe')
$uninstallLog = Get-NewestLog
Check $gone 'the CLI is gone after a plain /S uninstall'
Check (Wait-ForFileGone $appDir 15) 'the install directory is removed by the ARP uninstall path'
Check-Contains $uninstallLog 'uninstall done' 'uninstaller logged completion'
Check-Contains $uninstallLog 'state: records kept' 'uninstaller noticed the installation records'
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
        Check ($null -eq (Get-RegValue $userEnvKey 'Path')) 'PATH value created by the installer was removed again'
    }
} else {
    Skip 'uninstall registry cleanup assertions (unsigned executables are denied registry writes here)'
}

# ---------------------------------------------------------------------------
# 10) second round: the `_?=` form returns the real uninstaller exit code
# ---------------------------------------------------------------------------
Write-Host ''
Write-Host 'uninstall (_?= form, real exit code)'

$second = Invoke-Setup @('/S', '/CURRENTUSER', '/NODESKTOP', "/D=$appDir2")
Check ($second.ExitCode -eq 0) ('second install exits 0 (got ' + $second.ExitCode + ')')
if (Test-Path (Join-Path $appDir2 'Uninstall.exe')) {
    $selfDelete = Start-Process -FilePath (Join-Path $appDir2 'Uninstall.exe') -ArgumentList '/S', "_?=$appDir2" -PassThru -Wait
    Check ($selfDelete.ExitCode -eq 0) ('uninstaller with _?= exits 0 (got ' + $selfDelete.ExitCode + ')')
    Check (-not (Test-Path (Join-Path $appDir2 'msixpkg-native.exe'))) 'the payload is gone with _?='
    # NSIS keeps the uninstaller itself when `_?=` is used (the caller owns the directory),
    # so remove the leftovers here instead of asserting them away.
    Remove-Item $appDir2 -Recurse -Force -ErrorAction SilentlyContinue
    Check (-not (Test-Path $appDir2)) 'leftover directory removed by the caller'
} else {
    Check $false 'second install produced an uninstaller'
}

# ---------------------------------------------------------------------------
# 11) portable package (no NSIS stub, so no writable %TEMP% required)
# ---------------------------------------------------------------------------
Write-Host ''
Write-Host 'portable package (no NSIS stub, no %TEMP% needed)'

# An NSIS setup needs a writable %TEMP% before its script runs at all, which is not a
# given in containers, sandboxes or locked-down machines. The zip is the fallback for
# those, so it gets the same integrity checks as the installed payload.
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
    Check ($portableRun.ExitCode -eq 0) ('the unpacked CLI runs with no NSIS temporary folder involved (exit code ' + $portableRun.ExitCode + ')')
}

# ---------------------------------------------------------------------------
# 12) non-admin launcher (rescues setups when %TEMP% cannot be written)
# ---------------------------------------------------------------------------
Write-Host ''
Write-Host 'non-admin launcher (TEMP redirect)'

# The NSIS stub needs a writable %TEMP% before the setup script runs. On machines or
# sessions where that write is denied (containers, sandboxes, locked-down policies) a
# plain launch dies with "Error writing temporary file". The launcher picks a writable
# TEMP first, so the same non-elevated launch succeeds.
$launcher = Join-Path (Split-Path -Parent $setup) 'run-setup-with-local-temp.cmd'
if (-not (Test-Path $launcher)) {
    Skip 'non-admin launcher checks (run-setup-with-local-temp.cmd is not next to the setup exe)'
} else {
    $launcherApp = Join-Path $work 'launcher-app'
    $savedTemp = $env:TEMP
    $savedTmp = $env:TMP
    # Point this process at a %TEMP% the *child* cannot use: outside the workspace.
    $env:TEMP = Join-Path $env:LOCALAPPDATA 'Temp'
    $env:TMP = $env:TEMP
    try {
        $direct = Start-Process -FilePath $setup -ArgumentList '/S', '/CURRENTUSER', '/NODESKTOP', "/D=$launcherApp" -PassThru -Wait
        Write-Host ('  info  direct launch with that TEMP exited ' + $direct.ExitCode + ' (2 = the documented NSIS temp failure)')
        $launcherOutput = & cmd.exe /c "`"$launcher`" /S /CURRENTUSER /NODESKTOP /D=$launcherApp" 2>&1 | Out-String
        $launcherExit = $LASTEXITCODE
    } finally {
        $env:TEMP = $savedTemp
        $env:TMP = $savedTmp
    }
    Check ($launcherExit -eq 0) ('the launcher installs successfully with a %TEMP% the setup cannot use (exit ' + $launcherExit + ')')
    Check (Test-Path (Join-Path $launcherApp 'msixpkg-native.exe')) 'the launcher installed the payload'
    Check-Contains $launcherOutput 'Using TEMP=' 'the launcher reports which TEMP it chose'
}

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
