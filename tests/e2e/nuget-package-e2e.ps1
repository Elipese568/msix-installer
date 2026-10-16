<#
.SYNOPSIS
    Accept the Msixpkg.Build NuGet package: layout, PackageReference consumption, byte-identical output.

.DESCRIPTION
    This script answers "now that the build-time packer is a NuGet package, can a consumer actually use it?"
    It deliberately stays clear of every in-repo project reference and goes through NuGet only:

      1) Packs via scripts\pack-nuget.ps1, then asserts the package contents as a contract:
         build\Msixpkg.Build.targets, tasks\netstandard2.0\{Msixpkg.Tasks.dll, Msixpkg.Core.dll},
         LICENSE, PACKAGE.md; no lib\, no dependencies, MIT license expression, and the bundled
         Msixpkg.Core.dll must be byte-identical to the one just built in the repo.
      2) Restores + builds samples\MsixpkgPackageSample through PackageReference, once per MSBuild host:
           * the .NET SDK MSBuild (dotnet build)
           * the .NET Framework MSBuild from Visual Studio (MSBuild.exe) - only when one is found
         Every host run starts from an EMPTY package cache, which proves the package alone is enough.
      3) Asserts the produced .msixpkg is byte-identical to the golden vector in tests\vectors
         (the sample pins MsixpkgCreatedUtc), and that every entry is stored (method 0).

    Needs no administrator, writes nothing outside the repository, never touches a certificate store.

.NOTES
    ASCII only, on purpose: see the pitfalls section of tests\e2e\README.md (a UTF-8 BOM on a .ps1 file
    in this directory gets stripped by editors, and Windows PowerShell 5.1 then decodes the file as ANSI).

.PARAMETER Configuration
    Build configuration, Release by default.

.PARAMETER SkipPack
    Skip packing and use whatever package is already in OutputDir (handy while iterating).

.PARAMETER OutputDir
    Directory holding the package; defaults to artifacts\nuget.

.PARAMETER VsMsBuildPath
    Full path to the Visual Studio MSBuild.exe. Discovered with vswhere when omitted.

