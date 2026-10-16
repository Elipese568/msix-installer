<#
.SYNOPSIS
    把构建期打包工具（Msixpkg.Build）打成 NuGet 包。

.DESCRIPTION
    产物：artifacts/nuget/Msixpkg.Build.<版本>.nupkg

    本仓库**只发布这一个包**。Msixpkg.Core 不再 IsPackable，它作为「二进制附属」
    随包内的 tasks\netstandard2.0\Msixpkg.Core.dll 一起分发 —— 任务 DLL 由 MSBuild
    在自己的 load context 里加载，依赖必须与它同目录才能解析。Msixpkg.Packer（CLI）
    是开发机 / CI 的构建期工具，同样不上 NuGet（终端用户拿到的原生安装器走 installer/nsis）。

    包内容（这就是对外契约，tests\e2e\nuget-package-e2e.ps1 会逐条断言）：

        build\Msixpkg.Build.targets                  NuGet 自动导入
        tasks\netstandard2.0\Msixpkg.Tasks.dll        任务程序集
        tasks\netstandard2.0\Msixpkg.Core.dll         二进制附属
        LICENSE / PACKAGE.md
        （没有 lib\，也没有 dependencies）

.PARAMETER Configuration
    构建配置，默认 Release。

.PARAMETER OutputDir
    包输出目录，默认 artifacts\nuget。

.PARAMETER Version
    覆盖包版本。默认取 Directory.Build.props 里的 0.1.0。
    注意：samples\MsixpkgPackageSample 里的 PackageReference 钉的是默认版本，
    改版本号后要同步改那里，否则验收样例会还原失败。

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File scripts\pack-nuget.ps1

.EXAMPLE
    # 本地验收：打包 → 用 PackageReference 样例还原并构建 → 与黄金样本逐字节比对
    powershell -ExecutionPolicy Bypass -File tests\e2e\nuget-package-e2e.ps1
#>
[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [string]$OutputDir,
    [string]$Version
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
$projectPath = Join-Path $repoRoot 'src\Msixpkg.Build\Msixpkg.Build.csproj'

if (-not $OutputDir) {
    $OutputDir = Join-Path $repoRoot 'artifacts\nuget'
}

if (-not (Test-Path $projectPath)) {
    throw "找不到任务包工程：$projectPath"
}

# NuGet 的 scratch 目录默认指向用户级位置（本机是 E:\Nuget\NugetScratch）。在受限 / 沙箱环境里
# 那里可能不可写（表现为莫名其妙的锁定文件访问拒绝），所以固定指到仓库内的临时目录。
# 全局包缓存（NUGET_PACKAGES）**不**动：本包依赖 Microsoft.Build.* 与 netstandard.library，
# 那些得从开发机已有的缓存里解析。
$env:NUGET_SCRATCH = Join-Path $repoRoot 'artifacts\temp\nuget-scratch'

New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null

# 这三个 MSBuild 开关是为了在「不允许命名管道」的受限环境（沙箱 / 容器 / 某些 CI 策略）里也能构建：
#   -m:1                 单节点，避免 MSBuild 工作节点通过命名管道通信
#   -nodeReuse:false     不留常驻构建节点
#   -p:UseSharedCompilation=false   不用 Roslyn 编译器服务器（同样是命名管道）
# 本脚本只构建两个小工程，关掉它们几乎不影响耗时，但决定了它在受限环境下能不能跑。
$msbuildArguments = @(
    '-m:1'
    '-nodeReuse:false'
    '-p:UseSharedCompilation=false'
)

$packArguments = @(
    'pack', $projectPath
    '-c', $Configuration
    '--nologo'
    '-o', $OutputDir
) + $msbuildArguments

if ($Version) {
    $packArguments += "-p:Version=$Version"
    Write-Host "版本覆盖：$Version"
}

Write-Host "打包 Msixpkg.Build（$Configuration）..."
& dotnet @packArguments
if ($LASTEXITCODE -ne 0) {
    throw "dotnet pack 失败（退出码 $LASTEXITCODE）"
}

$packages = @(Get-ChildItem -Path $OutputDir -Filter 'Msixpkg.Build.*.nupkg' -File | Sort-Object LastWriteTime -Descending)
if ($packages.Count -eq 0) {
    throw "dotnet pack 报成功，但 $OutputDir 下没有 Msixpkg.Build.*.nupkg"
}

$package = $packages[0]
$hash = (Get-FileHash -Algorithm SHA256 -Path $package.FullName).Hash

Write-Host ''
Write-Host "产物：$($package.FullName)"
Write-Host "      大小 $([Math]::Round($package.Length / 1KB, 1)) KB"
Write-Host "      SHA-256 $hash"
Write-Host ''
Write-Host '下一步（本地验收，不需要管理员）：'
Write-Host '  powershell -ExecutionPolicy Bypass -File tests\e2e\nuget-package-e2e.ps1'
