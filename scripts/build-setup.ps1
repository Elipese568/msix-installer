<#
.SYNOPSIS
    Build the Msixpkg distribution setup (msixpkg-setup.exe) and the portable zip.

.DESCRIPTION
    The setup is a native Wren/UI process plus a self-appended payload:
      * msixpkg-installer.exe  (the wizard)
      * msixpkg-native.exe     (the CLI)
      * Uninstall.exe          (built from setup/uninstall_main.cpp)
    Packing appends [payload][index][80-byte footer] to the thin setup exe produced
    by CMake. The exe reads its own payload, so it needs no writable %TEMP% and no
    external archive extraction.

    The script then verifies the packed exe with `--verify-payload` before writing
    the .sha256 sidecar.

    Signing happens *after* packing: Authenticode covers the whole file, so signing
    the thin exe first would be invalidated by the appended payload.

.PARAMETER Configuration
    Payload configuration: Release (default) or Debug.

.PARAMETER PayloadDir
    Directory with msixpkg-setup.exe / msixpkg-uninstall.exe / msixpkg-installer.exe /
    msixpkg-native.exe. Defaults to the CMakePresets output.

.PARAMETER OutputDir
    Artifact directory. Defaults to artifacts\setup.

.PARAMETER BuildNative
    Run `cmake --preset windows-x64` and `cmake --build --preset windows-x64-<cfg>`
    first.

.PARAMETER Sign / PfxPath / PfxPassword / TimestampUrl
    Optional signtool step.

