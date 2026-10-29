# GUI smoke test: drive the Wren wizard through its diagnostic pages and assert the reports.
#
# The GUI is a WIN32 subsystem program: Start-Process is the only safe way to wait.
# This script never installs anything: --ui-page / --ui-result / --ui-progress only feed
# synthetic data into the wizard, and --exit-after closes it.
#
# Assertions use Widget::control_id (resource.h IDC_*) instead of localized captions, so
# the same script works on zh-CN and en-US Windows.
#
# Expected: checks N, failures 0.

param(
    [string]$GuiPath = "",
    [string]$VectorPath = ""
)

$ErrorActionPreference = "Stop"

$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if ([string]::IsNullOrWhiteSpace($GuiPath)) {
    $GuiPath = Join-Path $repo "src\MsixpkgInstaller.Native\build\x64\Release\msixpkg-installer.exe"
}
if ([string]::IsNullOrWhiteSpace($VectorPath)) {
    $VectorPath = Join-Path $repo "tests\vectors\Msixpkg.Sample_1.0.0.0_x64.msixpkg"
}

$GuiPath = [IO.Path]::GetFullPath($GuiPath)
$VectorPath = [IO.Path]::GetFullPath($VectorPath)
if (-not (Test-Path $GuiPath)) { throw "GUI not found: $GuiPath" }
if (-not (Test-Path $VectorPath)) { throw "vector not found: $VectorPath" }

$outDir = Join-Path $repo "artifacts\gui-smoke"
if (Test-Path $outDir) { Remove-Item -Recurse -Force $outDir }
New-Item -ItemType Directory -Force $outDir | Out-Null

# A fresh data root means the publisher policy has no remembered pins, so the golden
# sample previews as "unpinned" (the consent page renders) instead of E_PUBLISHER_NOT_PINNED.
$dataRoot = Join-Path $outDir "data-root"
New-Item -ItemType Directory -Force $dataRoot | Out-Null
$env:MSIXPKG_DATA_ROOT = $dataRoot

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

function Invoke-Wizard([string[]]$arguments, [int]$timeoutMs = 20000) {
    $process = Start-Process -FilePath $GuiPath -ArgumentList $arguments -PassThru
    $deadline = [DateTime]::UtcNow.AddMilliseconds($timeoutMs)
    while (-not $process.HasExited -and [DateTime]::UtcNow -lt $deadline) {
        Start-Sleep -Milliseconds 100
        $process.Refresh()
    }

    if (-not $process.HasExited) {
        try { $process.Kill() } catch {}
        return -1
    }

    return $process.ExitCode
}

function Read-Report([string]$path) {
    if (-not (Test-Path $path)) { return "" }
    return [IO.File]::ReadAllText($path, [Text.Encoding]::UTF8)
}

# Resolve the Wren internal state mask reported as "child[i] id=NNNN ... state=0xZZZZZZZZ".
# Bit values: 1=Focusable 2=Focused 4=Disabled 8=Checked.
function Child-StateById([string]$report, [int]$id) {
    $pattern = 'id=' + $id + '\s+role=.*?state=0x([0-9A-Fa-f]{8})'
    $match = [regex]::Match($report, $pattern)
    if (-not $match.Success) { return -1 }
    return [Convert]::ToInt32($match.Groups[1].Value, 16)
}

function Is-Disabled([int]$state) {
    return (($state -band 4) -ne 0)
}

function Count-Pattern([string]$report, [string]$pattern) {
    return ([regex]::Matches($report, $pattern)).Count
}

function Run-Page([string]$name, [string[]]$arguments, [int]$expectedExit) {
    $reportPath = Join-Path $outDir ($name + ".txt")
    $exit = Invoke-Wizard ($arguments + @("--ui-report", (Quote $reportPath), "--exit-after", "1200"))
    $report = Read-Report $reportPath
    Check ($exit -eq $expectedExit) "$name exit code $expectedExit (got $exit)"
    Check ($report.Length -gt 0) "$name report written"
    Check ($report -match 'presenter=') "$name report has presenter"
    Check ($report -match 'frames=[1-9][0-9]*') "$name rendered at least one frame"
    Check ($report -match 'msaa\.requests=') "$name report has MSAA request counter"
    return $report
}

