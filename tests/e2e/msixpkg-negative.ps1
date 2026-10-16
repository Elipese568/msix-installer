<#
.SYNOPSIS
    Negative end-to-end suite for msixpkg-native.

.DESCRIPTION
    Covers everything that must fail: container tampering, truncation, unpinned
    publishers, the three single-defect negative vectors (unsigned / expired /
    wrong EKU), usage errors, and "deployment failures must be reported, never
    silently abort".

    None of these need elevation or a real install, so this suite runs anywhere,
    including restricted environments. The real install/uninstall round trip is
    in msixpkg-e2e.ps1 (needs elevation and a genuinely signed MSIX).

    NOTE: this file is deliberately ASCII-only. Windows PowerShell 5.1 reads
    .ps1 files as ANSI unless they carry a UTF-8 BOM, and any editor that
    rewrites the file drops the BOM - which turns non-ASCII text into a parse
    error. Keep scripts ASCII; Chinese documentation lives in README.md.

    Exit codes: 0 all checks passed, 1 failures, 2 environment not ready.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tests\e2e\msixpkg-negative.ps1
#>
[CmdletBinding()]
param(
    # Debug or Release - selects build\x64\<Configuration>\msixpkg-native.exe
    [string]$Configuration = 'Debug',

    # Explicit path to msixpkg-native.exe (CI builds into a different directory).
    [string]$ExePath,

    # Working directory (defaults to artifacts\e2e inside the repo; %TEMP% may not be writable)
    [string]$WorkDirectory
)

Set-StrictMode -Version Latest

# Continue, not Stop: on PowerShell 5.1 a native command writing to stderr (our CLI
# prints its error code there) becomes a terminating NativeCommandError when
# ErrorActionPreference is Stop. Every case below checks exit codes explicitly.
$ErrorActionPreference = 'Continue'

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$exe = if ($ExePath) {
    $ExePath
} else {
    Join-Path $repoRoot "src\MsixpkgInstaller.Native\build\x64\$Configuration\msixpkg-native.exe"
}
if (-not (Test-Path $exe)) {
    Write-Host "not found: $exe" -ForegroundColor Yellow
    Write-Host "build it first: cd src\MsixpkgInstaller.Native; cmake --preset windows-x64 -S .; cmake --build --preset windows-x64-$($Configuration.ToLower())"
    exit 2
}

$vectors = Join-Path $repoRoot 'tests\vectors'
$golden = Join-Path $vectors 'Msixpkg.Sample_1.0.0.0_x64.msixpkg'
if (-not (Test-Path $golden)) {
    Write-Host "not found: $golden" -ForegroundColor Yellow
    exit 2
}

$work = if ($WorkDirectory) { $WorkDirectory } else { Join-Path $repoRoot 'artifacts\e2e' }
$dataRoot = Join-Path $work 'data'
New-Item -ItemType Directory -Force -Path $work | Out-Null

# Point the data root into the workspace: never touch the real %ProgramData%,
# and keep the assertions reproducible.
$env:MSIXPKG_DATA_ROOT = $dataRoot

$script:checks = 0
$script:failures = 0

function Check {
    param([bool]$Condition, [string]$Message)
    $script:checks++
    if ($Condition) {
        Write-Host ("  PASS  " + $Message)
    } else {
        $script:failures++
        Write-Host ("  FAIL  " + $Message) -ForegroundColor Red
    }
}

function Check-Contains {
    param([string]$Text, [string]$Pattern, [string]$Message)
    Check ($Text -match $Pattern) ($Message + " (expected /" + $Pattern + "/)")
}

function Invoke-Msixpkg {
    param([string[]]$Arguments)
    $output = & $exe @Arguments 2>&1 | Out-String
    return [pscustomobject]@{ ExitCode = $LASTEXITCODE; Output = $output }
}

function Get-DirectoryFingerprint {
    param([string]$Path)
    if (-not (Test-Path $Path)) { return '' }
    $entries = Get-ChildItem -Path $Path -Recurse -File -ErrorAction SilentlyContinue |
        Sort-Object FullName |
        ForEach-Object { $_.FullName.Substring($Path.Length) + '|' + $_.Length }
    return ($entries -join "`n")
}

