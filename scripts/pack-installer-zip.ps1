<#
.SYNOPSIS
    把分发安装包与面向最终用户的 README 打成一个 zip。

.DESCRIPTION
    产物：artifacts\setup\MsixpkgInstaller-<版本>-dist.zip
    内容：
        MsixpkgInstaller-<版本>-setup.exe
        MsixpkgInstaller-<版本>-setup.exe.sha256
        README.md                     ← 由 installer\setup\README-dist.md 渲染

    版本号从 src\MsixpkgInstaller.Native\res\msixpkg.rc 的 FILEVERSION 读取，
    与 scripts\build-setup.ps1 共用同一个事实来源。

    README 模板里的 {{VERSION}} / {{SETUP_FILE}} / {{SHA256}} 会被实际值替换；
    渲染后还会断言 README 里确实包含「管理员批准」的安装说明 —— 这是这个
    分发 zip 的硬性要求，缺了就失败，不要靠人工记得。

.PARAMETER SetupPath
    安装包路径。默认 artifacts\setup\MsixpkgInstaller-<版本>-setup.exe。

.PARAMETER OutputDir
    输出目录，默认 artifacts\setup。

.PARAMETER ReadmeTemplate
    README 模板路径，默认 installer\setup\README-dist.md。

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File scripts\pack-installer-zip.ps1

.EXAMPLE
    # 指定另一个已构建的安装包
    powershell -ExecutionPolicy Bypass -File scripts\pack-installer-zip.ps1 -SetupPath artifacts\setup\MsixpkgInstaller-0.3.0-setup.exe