# ---------- empty state ----------
$empty = Run-Page "empty" @() 0
Check ($empty -match 'page=summary') "empty stays on summary"
Check ((Child-StateById $empty 1072) -ge 0) "empty exposes cancel button id"
$nextState = Child-StateById $empty 1070
Check ($nextState -ge 0 -and (Is-Disabled $nextState)) "empty disables next"

# ---------- summary with a real container ----------
$summary = Run-Page "summary" @((Quote $VectorPath)) 0
Check ($summary -match 'page=summary') "summary page is summary"
Check ((Child-StateById $summary 1010) -ge 0) "summary shows package name"
Check ((Child-StateById $summary 1011) -ge 0) "summary shows version value"
Check ((Child-StateById $summary 1015) -ge 0) "summary shows dependencies value"
Check ((Child-StateById $summary 1016) -ge 0) "summary shows description value"
$nextState = Child-StateById $summary 1070
Check ($nextState -ge 0 -and -not (Is-Disabled $nextState)) "loaded summary enables next"

# ---------- light/solid theme ----------
$light = Run-Page "summary-light" @((Quote $VectorPath), "--ui-theme", "light", "--ui-material", "solid") 0
Check ($light -match 'appearance=light') "light theme applied"
Check ($light -match 'material=Solid') "solid material applied"

# ---------- consent safety gate ----------
$consent = Run-Page "consent" @((Quote $VectorPath), "--ui-page", "consent") 0
Check ($consent -match 'page=consent') "consent page is consent"
Check ((Child-StateById $consent 1030) -ge 0) "consent exposes certificate details"
Check ((Child-StateById $consent 1071) -ge 0) "consent exposes back button"
Check ((Child-StateById $consent 1032) -ge 0) "consent exposes accept checkbox"
Check ((Child-StateById $consent 1033) -ge 0) "consent exposes trust checkbox"
Check ((Child-StateById $consent 1034) -ge 0) "consent exposes all-users checkbox"
$nextState = Child-StateById $consent 1070
Check ($nextState -ge 0 -and (Is-Disabled $nextState)) "consent disables next before consent"
$trustState = Child-StateById $consent 1033
Check ($trustState -ge 0 -and (Is-Disabled $trustState)) "consent disables trust before consent"

# ---------- progress ----------
$progress = Run-Page "progress-42" @("--ui-progress", "42") 0
Check ($progress -match 'page=progress') "progress page is progress"
Check ((Child-StateById $progress 1040) -ge 0) "progress exposes progressbar"
Check ($progress -match 'id=1040 .*?value="42%"') "progress reports 42 percent"
Check ((Child-StateById $progress 1041) -ge 0) "progress exposes phase timeline"
Check ((Child-StateById $progress 1045) -ge 0) "progress exposes event log"

# ---------- result ----------
$success = Run-Page "result-success" @("--ui-result", "success") 0
Check ($success -match 'page=result') "result page is result"
Check ((Child-StateById $success 1050) -ge 0) "success exposes the body"
$launchState = Child-StateById $success 1052
Check ($launchState -ge 0 -and -not (Is-Disabled $launchState)) "success enables launch"

$failure = Run-Page "result-failure" @("--ui-result", "failure") 5
Check ($failure -match 'page=result') "failure page is result"
$launchState = Child-StateById $failure 1052
Check ($launchState -ge 0 -and (Is-Disabled $launchState)) "failure disables launch"

$cancelled = Run-Page "result-cancelled" @("--ui-result", "cancelled") 6
Check ($cancelled -match 'page=result') "cancelled page is result"

# ---------- manage ----------
$manage = Run-Page "manage" @("--ui-page", "manage") 0
Check ($manage -match 'page=manage') "manage page is manage"
Check ((Child-StateById $manage 1060) -ge 0) "manage exposes the table"
Check ((Child-StateById $manage 1062) -ge 0) "manage exposes uninstall"
Check ((Child-StateById $manage 1063) -ge 0) "manage exposes repair"
Check ((Child-StateById $manage 1064) -ge 0) "manage exposes open-logs"
Check ((Child-StateById $manage 1065) -ge 0) "manage exposes open-settings"

""
"checks $script:checks, failures $script:failures"
if ($script:failures -ne 0) { exit 1 }
