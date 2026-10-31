<#
.SYNOPSIS
    GUI smoke test for the native setup / uninstaller (Wren pages + MSAA report).

.DESCRIPTION
    Feeds synthetic pages through --ui-page / --ui-result / --ui-progress, so nothing is
    installed. Assertions use Widget::control_id (setup_resources.h) rather than localized
    captions, so the same script runs on zh-CN and en-US Windows.

    The GUI is a WIN32-subsystem program: Start-Process is the only safe way to wait.

    Expected: checks N, failures 0.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tests\e2e\setup-gui-smoke.ps1
#>
[CmdletBinding()]
param(
    [string]$SetupPath = '',
    [string]$UninstallPath = ''
)

$ErrorActionPreference = 'Stop'

$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if ([string]::IsNullOrWhiteSpace($SetupPath)) {
    $SetupPath = Join-Path $repo 'src\MsixpkgInstaller.Native\build\x64\Release\msixpkg-setup.exe'
}
if ([string]::IsNullOrWhiteSpace($UninstallPath)) {
    $UninstallPath = Join-Path $repo 'src\MsixpkgInstaller.Native\build\x64\Release\msixpkg-uninstall.exe'
}
$SetupPath = [IO.Path]::GetFullPath($SetupPath)
$UninstallPath = [IO.Path]::GetFullPath($UninstallPath)
if (-not (Test-Path $SetupPath)) { throw "setup not found: $SetupPath" }
if (-not (Test-Path $UninstallPath)) { throw "uninstaller not found: $UninstallPath" }

$outDir = Join-Path $repo 'artifacts\setup-gui-smoke'
if (Test-Path $outDir) { Remove-Item -Recurse -Force $outDir }
New-Item -ItemType Directory -Force $outDir | Out-Null

$script:checks = 0
$script:failures = 0

function Check([bool]$condition, [string]$name) {
    $script:checks++
    if ($condition) {
        Write-Host "ok   $name"
    } else {
        $script:failures++
        Write-Host "FAIL $name"
    }
}

function Quote([string]$value) {
    return '"' + $value + '"'
}

function Run-Page([string]$exe, [string]$name, [string[]]$arguments, [int]$expectedExit) {
    $reportPath = Join-Path $outDir ($name + '.txt')
    $process = Start-Process -FilePath $exe -ArgumentList ($arguments + @('--ui-report', (Quote $reportPath), '--exit-after', '1200')) -PassThru -Wait
    $report = if (Test-Path $reportPath) { [IO.File]::ReadAllText($reportPath, [Text.Encoding]::UTF8) } else { '' }
    Check ($process.ExitCode -eq $expectedExit) "$name exit code $expectedExit (got $($process.ExitCode))"
    Check ($report.Length -gt 0) "$name report written"
    Check ($report -match 'presenter=') "$name report has presenter"
    Check ($report -match 'frames=[1-9][0-9]*') "$name rendered at least one frame"
    Check ($report -match 'msaa\.requests=') "$name report has MSAA counter"
    return $report
}

function Child-StateById([string]$report, [int]$id) {
    $pattern = 'id=' + $id + '\s+role=.*?state=0x([0-9A-Fa-f]{8})'
    $match = [regex]::Match($report, $pattern)
    if (-not $match.Success) { return -1 }
    return [Convert]::ToInt32($match.Groups[1].Value, 16)
}

function Is-Disabled([int]$state) {
    return (($state -band 4) -ne 0)
}

function State-Has([string]$report, [int]$id, [int]$bit) {
    $state = Child-StateById $report $id
    return ($state -ge 0 -and (($state -band $bit) -ne 0))
}

# Wren buttons have no child HWNDs; exercise the footer with real WM_LBUTTONDOWN/UP.
# Regression: when page panels covered the whole client area, the footer looked
# clickable but the page swallowed every click.
if (-not ('MsixpkgSmokeMouse' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class MsixpkgSmokeMouse {
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr hwnd, uint message, IntPtr wparam, IntPtr lparam);
    [DllImport("user32.dll")] public static extern uint GetDpiForWindow(IntPtr hwnd);
}
'@
}

function Click-Client([IntPtr]$hwnd, [int]$x, [int]$y) {
    # Wren coordinates are DIP; WM_LBUTTONDOWN lParam is in physical pixels.
    $dpi = [MsixpkgSmokeMouse]::GetDpiForWindow($hwnd)
    if ($dpi -eq 0) { $dpi = 96 }
    $px = [int][Math]::Round($x * $dpi / 96.0)
    $py = [int][Math]::Round($y * $dpi / 96.0)
    $lparam = [IntPtr](($py -shl 16) -bor $px)
    [MsixpkgSmokeMouse]::PostMessage($hwnd, 0x0201, [IntPtr]1, $lparam) | Out-Null
    Start-Sleep -Milliseconds 80
    [MsixpkgSmokeMouse]::PostMessage($hwnd, 0x0202, [IntPtr]0, $lparam) | Out-Null
}

