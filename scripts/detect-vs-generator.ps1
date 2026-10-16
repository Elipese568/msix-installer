<#
.SYNOPSIS
    探测本机 Visual Studio，打印 CMake 需要的生成器名。

.DESCRIPTION
    CI 里**不要**写死 `-G "Visual Studio 17 2022"`：托管运行器会换镜像 ——
    windows-latest 已在 2026-06 从 Windows Server 2022 + VS 2022 换成
    Windows Server 2025 + VS 2026（actions/runner-images#14017），
    写死的生成器名会直接 `could not find any instance of Visual Studio` 失败。

    这里用 vswhere 找到**带 C++ 工具链**的 VS，把主版本映射成生成器名：
      17 → Visual Studio 17 2022
      18 → Visual Studio 18 2026
    本机（VS 2026 18.10.3）与 CI 都会得到正确的名字，所以本地也可以用它来对齐。

    输出：
      - 标准输出：生成器名（便于 `$g = & scripts\detect-vs-generator.ps1`）
      - 若环境变量 GITHUB_ENV 存在（GitHub Actions 里就是这样），额外写入
        `MSIXPKG_CMAKE_GENERATOR=<名字>`，供同一作业的后续 step 用 `${{ env.MSIXPKG_CMAKE_GENERATOR }}`

.EXAMPLE
    $generator = & scripts\detect-vs-generator.ps1
    cmake -S src\MsixpkgInstaller.Native -B build\ci-native -G $generator -A x64
#>
[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) {
    throw "找不到 vswhere.exe：$vswhere（没有安装 Visual Studio，或安装器版本过老）"
}

# -requires 把「装了 VS 但没有 C++ 工具链」的实例排除掉，否则 CMake 会死在更晚的一步。
$found = @(& $vswhere -latest -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property installationVersion)
if ($found.Count -eq 0 -or [string]::IsNullOrWhiteSpace($found[0])) {
    throw '安装的 Visual Studio 里没有一个带 C++ 工具链（Microsoft.VisualStudio.Component.VC.Tools.x86.x64）'
}

$installationVersion = $found[0].Trim()
$major = [int]$installationVersion.Split('.')[0]

switch ($major) {
    17 { $generator = 'Visual Studio 17 2022' }
    18 { $generator = 'Visual Studio 18 2026' }
    default {
        throw "Visual Studio 主版本 $major（$installationVersion）还没有对应的 CMake 生成器名；" +
              '请在 scripts\detect-vs-generator.ps1 的 switch 里补一条。'
    }
}

Write-Host "Visual Studio $installationVersion → CMake 生成器：$generator"

if ($env:GITHUB_ENV) {
    "MSIXPKG_CMAKE_GENERATOR=$generator" | Out-File -FilePath $env:GITHUB_ENV -Append -Encoding utf8
}

Write-Output $generator