#>
[CmdletBinding()]
param(
    [string]$SetupPath,
    [string]$OutputDir,
    [string]$ReadmeTemplate
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
$rcPath = Join-Path $repoRoot 'src\MsixpkgInstaller.Native\res\msixpkg.rc'
if (-not (Test-Path -LiteralPath $rcPath)) {
    throw "找不到版本来源：$rcPath"
}

# 版本号的单一事实来源（与 build-setup.ps1 相同）。
$rcText = [IO.File]::ReadAllText($rcPath, (New-Object Text.UTF8Encoding($false)))
if ($rcText -notmatch 'FILEVERSION\s+(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)') {
    throw "在 $rcPath 里找不到 FILEVERSION x, y, z, w"
}
$productVersion = "$($Matches[1]).$($Matches[2]).$($Matches[3])"

if (-not $OutputDir) {
    $OutputDir = Join-Path $repoRoot 'artifacts\setup'
}
$OutputDir = [IO.Path]::GetFullPath($OutputDir)
if (-not (Test-Path -LiteralPath $OutputDir)) {
    New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null
}

if (-not $SetupPath) {
    $SetupPath = Join-Path $OutputDir "MsixpkgInstaller-$productVersion-setup.exe"
}
$SetupPath = [IO.Path]::GetFullPath($SetupPath)
if (-not (Test-Path -LiteralPath $SetupPath)) {
    throw "找不到安装包：$SetupPath`n先构建：powershell -ExecutionPolicy Bypass -File scripts\build-setup.ps1"
}
$setupName = [IO.Path]::GetFileName($SetupPath)

if (-not $ReadmeTemplate) {
    $ReadmeTemplate = Join-Path $repoRoot 'installer\setup\README-dist.md'
}
if (-not (Test-Path -LiteralPath $ReadmeTemplate)) {
    throw "找不到 README 模板：$ReadmeTemplate"
}

# ---------------------------------------------------------------------------
# 1) 安装包摘要：并校验旁边已有的 .sha256 不是旧版残留
# ---------------------------------------------------------------------------
$hash = (Get-FileHash -Algorithm SHA256 -Path $SetupPath).Hash
$hashFile = "$SetupPath.sha256"
$hashLine = "$hash *$setupName"
if (Test-Path -LiteralPath $hashFile) {
    $recorded = (Get-Content -LiteralPath $hashFile -Raw).Trim()
    if ($recorded -ne $hashLine) {
        throw "摘要文件与安装包不一致：$hashFile`n实际：$hashLine`n文件：$recorded`n重新构建安装包，或删除过期的 .sha256 后重试。"
    }
}
else {
    $hashLine | Set-Content -LiteralPath $hashFile -Encoding ASCII
}

# ---------------------------------------------------------------------------
# 2) 渲染 README：版本、安装包文件名、SHA-256 全部来自实际产物
# ---------------------------------------------------------------------------
$template = [IO.File]::ReadAllText($ReadmeTemplate, (New-Object Text.UTF8Encoding($false)))
$readme = $template.Replace('{{VERSION}}', $productVersion).Replace('{{SETUP_FILE}}', $setupName).Replace('{{SHA256}}', $hash)
if ($readme -match '\{\{[A-Z0-9_]+\}\}') {
    throw "README 模板里还有未替换的占位符：$($Matches[0])"
}
if ($readme -notmatch '管理员批准') {
    throw 'README 模板没有说明「为所有用户安装会请求管理员批准」——这是分发 zip 的硬性要求。'
}

# ---------------------------------------------------------------------------
# 3) 组装并压缩
# ---------------------------------------------------------------------------
$stageDir = Join-Path $repoRoot 'artifacts\temp\installer-zip-stage'
$zipName = "MsixpkgInstaller-$productVersion-dist.zip"
$zipPath = Join-Path $OutputDir $zipName

try {
    if (Test-Path -LiteralPath $stageDir) {
        Remove-Item -LiteralPath $stageDir -Recurse -Force
    }
    New-Item -ItemType Directory -Force -Path $stageDir | Out-Null
    Copy-Item -LiteralPath $SetupPath -Destination (Join-Path $stageDir $setupName)
    Copy-Item -LiteralPath $hashFile -Destination (Join-Path $stageDir "$setupName.sha256")

    # UTF-8 with BOM：Windows PowerShell 5.1 与旧版记事本都能正确识别中文。
    [IO.File]::WriteAllText((Join-Path $stageDir 'README.md'), $readme, (New-Object Text.UTF8Encoding($true)))

    if (Test-Path -LiteralPath $zipPath) {
        Remove-Item -LiteralPath $zipPath -Force
    }
    Compress-Archive -Path (Join-Path $stageDir '*') -DestinationPath $zipPath -CompressionLevel Optimal
}
finally {
    if (Test-Path -LiteralPath $stageDir) {
        Remove-Item -LiteralPath $stageDir -Recurse -Force
    }
}

# ---------------------------------------------------------------------------
# 4) 自检：zip 里恰好是这三项，README 读回来仍有管理员说明
# ---------------------------------------------------------------------------
Add-Type -AssemblyName System.IO.Compression.FileSystem

$expected = @($setupName, "$setupName.sha256", 'README.md')
$archive = [IO.Compression.ZipFile]::OpenRead($zipPath)
try {
    $names = @($archive.Entries | ForEach-Object { $_.FullName })
    foreach ($name in $expected) {
        if ($names -notcontains $name) {
            throw "zip 里缺少 $name（实际：$($names -join ', ')）"
        }
    }
    if ($names.Count -ne $expected.Count) {
        throw "zip 里有多余条目：$($names -join ', ')"
    }

    $entry = $archive.GetEntry('README.md')
    $reader = New-Object IO.StreamReader($entry.Open(), (New-Object Text.UTF8Encoding($false)), $true)
    try {
        $readmeInZip = $reader.ReadToEnd()
    }
    finally {
        $reader.Dispose()
    }
}
finally {
    $archive.Dispose()
}

if ($readmeInZip -notmatch '管理员批准') {
    throw 'zip 里的 README.md 没有「管理员批准」说明'
}
if ($readmeInZip -notmatch [regex]::Escape($hash)) {
    throw 'zip 里的 README.md 没有写入安装包的实际 SHA-256'
}

$zipHash = (Get-FileHash -Algorithm SHA256 -Path $zipPath).Hash
"$zipHash *$zipName" | Set-Content -LiteralPath "$zipPath.sha256" -Encoding ASCII

$zipMb = [Math]::Round((Get-Item -LiteralPath $zipPath).Length / 1MB, 2)
Write-Host ''
Write-Host "分发 zip：$zipPath"
Write-Host "      版本 $productVersion"
Write-Host "      内容 $setupName + $setupName.sha256 + README.md（含 UAC 说明）"
Write-Host "      大小 $zipMb MB"
Write-Host "      SHA-256 $zipHash"
Write-Host "      摘要文件 $zipPath.sha256"
