<#
.SYNOPSIS
    Runs inside a Windows Sandbox (started by msixpkg-e2e.wsb).

.DESCRIPTION
    The sandbox has no toolchain, so this script only *runs* what the host built:

      1. the negative E2E suite (container validation, plans, usage, failure reporting)
      2. the full install / uninstall round trip, if a container was staged in
         C:\msixpkg\artifacts\e2e-full\sample.msixpkg

    Exit code 0 means everything that could be checked in a clean machine passed.
#>
[CmdletBinding()]
param()

$ErrorActionPreference = 'Continue'
$repo = 'C:\msixpkg'

function Write-Banner {
    param([string]$Text)
    Write-Host ''
    Write-Host ('==== ' + $Text + ' ====') -ForegroundColor Cyan
}

Write-Banner 'msixpkg E2E inside Windows Sandbox'
Write-Host ('  OS        : ' + (Get-CimInstance Win32_OperatingSystem).Caption + ' build ' + [Environment]::OSVersion.Version.Build)
Write-Host ('  elevated  : ' + (New-Object Security.Principal.WindowsPrincipal(
    [Security.Principal.WindowsIdentity]::GetCurrent())).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator))

$failures = 0

# --- 1. negative suite -------------------------------------------------------
$negative = Join-Path $repo 'tests\e2e\msixpkg-negative.ps1'
$exe = Join-Path $repo 'src\MsixpkgInstaller.Native\build\x64\Release\msixpkg-native.exe'

if ((Test-Path $negative) -and (Test-Path $exe)) {
    Write-Banner 'negative suite'
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $negative -Configuration Release -ExePath $exe
    if ($LASTEXITCODE -ne 0) { $failures++ }
} else {
    Write-Host '  SKIP  the Release binaries were not mapped into the sandbox' -ForegroundColor Yellow
    Write-Host ('        expected: ' + $exe) -ForegroundColor Yellow
}

# --- 2. full round trip ------------------------------------------------------
$container = Join-Path $repo 'artifacts\e2e-full\sample.msixpkg'
$full = Join-Path $repo 'tests\e2e\msixpkg-e2e.ps1'

if ((Test-Path $full) -and (Test-Path $container)) {
    Write-Banner 'full install / uninstall round trip'
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $full -Configuration Release -ContainerPath $container
    if ($LASTEXITCODE -ne 0) { $failures++ }
} else {
    Write-Host ''
    Write-Host '  SKIP  no container was staged for the full round trip' -ForegroundColor Yellow
    Write-Host ('        put one at: ' + $container) -ForegroundColor Yellow
    Write-Host '        (on the host: msixpkg pack --msix <App>.msix --pfx <dev>.pfx --password <pw> --cert-output <dir>\publisher.cer --output artifacts\e2e-full\sample.msixpkg)' -ForegroundColor Yellow
}

Write-Banner 'summary'
if ($failures -gt 0) {
    Write-Host ('sandbox E2E FAILED (' + $failures + ' suite(s))') -ForegroundColor Red
    exit 1
}

Write-Host 'sandbox E2E passed' -ForegroundColor Green
exit 0
