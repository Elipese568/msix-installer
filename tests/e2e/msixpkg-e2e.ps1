<#
.SYNOPSIS
    Full install/uninstall round trip for msixpkg-native (requires elevation).

.DESCRIPTION
    Verifies the whole loop against the real Windows deployment stack:

        pack -> install -> package registered + certificate trusted + ARP entry
             -> launch the app -> uninstall -> package gone + certificate reclaimed
             -> uninstall again (idempotent)

    Requirements
      * an ELEVATED PowerShell (the certificate must go into
        LocalMachine\TrustedPeople, and the ARP entry into HKLM);
      * a genuinely signed MSIX whose signer certificate you also have as a PFX
        (the container's four-way identity check compares the container signer,
        publisher.cer, the inner AppxSignature.p7x signer and the Appx manifest
        publisher - they must all be the same certificate);
      * the built native binaries and the built .NET packer.

    Produce the inputs with the documented toolchain, e.g.

        winapp cert generate --output samples\SamplePackagedApp\certs
        winapp sign --file <App>.msix --pfx <dev>.pfx --password <pw>
        msixpkg pack --msix <App>.msix --pfx <dev>.pfx --password <pw> --output <App>.msixpkg
        powershell -ExecutionPolicy Bypass -File tests\e2e\msixpkg-e2e.ps1 -ContainerPath <App>.msixpkg

    Everything this script creates on the machine is removed again at the end
    (including the certificate, even if an assertion failed halfway).

    NOTE: ASCII-only on purpose - PowerShell 5.1 reads .ps1 as ANSI without a BOM,
    and editors drop the BOM. Chinese documentation lives in README.md.

    Exit codes: 0 passed, 1 failures, 2 environment not ready.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tests\e2e\msixpkg-e2e.ps1 -ContainerPath C:\tmp\App.msixpkg
#>
[CmdletBinding()]
param(
    # Pre-packed container to install. Mutually exclusive with -MsixPath.
    [string]$ContainerPath,

    # A signed .msix to pack into a container first (needs -Pfx).
    [string]$MsixPath,
    [string]$Pfx,
    [string]$PfxPassword = 'msixpkg-test-only',
    [string]$OutputContainer,

    # Expected package identity name (defaults to the container manifest value).
    [string]$IdentityName,

    # Skip launching the app after installing.
    [switch]$SkipLaunch,

    # Also remove the installed package + certificate on exit (default: yes).
    [switch]$KeepInstalled,

    [string]$Configuration = 'Debug',
    [string]$WorkDirectory
)

Set-StrictMode -Version Latest

# Continue, not Stop: a native command writing to stderr becomes a terminating
# NativeCommandError on PowerShell 5.1 when ErrorActionPreference is Stop.
$ErrorActionPreference = 'Continue'

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$nativeExe = Join-Path $repoRoot "src\MsixpkgInstaller.Native\build\x64\$Configuration\msixpkg-native.exe"
$packerExe = Join-Path $repoRoot 'src\Msixpkg.Packer\bin\Debug\net9.0\msixpkg.exe'
$work = if ($WorkDirectory) { $WorkDirectory } else { Join-Path $repoRoot 'artifacts\e2e-full' }
New-Item -ItemType Directory -Force -Path $work | Out-Null

$script:checks = 0
$script:failures = 0
$script:installedFamilyName = ''
$script:installedCertificateSha1 = ''
$script:installedCertificateWasAdded = $false
$script:arpKeyName = ''

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

function Invoke-Msixpkg {
    param([string[]]$Arguments)
    $output = & $nativeExe @Arguments 2>&1 | Out-String
    return [pscustomobject]@{ ExitCode = $LASTEXITCODE; Output = $output }
}

function Get-InstalledPackage {
    param([string]$FamilyName)
    return Get-AppxPackage -FamilyName $FamilyName -ErrorAction SilentlyContinue
}

function Get-TrustedCertificate {
    param([string]$Sha1)
    $normalized = $Sha1 -replace '\s', ''
    return Get-ChildItem Cert:\LocalMachine\TrustedPeople -ErrorAction SilentlyContinue |
        Where-Object { $_.Thumbprint -eq $normalized }
}

function Test-ArpEntry {
    param([string]$KeyName)
    $machine = "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\$KeyName"
    $user = "HKCU:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\$KeyName"
    return (Test-Path $machine) -or (Test-Path $user)
}

# ---------------------------------------------------------------------------
# Preconditions
# ---------------------------------------------------------------------------
Write-Host 'msixpkg full E2E'

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$elevated = (New-Object Security.Principal.WindowsPrincipal($identity)).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)