.PARAMETER SkipPortable
    Do not create the portable zip.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File scripts\build-setup.ps1

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File scripts\build-setup.ps1 -BuildNative -Sign -PfxPath certs\release.pfx -PfxPassword ***
#>
[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [string]$PayloadDir,
    [string]$OutputDir,
    [switch]$BuildNative,
    [switch]$SkipPortable,
    [switch]$Sign,
    [string]$PfxPath,
    [string]$PfxPassword,
    [string]$TimestampUrl = 'http://timestamp.digicert.com'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
$nativeDir = Join-Path $repoRoot 'src\MsixpkgInstaller.Native'
$rcPath = Join-Path $nativeDir 'res\msixpkg.rc'

function ConvertTo-AbsolutePath {
    param([string]$Path)
    if ([string]::IsNullOrEmpty($Path)) { return $Path }
    if ([System.IO.Path]::IsPathRooted($Path)) { return [System.IO.Path]::GetFullPath($Path) }
    return [System.IO.Path]::GetFullPath((Join-Path (Get-Location).Path $Path))
}

if ($PayloadDir) { $PayloadDir = ConvertTo-AbsolutePath $PayloadDir }
if ($OutputDir) { $OutputDir = ConvertTo-AbsolutePath $OutputDir }
if (-not $OutputDir) { $OutputDir = Join-Path $repoRoot 'artifacts\setup' }

# ---------------------------------------------------------------------------
# 0) Version: single source of truth is res/msixpkg.rc FILEVERSION.
# ---------------------------------------------------------------------------
$rcText = [IO.File]::ReadAllText($rcPath, (New-Object Text.UTF8Encoding($false)))
if ($rcText -notmatch 'FILEVERSION\s+(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)') {
    throw "No FILEVERSION x, y, z, w in $rcPath"
}
$productVersion = "$($Matches[1]).$($Matches[2]).$($Matches[3])"
$productVersionQuad = "$($Matches[1]).$($Matches[2]).$($Matches[3]).$($Matches[4])"

# ---------------------------------------------------------------------------
# 1) Native payload
# ---------------------------------------------------------------------------
if ($BuildNative) {
    $preset = "windows-x64-$($Configuration.ToLowerInvariant())"
    Write-Host "Building native payload (cmake --preset windows-x64 / --build --preset $preset)..."
    Push-Location $nativeDir
    try {
        & cmake --preset windows-x64 -S . | Out-Host
        if ($LASTEXITCODE -ne 0) { throw "cmake configure failed ($LASTEXITCODE)" }
        & cmake --build --preset $preset --parallel | Out-Host
        if ($LASTEXITCODE -ne 0) { throw "cmake build failed ($LASTEXITCODE)" }
    }
    finally {
        Pop-Location
    }
}

if (-not $PayloadDir) {
    $PayloadDir = Join-Path $nativeDir "build\x64\$Configuration"
}

$thinSetup = Join-Path $PayloadDir 'msixpkg-setup.exe'
$uninstaller = Join-Path $PayloadDir 'msixpkg-uninstall.exe'
$gui = Join-Path $PayloadDir 'msixpkg-installer.exe'
$cli = Join-Path $PayloadDir 'msixpkg-native.exe'
foreach ($required in @($thinSetup, $uninstaller, $gui, $cli)) {
    if (-not (Test-Path -LiteralPath $required)) {
        throw "Missing $required`nBuild the native project first: cmake --preset windows-x64 -S src\MsixpkgInstaller.Native; cmake --build --preset windows-x64-$($Configuration.ToLowerInvariant())"
    }
}

New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null
$outputName = "MsixpkgInstaller-$productVersion-setup.exe"
$setupPath = Join-Path $OutputDir $outputName

# ---------------------------------------------------------------------------
# 2) Pack: [thin exe][payload files][index][footer]
# ---------------------------------------------------------------------------
# The format is mirrored by setup/setup_payload.h; keep both sides in sync.
$entries = @(
    @{ Name = 'msixpkg-installer.exe'; Path = $gui },
    @{ Name = 'msixpkg-native.exe'; Path = $cli },
    @{ Name = 'Uninstall.exe'; Path = $uninstaller }
)

$sha = [Security.Cryptography.SHA256]::Create()
try {
    if (Test-Path -LiteralPath $setupPath) { Remove-Item -LiteralPath $setupPath -Force }
    $stream = [IO.File]::Open($setupPath, [IO.FileMode]::Create, [IO.FileAccess]::ReadWrite)
    $writer = New-Object IO.BinaryWriter($stream)
    try {
        $writer.Write([IO.File]::ReadAllBytes($thinSetup))
        $index = New-Object System.Collections.Generic.List[object]
        $totalPayload = [uint64]0
        foreach ($entry in $entries) {
            $bytes = [IO.File]::ReadAllBytes($entry.Path)
            $offset = [uint64]$stream.Position
            $writer.Write($bytes)
            $hash = $sha.ComputeHash($bytes)
            $index.Add([pscustomobject]@{
                    Name   = [string]$entry.Name
                    Offset = $offset
                    Size   = [uint64]$bytes.Length
                    Hash   = $hash
                })
            $totalPayload += [uint64]$bytes.Length
            Write-Host ("  payload {0} ({1:N0} bytes) @ {2}" -f $entry.Name, $bytes.Length, $offset)
        }

        $indexOffset = [uint64]$stream.Position
        $indexStream = New-Object IO.MemoryStream
        $indexWriter = New-Object IO.BinaryWriter($indexStream)
        try {
            $indexWriter.Write([uint32]$index.Count)
            foreach ($row in $index) {
                $nameBytes = [Text.Encoding]::UTF8.GetBytes($row.Name)
                $indexWriter.Write([uint16]$nameBytes.Length)
                $indexWriter.Write($nameBytes)
                $indexWriter.Write([uint64]$row.Offset)
                $indexWriter.Write([uint64]$row.Size)
                $indexWriter.Write([byte[]]$row.Hash)
            }
            $indexWriter.Flush()
            $indexBytes = $indexStream.ToArray()
        }
        finally {
            $indexWriter.Dispose()
            $indexStream.Dispose()
        }

        $writer.Write($indexBytes)
        $indexHash = $sha.ComputeHash($indexBytes)

        $writer.Write([Text.Encoding]::ASCII.GetBytes('MSXSETUP'))
        $writer.Write([uint32]1)
        $writer.Write([uint32]0)
        $writer.Write([uint64]$indexOffset)
        $writer.Write([uint64]$indexBytes.Length)
        $writer.Write([uint64]$totalPayload)
        $writer.Write([uint64]0)
        $writer.Write([byte[]]$indexHash)
        $writer.Flush()
        $stream.Flush($true)
    }
    finally {
        $writer.Dispose()
        $stream.Dispose()
    }
}
finally {
    $sha.Dispose()
}

# ---------------------------------------------------------------------------
# 3) Verify the packed image with the installer's own parser.
# ---------------------------------------------------------------------------
$verify = Start-Process -FilePath $setupPath -ArgumentList @('--verify-payload', '/S') -PassThru -Wait
if ($verify.ExitCode -ne 0) {
    throw "Packed setup failed its own --verify-payload check (exit $($verify.ExitCode)): $setupPath"
}
Write-Host '  payload self-check: ok'

# ---------------------------------------------------------------------------
# 4) Optional signing (must be after packing)
# ---------------------------------------------------------------------------
if ($Sign) {
    if (-not $PfxPath -or -not (Test-Path -LiteralPath $PfxPath)) {
        throw "-Sign needs -PfxPath pointing at a usable PFX."
    }
    $signtool = $null
    $kits = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\bin'
    if (Test-Path $kits) {
        $candidates = Get-ChildItem -Path $kits -Filter signtool.exe -Recurse -ErrorAction SilentlyContinue |
            Where-Object { $_.FullName -match '\\x64\\' } | Sort-Object FullName -Descending
        if ($candidates) { $signtool = @($candidates)[0].FullName }
    }
    if (-not $signtool) {
        throw 'signtool.exe not found; install the Windows SDK or sign the artifact manually.'
    }
    Write-Host "Signing ($signtool)..."
    $signArguments = @('sign', '/fd', 'SHA256', '/f', $PfxPath)
    if ($PfxPassword) { $signArguments += @('/p', $PfxPassword) }
    $signArguments += @('/tr', $TimestampUrl, '/td', 'SHA256', $setupPath)
    & $signtool @signArguments | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "signtool failed ($LASTEXITCODE)" }

    # 签名会在文件末尾追加证书表；再跑一次自检，证明 footer 不是「必须在 EOF-80」。
    $signedVerify = Start-Process -FilePath $setupPath -ArgumentList @('--verify-payload', '/S') -PassThru -Wait
    if ($signedVerify.ExitCode -ne 0) {
        throw "Signed setup failed its --verify-payload check (exit $($signedVerify.ExitCode))"
    }
    Write-Host '  payload self-check after signing: ok'
}