.PARAMETER RequireVsMsBuild
    Fail when the Visual Studio host is missing or unusable (CI turns this on).

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tests\e2e\nuget-package-e2e.ps1
#>
[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [switch]$SkipPack,
    [string]$OutputDir,
    [string]$VsMsBuildPath,
    [switch]$RequireVsMsBuild
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$sampleProject = Join-Path $repoRoot 'samples\MsixpkgPackageSample\MsixpkgPackageSample.csproj'
$sampleArtifacts = Join-Path $repoRoot 'samples\MsixpkgPackageSample\artifacts'
$goldenPath = Join-Path $repoRoot 'tests\vectors\Msixpkg.Sample_1.0.0.0_x64.msixpkg'
$containerName = 'Msixpkg.Sample_1.0.0.0_x64.msixpkg'
$scratchRoot = Join-Path $repoRoot 'artifacts\temp\nuget-e2e'
$localPackages = Join-Path $scratchRoot 'packages'
$repositoryCoreDll = Join-Path $repoRoot "src\Msixpkg.Core\bin\$Configuration\netstandard2.0\Msixpkg.Core.dll"

if (-not $OutputDir) {
    $OutputDir = Join-Path $repoRoot 'artifacts\nuget'
}

$script:failures = New-Object System.Collections.Generic.List[string]
$script:checks = 0

function Assert-True {
    param([bool]$Condition, [string]$Message)

    $script:checks++
    if ($Condition) {
        Write-Host "  [ok]   $Message"
    } else {
        Write-Host "  [FAIL] $Message" -ForegroundColor Red
        $script:failures.Add($Message)
    }
}

function Write-Section {
    param([string]$Title)

    Write-Host ''
    Write-Host "=== $Title ===" -ForegroundColor Cyan
}

function Get-ContainerEntryMethods {
    param([string]$Path)

    # Walk the local file headers only (PK\3\4): entry data follows the header, so skip it by sizes.
    # The compression method lives at offset 8.
    $bytes = [System.IO.File]::ReadAllBytes($Path)
    $methods = New-Object System.Collections.Generic.List[int]
    $cursor = 0
    while ($cursor -le $bytes.Length - 4) {
        if ($bytes[$cursor] -eq 0x50 -and $bytes[$cursor + 1] -eq 0x4B -and
            $bytes[$cursor + 2] -eq 0x03 -and $bytes[$cursor + 3] -eq 0x04) {
            $methods.Add($bytes[$cursor + 8] -bor ($bytes[$cursor + 9] -shl 8))
            $nameLength = $bytes[$cursor + 26] -bor ($bytes[$cursor + 27] -shl 8)
            $extraLength = $bytes[$cursor + 28] -bor ($bytes[$cursor + 29] -shl 8)
            $size = [BitConverter]::ToUInt32($bytes, $cursor + 18)
            $cursor += 30 + $nameLength + $extraLength + $size
        } else {
            $cursor++
        }
    }

    return $methods
}

Add-Type -AssemblyName System.IO.Compression.FileSystem

# ---------------------------------------------------------------------------
# 1) Pack
# ---------------------------------------------------------------------------
if (-not $SkipPack) {
    Write-Section 'pack Msixpkg.Build'
    $packScript = Join-Path $repoRoot 'scripts\pack-nuget.ps1'
    & powershell -NoProfile -ExecutionPolicy Bypass -File $packScript -Configuration $Configuration
    if ($LASTEXITCODE -ne 0) {
        throw "packing failed (scripts\pack-nuget.ps1 exit code $LASTEXITCODE)"
    }
}

$packages = @(Get-ChildItem -Path $OutputDir -Filter 'Msixpkg.Build.*.nupkg' -File | Sort-Object LastWriteTime -Descending)
if ($packages.Count -eq 0) {
    throw "no Msixpkg.Build.*.nupkg under $OutputDir (run scripts\pack-nuget.ps1 first, or drop -SkipPack)"
}

$packagePath = $packages[0].FullName
Write-Host "package: $packagePath"
Write-Host "SHA-256 $((Get-FileHash -Algorithm SHA256 -Path $packagePath).Hash)"

# ---------------------------------------------------------------------------
# 2) Layout and metadata
# ---------------------------------------------------------------------------
Write-Section 'package layout and metadata'

$zip = [System.IO.Compression.ZipFile]::OpenRead($packagePath)
try {
    $entries = @{}
    foreach ($entry in $zip.Entries) {
        $entries[$entry.FullName] = $entry
    }

    $expectedEntries = @(
        'build/Msixpkg.Build.targets'
        'tasks/netstandard2.0/Msixpkg.Tasks.dll'
        'tasks/netstandard2.0/Msixpkg.Core.dll'
        'LICENSE'
        'PACKAGE.md'
    )

    foreach ($expected in $expectedEntries) {
        Assert-True ($entries.ContainsKey($expected)) "contains $expected"
    }

    $unexpected = @($entries.Keys | Where-Object {
        $_ -notin $expectedEntries -and
        $_ -ne 'Msixpkg.Build.nuspec' -and
        $_ -ne '[Content_Types].xml' -and
        $_ -notlike '_rels/*' -and
        $_ -notlike 'package/services/metadata/*'
    })
    Assert-True ($unexpected.Count -eq 0) "no unexpected entries (found: $($unexpected -join ', '))"
    Assert-True (@($entries.Keys | Where-Object { $_ -like 'lib/*' }).Count -eq 0) 'no lib/ (a task package offers no compile reference)'
    Assert-True (@($entries.Keys | Where-Object { $_ -like 'buildTransitive/*' }).Count -eq 0) 'no buildTransitive/ (consumers reference this package directly)'

    # The bundled binary must be the one just built (a stale Msixpkg.Core.dll would be invisible otherwise).
    $coreEntry = $entries['tasks/netstandard2.0/Msixpkg.Core.dll']
    $coreStream = $coreEntry.Open()
    $coreMemory = New-Object System.IO.MemoryStream
    $coreStream.CopyTo($coreMemory)
    $coreStream.Dispose()
    $coreBytes = $coreMemory.ToArray()
    $coreMemory.Dispose()
    $sha = [System.Security.Cryptography.SHA256]::Create()
    $packagedCoreHash = ([BitConverter]::ToString($sha.ComputeHash($coreBytes)) -replace '-', '')
    if (Test-Path $repositoryCoreDll) {
        $builtCoreHash = (Get-FileHash -Algorithm SHA256 -Path $repositoryCoreDll).Hash
        Assert-True ($packagedCoreHash -eq $builtCoreHash) 'bundled Msixpkg.Core.dll is byte-identical to the freshly built netstandard2.0 output'
    } else {
        Write-Host "  [warn] $repositoryCoreDll not found, skipping the 'bundled binary is up to date' check" -ForegroundColor Yellow
    }

    $nuspecStream = $entries['Msixpkg.Build.nuspec'].Open()
    $nuspecReader = New-Object System.IO.StreamReader($nuspecStream)
    $nuspec = $nuspecReader.ReadToEnd()
    $nuspecReader.Dispose()

    Assert-True ($nuspec -match '<id>Msixpkg\.Build</id>') 'nuspec id is Msixpkg.Build'
    Assert-True ($nuspec -match '<license type="expression">MIT</license>') 'license is the MIT SPDX expression'
    Assert-True ($nuspec -match '<readme>PACKAGE\.md</readme>') 'nuspec points at the packaged README'
    Assert-True ($nuspec -match 'github\.com/Elipese568/msix-installer') 'nuspec repository url points at Elipese568/msix-installer'
    Assert-True ($nuspec -notmatch '<dependencies>') 'no NuGet dependencies (Core is a bundled binary, not a dependency)'
} finally {
    $zip.Dispose()
}

# ---------------------------------------------------------------------------
# 3) Consumer view: restore + build through PackageReference
# ---------------------------------------------------------------------------
Write-Section 'consumer sample (PackageReference)'

$sampleText = Get-Content -Path $sampleProject -Raw
Assert-True ($sampleText -notmatch '<Import\s') 'the sample imports no targets (it relies on the packaged build\)'
Assert-True ($sampleText -notmatch '<ProjectReference') 'the sample has no ProjectReference into this repository'

if (-not (Test-Path $goldenPath)) {
    throw "golden vector not found: $goldenPath"
}
$goldenHash = (Get-FileHash -Algorithm SHA256 -Path $goldenPath).Hash
Write-Host "golden vector SHA-256: $goldenHash"

# MSBuild worker nodes and the Roslyn compiler server use named pipes, which locked-down
# environments (sandboxes, containers, some CI policies) refuse. These switches avoid both.
$msbuildArguments = @('-m:1', '-nodeReuse:false', '-p:UseSharedCompilation=false')

$previousNugetScratch = $env:NUGET_SCRATCH
$previousNugetPackages = $env:NUGET_PACKAGES

function Test-SampleOutput {
    param([string]$HostLabel, [bool]$Required, [int]$ExitCode)

    $outputPath = Join-Path $sampleArtifacts $containerName

    if ($ExitCode -ne 0 -or -not (Test-Path $outputPath)) {
        if ($Required) {
            Assert-True $false "$HostLabel built and produced $containerName (exit code $ExitCode)"
        } else {
            Write-Host "  [skip] $HostLabel unusable (exit code $ExitCode)" -ForegroundColor Yellow
        }

        return
    }

    Assert-True $true "$HostLabel built and produced $containerName"

    $hash = (Get-FileHash -Algorithm SHA256 -Path $outputPath).Hash
    Assert-True ($hash -eq $goldenHash) "$HostLabel output is byte-identical to the golden vector ($hash)"

    $methods = @(Get-ContainerEntryMethods -Path $outputPath)
    Assert-True ($methods.Count -eq 5) "$HostLabel output has 5 entries (found $($methods.Count))"
    Assert-True (@($methods | Where-Object { $_ -ne 0 }).Count -eq 0) "$HostLabel output stores every entry (method 0), found: $($methods -join ', ')"
}

try {
    New-Item -ItemType Directory -Force -Path $scratchRoot | Out-Null
    $env:NUGET_SCRATCH = Join-Path $scratchRoot 'scratch'

    # --- host 1: the .NET SDK MSBuild ---
    Remove-Item $sampleArtifacts -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item $localPackages -Recurse -Force -ErrorAction SilentlyContinue
    $env:NUGET_PACKAGES = $localPackages

    Write-Host ''
    Write-Host 'host 1: dotnet build (.NET SDK MSBuild)'
    & dotnet build $sampleProject -c $Configuration --nologo @msbuildArguments
    Test-SampleOutput -HostLabel 'dotnet build' -Required $true -ExitCode $LASTEXITCODE

    # The restored package must have come from the local feed (proof that the package, not the repo, was used).
    $extractedCore = Get-ChildItem -Path $localPackages -Filter 'Msixpkg.Core.dll' -Recurse -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -match 'msixpkg\.build' } | Select-Object -First 1
    Assert-True ($null -ne $extractedCore) 'the local package cache contains Msixpkg.Core.dll extracted from the nupkg'

    # --- host 2: the .NET Framework MSBuild from Visual Studio ---
    if (-not $VsMsBuildPath) {
        $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
        if (Test-Path $vswhere) {
            $found = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' 2>$null
            if ($found) {
                $VsMsBuildPath = @($found)[0].Trim()
            }
        }
    }

    Write-Host ''
    if ($VsMsBuildPath -and (Test-Path $VsMsBuildPath)) {
        Write-Host "host 2: $VsMsBuildPath (.NET Framework MSBuild)"
        Remove-Item $sampleArtifacts -Recurse -Force -ErrorAction SilentlyContinue
        Remove-Item $localPackages -Recurse -Force -ErrorAction SilentlyContinue

        # Restore and build MUST be separate invocations: the package's build\*.targets are imported
        # during evaluation, from obj\*.nuget.g.targets, and that file is produced by Restore.
        # A single /t:Restore,Rebuild does not re-evaluate, so it silently skips the import and
        # reports success without producing a .msixpkg. Classic NuGet + MSBuild trap.
        & $VsMsBuildPath $sampleProject /t:Restore "/p:Configuration=$Configuration" /nologo /v:m /nodeReuse:false /p:UseSharedCompilation=false
        $buildExitCode = $LASTEXITCODE
        if ($buildExitCode -eq 0) {
            & $VsMsBuildPath $sampleProject /t:Rebuild "/p:Configuration=$Configuration" /nologo /v:m /nodeReuse:false /p:UseSharedCompilation=false
            $buildExitCode = $LASTEXITCODE
        }

        Test-SampleOutput -HostLabel 'MSBuild.exe' -Required $true -ExitCode $buildExitCode
    } else {
        $explicitlyRequested = $PSBoundParameters.ContainsKey('VsMsBuildPath')
        if ($RequireVsMsBuild -or $explicitlyRequested) {
            Assert-True $false "the Visual Studio MSBuild.exe was required but not found: $VsMsBuildPath"
        } else {
            Write-Host '  [skip] no Visual Studio MSBuild.exe found, skipping the .NET Framework host' -ForegroundColor Yellow
        }
    }
} finally {
    $env:NUGET_SCRATCH = $previousNugetScratch
    $env:NUGET_PACKAGES = $previousNugetPackages
}

# ---------------------------------------------------------------------------
# 4) Summary
# ---------------------------------------------------------------------------
Write-Host ''
if ($script:failures.Count -eq 0) {
    Write-Host "NuGet package accepted: $script:checks checks passed." -ForegroundColor Green
    exit 0
}

Write-Host "NuGet package rejected: $($script:failures.Count) of $script:checks checks failed." -ForegroundColor Red
foreach ($failure in $script:failures) {
    Write-Host "  - $failure" -ForegroundColor Red
}

exit 1