function Test-FooterClick([string]$exe) {
    $process = Start-Process -FilePath $exe -ArgumentList @('--ui-page', 'options', '--exit-after', '5000') -PassThru
    Start-Sleep -Milliseconds 1200
    $process.Refresh()
    $hwnd = $process.MainWindowHandle
    if ($hwnd -eq 0) {
        Start-Sleep -Milliseconds 800
        $process.Refresh()
        $hwnd = $process.MainWindowHandle
    }
    if ($hwnd -eq 0) {
        try { $process.Kill() } catch {}
        return -1
    }

    # The Cancel button: left column, footer (DIP 28..148 x 502..536, centre 88,519).
    Click-Client $hwnd 88 519
    $deadline = (Get-Date).AddSeconds(5)
    while (-not $process.HasExited -and (Get-Date) -lt $deadline) {
        Start-Sleep -Milliseconds 100
        $process.Refresh()
    }
    if (-not $process.HasExited) {
        try { $process.Kill() } catch {}
        return -1
    }
    return $process.ExitCode
}

# ---------- setup pages ----------
$welcome = Run-Page $SetupPath 'welcome' @('--ui-page', 'welcome') 0
Check ($welcome -match 'page=welcome') 'welcome page is welcome'
Check ($welcome -match 'mode=install') 'welcome mode is install'
Check ((Child-StateById $welcome 2051) -ge 0) 'welcome exposes next'

$options = Run-Page $SetupPath 'options' @('--ui-page', 'options') 0
Check ($options -match 'page=options') 'options page is options'
Check ((Child-StateById $options 2010) -ge 0) 'options exposes all-users choice'
Check ((Child-StateById $options 2011) -ge 0) 'options exposes current-user choice'
Check ((Child-StateById $options 2012) -ge 0) 'options exposes install path'
Check ((Child-StateById $options 2013) -ge 0) 'options exposes browse'
Check ((Child-StateById $options 2020) -ge 0) 'options exposes association checkbox'
Check ((Child-StateById $options 2021) -ge 0) 'options exposes PATH checkbox'
Check ((Child-StateById $options 2022) -ge 0) 'options exposes desktop checkbox'
Check (State-Has $options 2020 8) 'association is checked by default'
Check (State-Has $options 2021 8) 'PATH is checked by default'
Check (-not (State-Has $options 2022 8)) 'desktop shortcut is off by default'
$next = Child-StateById $options 2051
Check ($next -ge 0 -and -not (Is-Disabled $next)) 'options enables install'

$optionsDark = Run-Page $SetupPath 'options-dark' @('--ui-page', 'options', '--ui-theme', 'dark', '--ui-material', 'solid') 0
Check ($optionsDark -match 'appearance=dark') 'dark theme applied'
Check ($optionsDark -match 'material=Solid') 'solid material applied'

$progress = Run-Page $SetupPath 'progress' @('--ui-page', 'progress', '--ui-progress', '42') 0
Check ($progress -match 'page=progress') 'progress page is progress'
Check ((Child-StateById $progress 2030) -ge 0) 'progress exposes the bar'
Check ($progress -match 'id=2030 .*?value="42%"') 'progress reports 42 percent'
Check ((Child-StateById $progress 2031) -ge 0) 'progress exposes the phase timeline'
Check ((Child-StateById $progress 2034) -ge 0) 'progress exposes the log'

$success = Run-Page $SetupPath 'finish-success' @('--ui-page', 'finish', '--ui-result', 'success') 0
Check ($success -match 'page=finish') 'finish page is finish'
Check ((Child-StateById $success 2042) -ge 0) 'success exposes the body'
Check ((Child-StateById $success 2043) -ge 0) 'success exposes run-after'
Check ($success -match 'id=2043 .*?state=0x00000009') 'run-after is checked'
$closeState = Child-StateById $success 2052
Check ($closeState -ge 0 -and -not (Is-Disabled $closeState)) 'success enables close'

$warning = Run-Page $SetupPath 'finish-warning' @('--ui-page', 'finish', '--ui-result', 'warning') 0
Check ($warning -match 'page=finish') 'warning finish page is finish'
Check ($warning -match 'id=2041 .*?name="[^"]*"') 'warning finish has a title'

$failure = Run-Page $SetupPath 'finish-failure' @('--ui-page', 'finish', '--ui-result', 'failure') 5
Check ($failure -match 'page=finish') 'failure finish page is finish'
Check ((Child-StateById $failure 2052) -ge 0) 'failure exposes close'

# ---------- uninstaller pages ----------
$confirm = Run-Page $UninstallPath 'confirm' @('--ui-page', 'confirm') 0
Check ($confirm -match 'page=confirm') 'confirm page is confirm'
Check ($confirm -match 'mode=uninstall') 'confirm mode is uninstall'
Check ((Child-StateById $confirm 2060) -ge 0) 'confirm exposes the body'
Check ((Child-StateById $confirm 2051) -ge 0) 'confirm exposes uninstall'

$uninstallProgress = Run-Page $UninstallPath 'uninstall-progress' @('--ui-page', 'progress', '--ui-progress', '55') 0
Check ($uninstallProgress -match 'page=progress') 'uninstall progress page is progress'
Check ((Child-StateById $uninstallProgress 2031) -ge 0) 'uninstall progress exposes the timeline'

$uninstallDone = Run-Page $UninstallPath 'uninstall-finish' @('--ui-result', 'success') 0
Check ($uninstallDone -match 'page=finish') 'uninstall finish page is finish'
Check ((Child-StateById $uninstallDone 2042) -ge 0) 'uninstall finish exposes the body'

# ---------- footer buttons are actually clickable ----------
$footerExit = Test-FooterClick $SetupPath
Check ($footerExit -eq 6) "footer cancel button is clickable (exit $footerExit; 6 = cancelled)"

''
"checks $script:checks, failures $script:failures"
if ($script:failures -ne 0) { exit 1 }
exit 0