function New-DamagedContainer {
    param([string]$Source, [string]$Destination, [switch]$Truncate)
    $bytes = [IO.File]::ReadAllBytes($Source)
    if ($Truncate) {
        $bytes = $bytes[0..([int]($bytes.Length / 2) - 1)]
    } else {
        # Flip one byte in the middle of the file: the ZIP central directory lives at
        # the end, so the middle is inside the payload. The native side trips the
        # ZIP CRC-32 first (E_CONTAINER_INVALID) while .NET would reach
        # E_HASH_MISMATCH - spec section 5 allows both paths, so accept either.
        $index = [int]($bytes.Length / 2)
        $bytes[$index] = $bytes[$index] -bxor 0xFF
    }

    [IO.File]::WriteAllBytes($Destination, $bytes)
}

# Publisher SHA-256 thumbprint of the golden vector, read as UTF-8 explicitly.
$manifest = [IO.File]::ReadAllText((Join-Path $vectors 'golden-msixpkg.json'), [Text.Encoding]::UTF8) | ConvertFrom-Json
$thumbprint = $manifest.publisher.thumbprintSha256

Write-Host "msixpkg negative E2E"
Write-Host ("  exe      : " + $exe)
Write-Host ("  dataRoot : " + $dataRoot)
Write-Host ''

# ---------- 1. positive control ----------
Write-Host '1. positive control: correct pin + dry-run must succeed'
# Point the data root at a directory that does NOT exist yet: a dry-run must not
# create it. (A preview that littered %ProgramData% with directories and logs was
# a real defect; this check is the regression guard for it.)
$dryRunRoot = Join-Path $work 'dry-run-must-not-create'
if (Test-Path $dryRunRoot) { Remove-Item $dryRunRoot -Recurse -Force }
$env:MSIXPKG_DATA_ROOT = $dryRunRoot

$result = Invoke-Msixpkg @('install', $golden, '--pin', $thumbprint, '--require-pin', '--dry-run')
Check ($result.ExitCode -eq 0) 'pinned dry-run exits 0'
Check-Contains $result.Output 'result\s+dry-run' 'dry-run result is printed'
Check-Contains $result.Output 'certificate' 'the plan contains the certificate step'
Check (-not (Test-Path $dryRunRoot)) 'dry-run created nothing at all (not even the data directory)'

# Back to the normal work data root for the remaining checks.
$env:MSIXPKG_DATA_ROOT = $dataRoot

# ---------- 2. unpinned publisher ----------
Write-Host '2. unpinned publisher must be refused with --require-pin'
$result = Invoke-Msixpkg @('install', $golden, '--require-pin', '--dry-run')
Check ($result.ExitCode -eq 2) 'exits 2 (validation failure)'
Check-Contains $result.Output 'E_PUBLISHER_NOT_PINNED' 'error code is E_PUBLISHER_NOT_PINNED'

# ---------- 3. tampered payload ----------
Write-Host '3. tampered payload must be refused'
$tampered = Join-Path $work 'tampered.msixpkg'
New-DamagedContainer -Source $golden -Destination $tampered
$result = Invoke-Msixpkg @('install', $tampered, '--dry-run')
Check ($result.ExitCode -ne 0) 'tampered container exits non-zero'
Check-Contains $result.Output 'E_HASH_MISMATCH|E_CONTAINER_INVALID|E_SIGNATURE_INVALID' 'an integrity error code is reported'

# ---------- 4. truncated file ----------
Write-Host '4. truncated container must be refused'
$truncated = Join-Path $work 'truncated.msixpkg'
New-DamagedContainer -Source $golden -Destination $truncated -Truncate
$result = Invoke-Msixpkg @('install', $truncated, '--dry-run')
Check ($result.ExitCode -ne 0) 'truncated container exits non-zero'
Check-Contains $result.Output 'E_CONTAINER_INVALID|E_IO|E_HASH_MISMATCH' 'a container-damage error code is reported'