if (-not (Test-Path $nativeExe)) {
    Write-Host "not found: $nativeExe" -ForegroundColor Yellow
    Write-Host "build it: cd src\MsixpkgInstaller.Native; cmake --preset windows-x64 -S .; cmake --build --preset windows-x64-$($Configuration.ToLower())"
    exit 2
}

if (-not $elevated) {
    Write-Host 'This script must run from an ELEVATED PowerShell:' -ForegroundColor Yellow
    Write-Host '  the publisher certificate goes into LocalMachine\TrustedPeople and the' -ForegroundColor Yellow
    Write-Host '  uninstall entry into HKLM, both of which need administrator rights.' -ForegroundColor Yellow
    exit 2
}

# Resolve the container: use it directly, or pack one from a signed .msix.
if (-not $ContainerPath -and $MsixPath) {
    if (-not $Pfx) { Write-Host '-MsixPath also needs -Pfx (the signer certificate).' -ForegroundColor Yellow; exit 2 }
    if (-not $OutputContainer) { $OutputContainer = Join-Path $work 'sample.msixpkg' }
    if (-not (Test-Path $packerExe)) { Write-Host "not found: $packerExe (build the packer first)" -ForegroundColor Yellow; exit 2 }
    Write-Host '0. packing a container from the signed MSIX'
    $packOutput = & $packerExe pack --msix $MsixPath --pfx $Pfx --password $PfxPassword --cert-output (Join-Path $work 'publisher.cer') --output $OutputContainer 2>&1 | Out-String
    Check ($LASTEXITCODE -eq 0) 'the packer produced a container'
    Write-Host $packOutput
    $ContainerPath = $OutputContainer
}

if (-not $ContainerPath -or -not (Test-Path $ContainerPath)) {
    Write-Host 'No container to install.' -ForegroundColor Yellow
    Write-Host 'Pass -ContainerPath <App.msixpkg>, or -MsixPath <App.msix> -Pfx <dev.pfx>.' -ForegroundColor Yellow
    Write-Host 'A quick way to get one is the sample app:' -ForegroundColor Yellow
    Write-Host '  winapp cert generate   (then sign the sample app with the generated pfx)' -ForegroundColor Yellow
    Write-Host '  msixpkg pack --msix <App>.msix --pfx <dev>.pfx --password <pw>' -ForegroundColor Yellow
    exit 2
}

$ContainerPath = (Resolve-Path $ContainerPath).Path
Write-Host ("  container : " + $ContainerPath)

# The data root must be a real machine-wide location here: the certificate state
# is what we are verifying. Keep the default (%ProgramData%\Msixpkg) so the
# elevated helper and the main process agree on it.
$dataRoot = Join-Path $env:ProgramData 'Msixpkg'
Remove-Item Env:\MSIXPKG_DATA_ROOT -ErrorAction SilentlyContinue

# ---------------------------------------------------------------------------
# 1. Read the plan (dry-run) and learn the expected identity
# ---------------------------------------------------------------------------
Write-Host ''
Write-Host '1. dry-run: the plan must list every mutating step'

$inspect = & $nativeExe install $ContainerPath --dry-run --json 2>&1 | Out-String
$plan = $null
try { $plan = $inspect | ConvertFrom-Json } catch { }
Check ($null -ne $plan) 'dry-run --json produced parseable JSON'
if ($null -eq $plan) {
    Write-Host $inspect
    exit 1
}

if (-not $IdentityName) { $IdentityName = $plan.identityName }
$expectedFamilyName = $plan.familyName
$expectedCertificateSha1 = ($plan.certificateSha1 -replace '\s', '')

Check (-not [string]::IsNullOrEmpty($IdentityName)) 'the container reports an identity name'
Check ($plan.steps -contains 'certificate') 'the plan contains the certificate step'
Check ($plan.steps -contains 'main') 'the plan contains the main package step'
Check ($plan.steps -contains 'journal') 'the plan contains the bookkeeping step'

$publisherCer = Join-Path $work 'publisher.cer'
Check (Test-Path $publisherCer) 'a publisher.cer is available for the pin (produced by the packer)' 

# Pin the publisher: without a pin the installer refuses (that is the safe default).
$thumbprint = $null
if (Test-Path $publisherCer) {
    $cert = New-Object System.Security.Cryptography.X509Certificates.X509Certificate2($publisherCer)
    $thumbprint = ($cert.Thumbprint -replace '\s', '').ToLowerInvariant()
}