# ---------------------------------------------------------------------------
# 5) Checksums and portable zip
# ---------------------------------------------------------------------------
$hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $setupPath).Hash
$hashPath = "$setupPath.sha256"
"$hash *$outputName" | Set-Content -LiteralPath $hashPath -Encoding ASCII

$portablePath = $null
if (-not $SkipPortable) {
    $portableName = "MsixpkgInstaller-$productVersion-portable.zip"
    $portablePath = Join-Path $OutputDir $portableName
    $stageDir = Join-Path $repoRoot 'artifacts\temp\portable-stage'
    if (Test-Path -LiteralPath $stageDir) { Remove-Item -LiteralPath $stageDir -Recurse -Force }
    New-Item -ItemType Directory -Force -Path $stageDir | Out-Null
    Copy-Item -LiteralPath $gui -Destination (Join-Path $stageDir 'msixpkg-installer.exe')
    Copy-Item -LiteralPath $cli -Destination (Join-Path $stageDir 'msixpkg-native.exe')
    $portableReadme = @(
        "Msixpkg Installer $productVersion - portable copy"
        ""
        "msixpkg-installer.exe   The wizard: open a .msixpkg and install the app inside."
        "msixpkg-native.exe      The command line: install / uninstall / list / verify / repair / clean."
        ""
        "Usage"
        "  Keep both files in one folder and run msixpkg-installer.exe."
        "  Add that folder to PATH if you want msixpkg-native in any shell."
        ""
        "What this copy does NOT do (use the setup exe for those)"
        "  * no .msixpkg file association"
        "  * no Start menu or desktop shortcuts"
        "  * no Apps & features uninstall entry"
        "  Uninstalling is simply deleting the folder."
        ""
        "Why a portable copy exists"
        "  Containers, Windows Sandbox and locked-down machines. The setup exe already"
        "  needs no writable temp folder, but it still writes to the install folder and"
        "  the registry; this zip touches neither."
        ""
        "Documentation: installer/setup/README.md in the repository."
    )
    [IO.File]::WriteAllLines((Join-Path $stageDir 'README-portable.txt'), $portableReadme, (New-Object Text.UTF8Encoding($false)))
    if (Test-Path -LiteralPath $portablePath) { Remove-Item -LiteralPath $portablePath -Force }
    Compress-Archive -Path (Join-Path $stageDir '*') -DestinationPath $portablePath -CompressionLevel Optimal
    Remove-Item -LiteralPath $stageDir -Recurse -Force
    $portableHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $portablePath).Hash
    "$portableHash *$portableName" | Set-Content -LiteralPath "$portablePath.sha256" -Encoding ASCII
}

# ---------------------------------------------------------------------------
# 6) Report
# ---------------------------------------------------------------------------
$sizeMb = [Math]::Round((Get-Item -LiteralPath $setupPath).Length / 1MB, 2)
Write-Host ''
Write-Host "Setup: $setupPath"
Write-Host "       version $productVersion ($productVersionQuad), configuration $Configuration"
Write-Host "       size $sizeMb MB, SHA-256 $hash"
Write-Host "       checksum $hashPath"
if ($portablePath) {
    $portableMb = [Math]::Round((Get-Item -LiteralPath $portablePath).Length / 1MB, 2)
    Write-Host "Portable: $portablePath ($portableMb MB)"
    Write-Host "          SHA-256 $((Get-FileHash -Algorithm SHA256 -LiteralPath $portablePath).Hash)"
}
