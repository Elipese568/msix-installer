<#
.SYNOPSIS
    从 Windows SDK 的 winerror.h 生成部署错误码表（src/deployment_errors.generated.h）。

.DESCRIPTION
    安装 / 卸载走的是 Windows 部署栈（PackageManager），失败时返回的是扩展错误码
    （HRESULT）。要让用户看到「为什么失败、下一步做什么」，必须把这些数字翻译成人话。
    表本身不手抄：直接从 SDK 的 winerror.h 提取，保证「没漏码」，也保证名字与系统一致。

    winerror.h 里这三类定义形式都要处理：
      1. `#define ERROR_INSTALL_FAILED             15609L`            —— 十进制 Win32 码
      2. `#define APPX_E_PACKAGING_INTERNAL        _HRESULT_TYPEDEF_(0x80080200L)`
      3. `#define NAME  0x8007....L`                                  —— 裸十六进制

    十进制的是 Win32 错误码，部署栈返回的是 `HRESULT_FROM_WIN32(code)` = 0x80070000 | code，
    因此这里统一归一化成 32 位 HRESULT，方便运行时只做一次 switch。

.PARAMETER WinErrorHeader
    winerror.h 的路径。省略时自动挑选 SDK Include 目录下版本号最高的一个。

.PARAMETER OutputPath
    生成的头文件路径。省略时写到脚本所在仓库的 src/MsixpkgInstaller.Native/src/ 下。

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File scripts/generate-deployment-errors.ps1
#>
[CmdletBinding()]
param(
    [string]$WinErrorHeader,
    [string]$OutputPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Find-WinErrorHeader {
    $roots = @(
        (Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\Include'),
        (Join-Path $env:ProgramFiles 'Windows Kits\10\Include')
    ) | Where-Object { $_ -and (Test-Path $_) }

    $candidates = @()
    foreach ($root in $roots) {
        # winerror.h 在 shared\ 下（不是 um\ —— 交接文档里写错过一次）。
        $candidates += Get-ChildItem -Path $root -Directory -ErrorAction SilentlyContinue |
            ForEach-Object { Join-Path $_.FullName 'shared\winerror.h' } |
            Where-Object { Test-Path $_ }
    }

    if ($candidates.Count -eq 0) {
        throw 'winerror.h 未找到；请用 -WinErrorHeader 显式指定路径。'
    }

    # 目录名是版本号（10.0.26100.0），按版本排序取最高。
    $best = $candidates | Sort-Object -Property {
        $version = Split-Path (Split-Path (Split-Path $_ -Parent) -Parent) -Leaf
        try { [version]$version } catch { [version]'0.0.0.0' }
    } | Select-Object -Last 1

    return $best
}

if (-not $WinErrorHeader) {
    $WinErrorHeader = Find-WinErrorHeader
}

if (-not (Test-Path $WinErrorHeader)) {
    throw ("winerror.h 不存在: " + $WinErrorHeader)
}

if (-not $OutputPath) {
    $repoRoot = Split-Path $PSScriptRoot -Parent
    $OutputPath = Join-Path $repoRoot 'src\MsixpkgInstaller.Native\src\deployment_errors.generated.h'
}

# 要提取的前缀：部署栈直接返回的、以及「证书不受信」这一类最常见的失败。
$prefixes = @(
    'ERROR_INSTALL_',
    'ERROR_PACKAGE_',
    'APPX_',
    'TRUST_E_',
    'CERT_E_',
    'ERROR_DEPLOYMENT_'
)

# 三种定义形式统一抓出来。
$defineRegex = '^#define\s+(?<name>[A-Z][A-Z0-9_]*)\s+(?<value>_HRESULT_TYPEDEF_\(\s*0[xX][0-9A-Fa-f]+L?\s*\)|0[xX][0-9A-Fa-f]+L?|\d+L?)\s*(?://.*)?$'
$hexRegex = '0[xX](?<digits>[0-9A-Fa-f]+)'
$decRegex = '^(?<digits>\d+)L?$'
$typedefRegex = '_HRESULT_TYPEDEF_\('

# winerror.h 是纯 ASCII，按 ASCII 读，避免任何代码页参与。
$lines = [IO.File]::ReadAllLines($WinErrorHeader, [Text.Encoding]::ASCII)

$entries = New-Object 'System.Collections.Generic.Dictionary[uint32,string]'
$skipped = New-Object 'System.Collections.Generic.List[string]'

foreach ($line in $lines) {
    $match = [regex]::Match($line, $defineRegex)
    if (-not $match.Success) {
        continue
    }

    $name = $match.Groups['name'].Value
    $matched = $false
    foreach ($prefix in $prefixes) {
        if ($name.StartsWith($prefix, [StringComparison]::Ordinal)) {
            $matched = $true
            break
        }
    }

    if (-not $matched) {
        continue
    }

    $raw = $match.Groups['value'].Value
    $value = $null

    if ($raw -match $typedefRegex -or $raw -match '^0[xX]') {
        # 已经是 HRESULT 字面量，直接取十六进制。
        $hexMatch = [regex]::Match($raw, $hexRegex)
        if (-not $hexMatch.Success) {
            $skipped.Add($name) | Out-Null
            continue
        }

        $value = [uint32]([Convert]::ToUInt32($hexMatch.Groups['digits'].Value, 16))
    }
    else {
        $decMatch = [regex]::Match($raw, $decRegex)
        if (-not $decMatch.Success) {
            $skipped.Add($name) | Out-Null
            continue
        }

        # 十进制 Win32 码 → HRESULT_FROM_WIN32。
        $code = [uint32]([Convert]::ToUInt32($decMatch.Groups['digits'].Value, 10))
        $value = [uint32](0x80070000 -bor $code)
    }

    if (-not $entries.ContainsKey($value)) {
        $entries.Add($value, $name)
    }
}

if ($entries.Count -eq 0) {
    throw '没有从 winerror.h 提取到任何错误码；请检查文件内容或前缀列表。'
}

$builder = New-Object System.Text.StringBuilder
$builder.AppendLine('// 由 scripts/generate-deployment-errors.ps1 自动生成，请勿手工编辑。') | Out-Null
$builder.AppendLine('//') | Out-Null
$builder.AppendLine('// 用途：把 Windows 部署栈（PackageManager / Provisioning）返回的扩展错误码翻译成人话。') | Out-Null
$builder.AppendLine('// 名字与数值直接来自 Windows SDK，避免手抄漏码。') | Out-Null
$builder.AppendLine('//') | Out-Null
$builder.AppendLine('// 这些是**安装期**错误码，不是容器格式错误码：') | Out-Null
$builder.AppendLine('// docs/msixpkg-format-v1.md §8 与 C# 端 MsixpkgErrors.cs 的 42 个 E_* 码集合不受影响，') | Out-Null
$builder.AppendLine('// 仍然逐字一致。') | Out-Null
$builder.AppendLine('//') | Out-Null
$builder.AppendLine(('// 来源：' + $WinErrorHeader)) | Out-Null
$builder.AppendLine(('// 条目：' + $entries.Count + ' 个（按数值升序，数值相同的取先出现的名字）')) | Out-Null
$builder.AppendLine() | Out-Null
$builder.AppendLine('#pragma once') | Out-Null
$builder.AppendLine() | Out-Null
$builder.AppendLine('#include <cstdint>') | Out-Null
$builder.AppendLine() | Out-Null
$builder.AppendLine('namespace msixpkg {') | Out-Null
$builder.AppendLine() | Out-Null
$builder.AppendLine('struct DeploymentErrorName {') | Out-Null
$builder.AppendLine('    uint32_t code;') | Out-Null
$builder.AppendLine('    const char* name;') | Out-Null
$builder.AppendLine('};') | Out-Null
$builder.AppendLine() | Out-Null
$builder.AppendLine('/// 升序排列，供二分 / 线性查找；数值是 32 位 HRESULT。') | Out-Null
$builder.AppendLine('inline constexpr DeploymentErrorName kDeploymentErrorNames[] = {') | Out-Null

foreach ($pair in ($entries.GetEnumerator() | Sort-Object -Property Key)) {
    $hex = ('0x{0:x8}' -f $pair.Key)
    $builder.AppendLine(('    {' + $hex + 'u, "' + $pair.Value + '"},')) | Out-Null
}

$builder.AppendLine('};') | Out-Null
$builder.AppendLine() | Out-Null
$builder.AppendLine('}  // namespace msixpkg') | Out-Null

$directory = Split-Path $OutputPath -Parent
if (-not (Test-Path $directory)) {
    New-Item -ItemType Directory -Path $directory -Force | Out-Null
}

# 必须写 UTF-8 无 BOM：CMake / MSVC 用 /utf-8 读，BOM 会进到源码里。
[IO.File]::WriteAllText($OutputPath, $builder.ToString(), (New-Object Text.UTF8Encoding($false)))

Write-Host ('来源   : ' + $WinErrorHeader)
Write-Host ('输出   : ' + $OutputPath)
Write-Host ('条目   : ' + $entries.Count)
if ($skipped.Count -gt 0) {
    Write-Host ('跳过   : ' + ($skipped -join ', '))
}