if (-not $thumbprint) {
    Write-Host 'Cannot determine the publisher SHA-256 thumbprint (need publisher.cer next to the container).' -ForegroundColor Yellow
    Write-Host 'Rebuild the container with: msixpkg pack ... --cert-output <dir>\publisher.cer' -ForegroundColor Yellow
    exit 2
}

Write-Host ("  identity  : " + $IdentityName)
Write-Host ("  family    : " + $expectedFamilyName)
Write-Host ("  pin(sha256): " + $thumbprint)

# ---------------------------------------------------------------------------
# 2. Install
# ---------------------------------------------------------------------------
Write-Host ''
Write-Host '2. install (pinned, unattended, already elevated -> no UAC prompt)'

$install = Invoke-Msixpkg @('install', $ContainerPath, '--pin', $thumbprint, '--require-pin', '--yes')
Check ($install.ExitCode -eq 0) 'install exits 0'
Write-Host $install.Output

$installed = Invoke-Msixpkg @('install', $ContainerPath, '--pin', $thumbprint, '--require-pin', '--yes', '--json')
$state = $null
try { $state = $installed.Output | ConvertFrom-Json } catch { }
Check ($null -ne $state -and $state.installed) 'install --json reports installed=true'

if ($null -eq $state) {
    Write-Host $installed.Output
    exit 1
}

$script:installedFamilyName = $state.familyName
$script:installedCertificateSha1 = ($state.certificateSha1 -replace '\s', '')
$script:installedCertificateWasAdded = [bool]$state.certificateAddedByInstaller
$script:arpKeyName = 'Msixpkg.' + $IdentityName

Check (-not [string]::IsNullOrEmpty($script:installedFamilyName)) 'a package family name was resolved'
Check ([bool]$state.certificateExistedBefore -eq $false) 'the certificate did not exist before this install'
Check ($script:installedCertificateWasAdded) 'the installer recorded that it added the certificate'

# ---------------------------------------------------------------------------
# 3. Assertions on the machine
# ---------------------------------------------------------------------------
Write-Host ''
Write-Host '3. the machine really changed'

$package = Get-InstalledPackage $script:installedFamilyName
Check ($null -ne $package) 'Get-AppxPackage finds the package'
Check ($null -ne (Get-TrustedCertificate $script:installedCertificateSha1)) 'the certificate is in LocalMachine\TrustedPeople'
Check (Test-ArpEntry $script:arpKeyName) 'the "Apps & features" uninstall entry exists'

$arpMachine = "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\$($script:arpKeyName)"
if (Test-Path $arpMachine) {
    $arp = Get-ItemProperty $arpMachine
    Check ($arp.UninstallString -match 'uninstall') 'the uninstall entry points at our uninstaller'
}