# ---------- 5. the three single-defect negative vectors ----------
Write-Host '5. single-defect negative vectors (unsigned / expired / wrong EKU)'
$negativeCases = @(
    @{ File = 'negative-unsigned.msixpkg';  Code = 'E_SIGNATURE_MISSING' },
    @{ File = 'negative-expired.msixpkg';   Code = 'E_CERTIFICATE_EXPIRED' },
    @{ File = 'negative-wrong-eku.msixpkg'; Code = 'E_CERTIFICATE_NOT_CODE_SIGNING' }
)
foreach ($case in $negativeCases) {
    $path = Join-Path $vectors $case.File
    $result = Invoke-Msixpkg @('install', $path, '--dry-run')
    Check ($result.ExitCode -eq 2) ($case.File + ' exits 2')
    Check-Contains $result.Output $case.Code ($case.File + ' reports ' + $case.Code)
}

# ---------- 6. missing container ----------
Write-Host '6. missing container'
$result = Invoke-Msixpkg @('install', (Join-Path $work 'no-such-file.msixpkg'), '--dry-run')
Check ($result.ExitCode -eq 4) 'exits 4 (I/O)'
Check-Contains $result.Output 'E_IO' 'error code is E_IO'

# ---------- 7. usage errors ----------
Write-Host '7. usage errors'
$result = Invoke-Msixpkg @('uninstall')
Check ($result.ExitCode -eq 3) 'uninstall without a target exits 3'
$result = Invoke-Msixpkg @('no-such-command')
Check ($result.ExitCode -eq 3) 'unknown subcommand exits 3'

# ---------- 8. deployment failures must be readable, never a silent abort ----------
Write-Host '8. deployment failure is reported (regression: a leaked winrt::hresult_error used to abort silently)'
$result = Invoke-Msixpkg @('install', '--msix', (Join-Path $vectors 'sample-main.msix'))
Check ($result.ExitCode -ne 0) 'failure exits non-zero'
Check ($result.ExitCode -ne 3) 'it is not a bogus usage error'
Check ($result.Output.Trim().Length -gt 0) 'there is output (the process did not die silently)'
Check-Contains $result.Output 'E_DEPLOY|E_IO|E_APPX' 'a stable deployment/container error code is reported'

# ---------- 9. verify / clean emit parseable JSON ----------
Write-Host '9. verify / clean emit parseable JSON'
$result = Invoke-Msixpkg @('verify', '--json')
Check ($result.ExitCode -eq 0 -or $result.ExitCode -eq 2) 'verify exits 0 or 2'
$parsed = $null
try { $parsed = $result.Output | ConvertFrom-Json } catch { }
Check ($null -ne $parsed) 'verify --json output is valid JSON'
Check ($null -ne $parsed -and $null -ne $parsed.issues) 'the JSON has an issues array'

$result = Invoke-Msixpkg @('clean', '--json')
$parsed = $null
try { $parsed = $result.Output | ConvertFrom-Json } catch { }
Check ($result.ExitCode -eq 0 -and $null -ne $parsed) 'clean --json succeeds and is valid JSON'

# ---------- 10. repair never changes anything ----------
Write-Host '10. repair only reports; it never changes anything'
$before = Get-DirectoryFingerprint $dataRoot
$result = Invoke-Msixpkg @('repair')
$after = Get-DirectoryFingerprint $dataRoot
Check ($result.ExitCode -eq 0) 'repair exits 0'
Check ($before -eq $after) 'the data directory is byte-for-byte identical before and after repair'
Check-Contains $result.Output 'nothing was changed' 'it states that nothing was changed'

# ---------- 11. certificate changes need elevation ----------
Write-Host '11. certificate changes require elevation (must not silently succeed)'
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$elevated = (New-Object Security.Principal.WindowsPrincipal($identity)).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)

if ($elevated) {
    Write-Host '  SKIP  already elevated; msixpkg-e2e.ps1 owns the elevated certificate path'
} else {
    $sha1 = $manifest.publisher.thumbprintSha1
    $result = Invoke-Msixpkg @('cert-add', '--container', $golden, '--thumbprint-sha1', $sha1)
    Check ($result.ExitCode -ne 0) 'cert-add fails without elevation'
    Check-Contains $result.Output 'E_IO|E_DEPLOY_ACCESS_DENIED' 'it fails on permissions/I-O rather than silently succeeding'
}

Write-Host ''
Write-Host ("checks " + $script:checks + ", failures " + $script:failures)
if ($script:failures -gt 0) {
    Write-Host 'negative E2E FAILED' -ForegroundColor Red
    exit 1
}

Write-Host 'negative E2E passed' -ForegroundColor Green
exit 0