$stateFile = Join-Path $dataRoot ("state\" + $state.installId + ".json")
Check (Test-Path $stateFile) 'the installation record was written'

$verify = Invoke-Msixpkg @('verify', '--json')
$verifyJson = $null
try { $verifyJson = $verify.Output | ConvertFrom-Json } catch { }
Check ($null -ne $verifyJson) 'verify --json is parseable'
Check ($verify.ExitCode -eq 0) 'verify reports a consistent state store'

$list = Invoke-Msixpkg @('list', '--json')
Check ($list.Output -match [regex]::Escape($state.installId)) 'list includes the new installation'

# ---------------------------------------------------------------------------
# 4. Launch the app
# ---------------------------------------------------------------------------
Write-Host ''
Write-Host '4. the installed app starts'

$appId = $null
$executableName = $null
if ($package) {
    $manifestPath = Join-Path $package.InstallLocation 'AppxManifest.xml'
    if (Test-Path $manifestPath) {
        [xml]$appxManifest = Get-Content $manifestPath -Raw
        $application = $appxManifest.Package.Applications.Application | Select-Object -First 1
        if ($application) {
            $appId = $application.Id
            $executableName = [IO.Path]::GetFileNameWithoutExtension($application.Executable)
        }
    }
}

if (-not $SkipLaunch -and $appId -and $executableName) {
    $aumid = $script:installedFamilyName + '!' + $appId
    Start-Process ('shell:AppsFolder\' + $aumid) | Out-Null
    $deadline = (Get-Date).AddSeconds(20)
    $running = $false
    while ((Get-Date) -lt $deadline) {
        if (Get-Process -Name $executableName -ErrorAction SilentlyContinue) { $running = $true; break }
        Start-Sleep -Milliseconds 500
    }

    Check $running ('the app process appeared: ' + $executableName)
    if ($running) {
        # Close it again so the uninstall is not fighting a running app.
        Get-Process -Name $executableName -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
        Start-Sleep -Seconds 2
    }
} else {
    Write-Host '  SKIP  no Application Id / Executable in the Appx manifest (or -SkipLaunch)'
}

# ---------------------------------------------------------------------------
# 5. Uninstall
# ---------------------------------------------------------------------------
Write-Host ''
Write-Host '5. uninstall: the package goes first, then the certificate'

$uninstall = Invoke-Msixpkg @('uninstall', '--family-name', $script:installedFamilyName, '--yes', '--json')
$uninstallJson = $null
try { $uninstallJson = $uninstall.Output | ConvertFrom-Json } catch { }
Check ($uninstall.ExitCode -eq 0) 'uninstall exits 0'
Check ($null -ne $uninstallJson -and $uninstallJson.removed) 'the package was removed'

if ($null -ne $uninstallJson) {
    Write-Host ("  certificate decision: " + $uninstallJson.certificate.reason)
    Check ([bool]$uninstallJson.certificate.remove) 'the certificate was eligible for reclamation'
    if ($uninstallJson.certificate.repositoryUnavailable) {
        Write-Host '  NOTE  the package repository could not be enumerated, so the certificate is kept (conservative)'
    } elseif ([bool]$uninstallJson.certificate.otherPackagesUsePublisher) {
        Write-Host '  NOTE  another registered package uses this publisher; the certificate is kept'
    } else {
        Check ([bool]$uninstallJson.certificateRemoved) 'the certificate was reclaimed from TrustedPeople'
    }
}

Check ($null -eq (Get-InstalledPackage $script:installedFamilyName)) 'Get-AppxPackage no longer finds the package'
if (-not $uninstallJson.certificate.repositoryUnavailable -and
    -not $uninstallJson.certificate.otherPackagesUsePublisher) {
    Check ($null -eq (Get-TrustedCertificate $script:installedCertificateSha1)) 'the certificate is gone from TrustedPeople'
}

Check (-not (Test-ArpEntry $script:arpKeyName)) 'the "Apps & features" entry was removed'
Check (-not (Test-Path $stateFile)) 'the installation record was deleted'

# ---------------------------------------------------------------------------
# 6. Idempotency
# ---------------------------------------------------------------------------
Write-Host ''
Write-Host '6. uninstalling twice is not an error'

$again = Invoke-Msixpkg @('uninstall', '--family-name', $script:installedFamilyName, '--yes', '--json')
Check ($again.ExitCode -eq 0) 'a second uninstall exits 0'
Check ($again.Output -match '"removed":\s*false') 'the second uninstall reports "nothing to remove"'

# ---------------------------------------------------------------------------
# 7. Manual fallback with a deleted state store (T4 acceptance 3)
# ---------------------------------------------------------------------------
Write-Host ''
Write-Host '7. fallback: uninstall by family name + certificate thumbprint still works'
$fallback = Invoke-Msixpkg @('uninstall', '--family-name', $script:installedFamilyName,
                             '--cert-thumbprint', $script:installedCertificateSha1, '--dry-run')
Check ($fallback.ExitCode -eq 0) 'the documented fallback path is accepted'
Check ($fallback.Output -match 'certificate') 'its plan mentions the certificate decision'

# ---------------------------------------------------------------------------
# Cleanup + summary
# ---------------------------------------------------------------------------
if (-not $KeepInstalled -and $script:installedFamilyName) {
    $leftover = Get-InstalledPackage $script:installedFamilyName
    if ($leftover) {
        Write-Host ''
        Write-Host 'cleanup: removing a package left behind by a failed assertion'
        Remove-AppxPackage -Package $leftover.PackageFullName -ErrorAction SilentlyContinue
    }

    if ($script:installedCertificateSha1) {
        $cert = Get-TrustedCertificate $script:installedCertificateSha1
        if ($cert) {
            Write-Host 'cleanup: removing a certificate left behind by a failed assertion'
            Remove-Item ("Cert:\LocalMachine\TrustedPeople\" + $cert.Thumbprint) -Force -ErrorAction SilentlyContinue
        }
    }

    if ($script:arpKeyName) {
        Remove-Item ("HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\" + $script:arpKeyName) -Recurse -Force -ErrorAction SilentlyContinue
        Remove-Item ("HKCU:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\" + $script:arpKeyName) -Recurse -Force -ErrorAction SilentlyContinue
    }
}

Write-Host ''
Write-Host ("checks " + $script:checks + ", failures " + $script:failures)
if ($script:failures -gt 0) {
    Write-Host 'full E2E FAILED' -ForegroundColor Red
    exit 1
}

Write-Host 'full E2E passed' -ForegroundColor Green
exit 0
