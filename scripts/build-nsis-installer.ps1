<#
.SYNOPSIS
    用 NSIS 3 构建 Msixpkg Installer 的分发安装包。

.DESCRIPTION
    产物：artifacts/setup/MsixpkgInstaller-<版本>-setup.exe（外加同名的 .sha256）。

    这个包干的事是「把 Msixpkg 安装器装到用户机器上」，也就是「安装包的安装包」：
      * 载荷 = 原生安装器的两个产物（msixpkg-installer.exe 向导 + msixpkg-native.exe CLI）
      * 另外配好开始菜单/桌面快捷方式、.msixpkg 文件关联、PATH、「设置 → 应用」卸载入口
    脚本本身（installer/nsis/MsixpkgInstaller.nsi）里有完整的开关与退出码说明。

    构建步骤：
      1. 版本号从 src/MsixpkgInstaller.Native/res/msixpkg.rc 的 FILEVERSION 读 —— 单一事实来源，
         不手抄版本号（exe 的「属性 → 详细信息」、ARP 的 DisplayVersion 与包名必然一致）。
      2. 检查载荷（native 的 Release/Debug 产物）在不在；-BuildNative 时先跑 cmake 构建。
      3. 准备 NSIS 工具链：-NsisDir → artifacts/tools/nsis → 自动下载官方包。
         官方 3.10 的默认构建是 NSIS_MAX_STRLEN=1024，PATH 组件读长 PATH 会被静默截断，
         因此这里下载官方 nsis-3.10-strlen_8192.zip 叠加到主包上（两份都校验 SHA-256）。
      4. 生成 installer/nsis/build-defines.nsh（引号内的路径，绕开命令行 /D 的空格问题），
         调 makensis /WX /INPUTCHARSET UTF8，编译完删掉该文件。
      5. 打印产物路径、大小与 SHA-256，并写出 .sha256 文件；-Sign 时再用 signtool 签名。

.PARAMETER Configuration
    载荷用哪个配置的原生产物：Release（默认）或 Debug。

.PARAMETER NsisDir
    已安装的 NSIS 目录（含 makensis.exe）。省略时用 artifacts/tools/nsis，没有就自动下载。

.PARAMETER OutputDir
    产物目录，默认 artifacts/setup。

.PARAMETER PayloadDir
    载荷目录（含 msixpkg-installer.exe / msixpkg-native.exe）。省略时用本地 CMakePresets
    的输出目录 src\MsixpkgInstaller.Native\build\x64\<Configuration>；CI 的构建目录不同
    （build\ci-native\Release），用这个参数指过去。

.PARAMETER BuildNative
    先按 CMakePresets 构建原生工程（cmake --preset windows-x64 + --build --preset windows-x64-<配置>）。

.PARAMETER NoProvision
    不允许联网下载 NSIS 工具链：缺工具链时直接失败。

.PARAMETER Sign / PfxPath / PfxPassword / TimestampUrl
    可选的 signtool 签名（不传 -Sign 就完全不动签名，方便本地验证）。

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File scripts/build-nsis-installer.ps1

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File scripts/build-nsis-installer.ps1 -BuildNative -Sign -PfxPath certs\release.pfx -PfxPassword ***
#>
[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [string]$PayloadDir,
    [string]$NsisDir,
    [string]$OutputDir,
    [switch]$BuildNative,
    [switch]$NoProvision,
    [switch]$Sign,
    [string]$PfxPath,
    [string]$PfxPassword,
    [string]$TimestampUrl = 'http://timestamp.digicert.com'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
$nativeDir = Join-Path $repoRoot 'src\MsixpkgInstaller.Native'
$nsiPath = Join-Path $repoRoot 'installer\nsis\MsixpkgInstaller.nsi'
$definesPath = Join-Path $repoRoot 'installer\nsis\build-defines.nsh'
$toolsDir = Join-Path $repoRoot 'artifacts\tools'
$defaultNsisDir = Join-Path $toolsDir 'nsis'

if (-not $OutputDir) {
    $OutputDir = Join-Path $repoRoot 'artifacts\setup'
}

# ---------------------------------------------------------------------------
# 0) 入参绝对化
# ---------------------------------------------------------------------------
# makensis 解析 File / OutFile 时相对的是**脚本所在目录**（installer\nsis\），不是当前工作目录。
# 于是 -PayloadDir / -OutputDir 只要传相对路径就会变成
#   File: "build\ci-native\Release\msixpkg-installer.exe" -> no files found.
# （本机默认路径恰好是绝对路径，所以这个坑只在 CI 里传相对路径时才暴露。）
function ConvertTo-AbsolutePath {
    param([string]$Path)

    if ([string]::IsNullOrEmpty($Path)) {
        return $Path
    }

    if ([System.IO.Path]::IsPathRooted($Path)) {
        return [System.IO.Path]::GetFullPath($Path)
    }

    return [System.IO.Path]::GetFullPath((Join-Path (Get-Location).Path $Path))
}

if ($PayloadDir) { $PayloadDir = ConvertTo-AbsolutePath $PayloadDir }
if ($OutputDir) { $OutputDir = ConvertTo-AbsolutePath $OutputDir }
if ($NsisDir) { $NsisDir = ConvertTo-AbsolutePath $NsisDir }

# 官方 NSIS 3.10 的两个包（来源：sourceforge.net/projects/nsis/files/NSIS%203/3.10/）。
# 哈希取自该目录的文件清单；下载后必须逐字节对上，防止镜像被替换或半截下载。
$nsisMainUrl = 'https://downloads.sourceforge.net/project/nsis/NSIS%203/3.10/nsis-3.10.zip'
$nsisMainSha256 = 'FCDCE3229717A2A148E7CDA0AB5BDB667F39D8FB33EDE1DA8DABC336BD5AD110'
$nsisLenUrl = 'https://downloads.sourceforge.net/project/nsis/NSIS%203/3.10/nsis-3.10-strlen_8192.zip'
$nsisLenSha256 = 'B475B49E7FB788524E768AD56BFF951DC72A6B8EA94B3E70CF07B81A0F08A419'

function Get-RemoteFile {
    param([string]$Url, [string]$Destination, [string]$Sha256)

    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Destination) | Out-Null
    if (Test-Path $Destination) {
        $existing = (Get-FileHash -Algorithm SHA256 -Path $Destination).Hash
        if ($existing -eq $Sha256) {
            Write-Host "  [cache] $Destination"
            return
        }
        Write-Host "  [stale] $Destination (哈希不符，重新下载)"
        Remove-Item $Destination -Force
    }

    # curl.exe 优先：Windows 自带的 schannel 版本在拿不到吊销列表的网络里会以
    # CRYPT_E_NO_REVOCATION_CHECK 失败，所以先带 --ssl-no-revoke 试一次。
    $curl = Get-Command curl.exe -ErrorAction SilentlyContinue
    $downloaded = $false
    if ($curl) {
        foreach ($extra in @(@('--ssl-no-revoke'), @())) {
            $arguments = @('-sS', '-L', '--max-time', '600') + $extra + @('-o', $Destination, $Url)
            & $curl.Source @arguments 2>$null
            if ($LASTEXITCODE -eq 0 -and (Test-Path $Destination) -and (Get-Item $Destination).Length -gt 0) {
                $downloaded = $true
                break
            }
        }
    }

    if (-not $downloaded) {
        [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
        [Net.ServicePointManager]::CheckCertificateRevocationList = $false
        try {
            (New-Object Net.WebClient).DownloadFile($Url, $Destination)
            $downloaded = $true
        } catch {
            throw "下载失败：$Url`n$($_.Exception.Message)"
        }
    }

    $actual = (Get-FileHash -Algorithm SHA256 -Path $Destination).Hash
    if ($actual -ne $Sha256) {
        throw "SHA-256 不符：$Destination`n  期望 $Sha256`n  实际 $actual"
    }
    Write-Host "  [ok]    $Destination"
}

function Test-NsisToolchain {
    param([string]$Directory)

    $makensis = Join-Path $Directory 'makensis.exe'
    if (-not (Test-Path $makensis)) { return $false }
    if (-not (Test-Path (Join-Path $Directory 'Include\MUI2.nsh'))) { return $false }
    if (-not (Test-Path (Join-Path $Directory 'Include\StrFunc.nsh'))) { return $false }
    return $true
}

function Get-NsisMaxStrlen {
    param([string]$Directory)

    $info = & (Join-Path $Directory 'makensis.exe') /HDRINFO 2>&1 | Out-String
    if ($info -match 'NSIS_MAX_STRLEN=(\d+)') { return [int]$Matches[1] }
    return 0
}

function Install-NsisToolchain {
    param([string]$ToolsDir, [string]$NsisDir, [switch]$NoProvision)

    if ($NoProvision) {
        throw "找不到可用的 NSIS 工具链（$NsisDir）。请用 -NsisDir 指定，或去掉 -NoProvision 允许自动下载。"
    }

    Write-Host '准备 NSIS 工具链（首次需要联网下载官方包）...'
    $mainZip = Join-Path $ToolsDir 'nsis-3.10.zip'
    $lenZip = Join-Path $ToolsDir 'nsis-3.10-strlen_8192.zip'
    Get-RemoteFile -Url $nsisMainUrl -Destination $mainZip -Sha256 $nsisMainSha256
    Get-RemoteFile -Url $nsisLenUrl -Destination $lenZip -Sha256 $nsisLenSha256

    $mainExtract = Join-Path $ToolsDir 'nsis-3.10'
    $lenExtract = Join-Path $ToolsDir 'nsis-3.10-strlen_8192'
    foreach ($pair in @(@($mainZip, $mainExtract), @($lenZip, $lenExtract))) {
        if (Test-Path $pair[1]) { Remove-Item $pair[1] -Recurse -Force }
        Expand-Archive -Path $pair[0] -DestinationPath $pair[1] -Force
    }

    # 主包是完整发行版（Include/Plugins/Docs...），strlen_8192 只是「覆盖包」
    # （makensis.exe + Bin + Stubs），两者叠在一起才有 MUI2.nsh 与 8192 的字符串上限。
    $mainRoot = Get-ChildItem $mainExtract -Directory | Select-Object -First 1
    if (-not $mainRoot) { throw "压缩包结构不认识：$mainExtract" }

    if (Test-Path $NsisDir) { Remove-Item $NsisDir -Recurse -Force }
    Copy-Item $mainRoot.FullName $NsisDir -Recurse -Force
    Copy-Item (Join-Path $lenExtract '*') $NsisDir -Recurse -Force
    Write-Host "  叠加完成：$NsisDir"
}

function Build-NativePayload {
    param([string]$NativeDir, [string]$Configuration)

    $preset = "windows-x64-$($Configuration.ToLowerInvariant())"
    Write-Host "构建原生载荷（cmake --preset windows-x64 / --build --preset $preset）..."
    Push-Location $NativeDir
    try {
        & cmake --preset windows-x64 -S . | Out-Host
        if ($LASTEXITCODE -ne 0) { throw "cmake configure 失败（退出码 $LASTEXITCODE）" }
        & cmake --build --preset $preset --parallel | Out-Host
        if ($LASTEXITCODE -ne 0) { throw "cmake build 失败（退出码 $LASTEXITCODE）" }
    } finally {
        Pop-Location
    }
}

# ---------------------------------------------------------------------------
# 1) 版本号（来自 res/msixpkg.rc 的 FILEVERSION）
# ---------------------------------------------------------------------------
$rcPath = Join-Path $nativeDir 'res\msixpkg.rc'
if (-not (Test-Path $rcPath)) { throw "找不到版本来源：$rcPath" }
$rcText = [IO.File]::ReadAllText($rcPath, [Text.Encoding]::UTF8)
if ($rcText -notmatch 'FILEVERSION\s+(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)') {
    throw "在 $rcPath 里找不到 FILEVERSION x, y, z, w"
}
$productVersion = "$($Matches[1]).$($Matches[2]).$($Matches[3])"
$productVersionQuad = "$($Matches[1]).$($Matches[2]).$($Matches[3]).$($Matches[4])"

# ---------------------------------------------------------------------------
# 2) 载荷
# ---------------------------------------------------------------------------
if ($BuildNative) { Build-NativePayload -NativeDir $nativeDir -Configuration $Configuration }

if (-not $PayloadDir) {
    $PayloadDir = Join-Path $nativeDir "build\x64\$Configuration"
}
$guiExe = Join-Path $PayloadDir 'msixpkg-installer.exe'
$cliExe = Join-Path $PayloadDir 'msixpkg-native.exe'
foreach ($required in @($guiExe, $cliExe, $nsiPath)) {
    if (-not (Test-Path $required)) {
        throw "缺少 $required`n先构建原生工程：cmake --preset windows-x64 -S src\MsixpkgInstaller.Native`n              cmake --build --preset windows-x64-$($Configuration.ToLowerInvariant())`n或者加 -BuildNative 让本脚本代劳（CI 里用 -PayloadDir 指向它的构建目录）。"
    }
}

# ---------------------------------------------------------------------------
# 3) NSIS 工具链
# ---------------------------------------------------------------------------
if ($NsisDir) {
    if (-not (Test-NsisToolchain -Directory $NsisDir)) {
        throw "-NsisDir 指向的目录不像 NSIS 安装目录（缺 makensis.exe / Include\MUI2.nsh）：$NsisDir"
    }
} else {
    $NsisDir = $defaultNsisDir
    if (-not (Test-NsisToolchain -Directory $NsisDir)) {
        Install-NsisToolchain -ToolsDir $toolsDir -NsisDir $NsisDir -NoProvision:$NoProvision
    } elseif ((Get-NsisMaxStrlen -Directory $NsisDir) -lt 8192) {
        Write-Host "现有工具链的 NSIS_MAX_STRLEN < 8192，PATH 组件会拒绝改动较长的 PATH；重新准备工具链。"
        Install-NsisToolchain -ToolsDir $toolsDir -NsisDir $NsisDir -NoProvision:$NoProvision
    }
}

$makensis = Join-Path $NsisDir 'makensis.exe'
$maxStrlen = Get-NsisMaxStrlen -Directory $NsisDir
if ($maxStrlen -lt 8192) {
    throw "NSIS_MAX_STRLEN=$maxStrlen < 8192：PATH 组件在长 PATH 上会被迫拒绝改动（安全但没用）。请用官方 strlen_8192 构建，见 installer/nsis/README.md。"
}

# ---------------------------------------------------------------------------
# 4) 生成 build-defines.nsh 并编译
# ---------------------------------------------------------------------------
New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null
$iconPath = Join-Path $nativeDir 'res\msixpkg.ico'
$outputName = "MsixpkgInstaller-$productVersion-setup.exe"

# ARP 的 EstimatedSize（KB）：载荷 + 一个卸载器（NSIS 的卸载器在安装时生成，约 430 KB。
# 这里只是「设置 → 应用」里显示的一个体积提示，不参与任何逻辑）。
$estimatedSizeKb = [int]((((Get-Item $guiExe).Length + (Get-Item $cliExe).Length) / 1KB) + 450)

$defines = @(
    "; 由 scripts/build-nsis-installer.ps1 生成，编译结束就删；不要手改。"
    "; 放在这里而不是命令行 /D：路径里可能有空格，/D 的引号规则很容易踩坑。"
    "!define PRODUCT_VERSION `"$productVersion`""
    "!define PRODUCT_VERSION_QUAD `"$productVersionQuad`""
    "!define APP_GUI `"$guiExe`""
    "!define APP_CLI `"$cliExe`""
    "!define APP_ICON `"$iconPath`""
    "!define OUTPUT_DIR `"$OutputDir`""
    "!define OUTPUT_NAME `"$outputName`""
    "!define ESTIMATED_SIZE_KB $estimatedSizeKb"
)
[IO.File]::WriteAllLines($definesPath, $defines, (New-Object Text.UTF8Encoding($false)))

try {
    Write-Host "编译（NSIS $(& $makensis /VERSION)、NSIS_MAX_STRLEN=$maxStrlen、/WX）..."
    & $makensis /V2 /WX /INPUTCHARSET UTF8 $nsiPath
    if ($LASTEXITCODE -ne 0) { throw "makensis 失败（退出码 $LASTEXITCODE）" }
} finally {
    if (Test-Path $definesPath) { Remove-Item $definesPath -Force }
}

$setupPath = Join-Path $OutputDir $outputName
if (-not (Test-Path $setupPath)) { throw "makensis 报成功，但产物不存在：$setupPath" }

# ---------------------------------------------------------------------------
# 5) 可选签名 + 摘要
# ---------------------------------------------------------------------------
if ($Sign) {
    if (-not $PfxPath -or -not (Test-Path $PfxPath)) {
        throw "-Sign 需要 -PfxPath 指向一个可用的 PFX（自签名证书只适合内部分发）。"
    }
    $signtool = $SignTool
    if (-not $signtool) {
        $candidates = Get-ChildItem -Path (Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\bin') -Filter signtool.exe -Recurse -ErrorAction SilentlyContinue |
            Where-Object { $_.FullName -match '\\x64\\' } | Sort-Object FullName -Descending
        if ($candidates) { $signtool = $candidates[0].FullName }
    }
    if (-not $signtool -or -not (Test-Path $signtool)) {
        throw '找不到 signtool.exe，请用 -SignTool 指定（Windows SDK 的 bin\<ver>\x64\signtool.exe）。'
    }
    Write-Host "签名（$signtool）..."
    $signArguments = @('sign', '/fd', 'SHA256', '/f', $PfxPath, '/tr', $TimestampUrl, '/td', 'SHA256', $setupPath)
    if ($PfxPassword) { $signArguments = @('sign', '/fd', 'SHA256', '/f', $PfxPath, '/p', $PfxPassword, '/tr', $TimestampUrl, '/td', 'SHA256', $setupPath) }
    & $signtool @signArguments | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "signtool 失败（退出码 $LASTEXITCODE）" }
}

$hash = (Get-FileHash -Algorithm SHA256 -Path $setupPath).Hash
$hashPath = "$setupPath.sha256"
"$hash *$outputName" | Set-Content -Path $hashPath -Encoding ASCII

# ---------------------------------------------------------------------------
# 6) 免提权启动器：先把 TEMP/TMP 指到可写的位置，再启动安装包
# ---------------------------------------------------------------------------
# NSIS 的 stub 在跑脚本**之前**就要写 %TEMP%；写不了时双击 setup 只会弹
# "Error writing temporary file. Make sure your temp folder is valid."，而且安装包
# 自己来不及解释（脚本还没开始跑）。这个包装脚本先挑一个可写的临时目录再启动安装包，
# 于是「非提权 + %TEMP% 不可写」的机器也能装（受限环境里关联/PATH/ARP 仍可能被拒，
# 那些会如实写进 setup.log，见 installer/nsis/README.md §7）。
# 内容是纯 ASCII：.cmd 由 cmd.exe 按控制台代码页解析，不该让它碰中文。
$launcherName = 'run-setup-with-local-temp.cmd'
$launcherPath = Join-Path $OutputDir $launcherName
$launcher = @(
    '@echo off'
    'rem Launches the Msixpkg Installer setup with a TEMP folder it can actually write.'
    'rem'
    'rem Why: the NSIS stub must write %TEMP% *before* the setup script runs. If that'
    'rem write is denied (containers, sandboxes, locked-down machines), a plain double'
    'rem click only shows "Error writing temporary file. Make sure your temp folder is'
    'rem valid." - the script never gets a chance to explain anything itself.'
    'rem'
    'rem Arguments are forwarded as-is, e.g.:'
    'rem   run-setup-with-local-temp.cmd /S /CURRENTUSER /D=C:\tools\msixpkg'
    'setlocal'
    "set `"SETUP=%~dp0$outputName`""
    'set "LOCALTEMP=%~dp0.installer-temp"'
    'if not exist "%LOCALTEMP%" mkdir "%LOCALTEMP%" 2>nul'
    'if exist "%LOCALTEMP%" goto temp_ready'
    'set "LOCALTEMP=%LOCALAPPDATA%\MsixpkgInstaller\temp"'
    'if not exist "%LOCALTEMP%" mkdir "%LOCALTEMP%" 2>nul'
    'if exist "%LOCALTEMP%" goto temp_ready'
    'set "LOCALTEMP=%TEMP%"'
    ':temp_ready'
    'set "TEMP=%LOCALTEMP%"'
    'set "TMP=%LOCALTEMP%"'
    'echo Using TEMP=%TEMP%'
    '"%SETUP%" %*'
    'set "RC=%ERRORLEVEL%"'
    'if not "%RC%"=="0" ('
    '  echo.'
    '  echo The setup exited with code %RC%.'
    '  pause'
    ')'
    'endlocal & exit /b %RC%'
)
[IO.File]::WriteAllLines($launcherPath, $launcher, (New-Object Text.ASCIIEncoding))

# ---------------------------------------------------------------------------
# 7) 便携包：同样的两个 exe 打成 zip
# ---------------------------------------------------------------------------
# 为什么要有它：**所有** NSIS 安装包在跑脚本之前就得能写 %TEMP%（stub 要建自己的
# 临时目录），在容器 / Sandbox / 被策略锁死的机器上这一步会直接弹
# "Error writing temporary file. Make sure your temp folder is valid." —— 而且
# 那时脚本还没开始跑，安装包自己没法给提示。便携包不依赖临时目录，解压即用
#（代价是不注册快捷方式 / 关联 / PATH / ARP，见 installer/nsis/README.md）。
$portableName = "MsixpkgInstaller-$productVersion-portable.zip"
$portablePath = Join-Path $OutputDir $portableName
$stageDir = Join-Path $toolsDir 'portable-stage'
if (Test-Path $stageDir) { Remove-Item $stageDir -Recurse -Force }
New-Item -ItemType Directory -Force -Path $stageDir | Out-Null
Copy-Item $guiExe (Join-Path $stageDir 'msixpkg-installer.exe')
Copy-Item $cliExe (Join-Path $stageDir 'msixpkg-native.exe')
$portableReadme = @(
    "Msixpkg Installer $productVersion - portable copy"
    ""
    "msixpkg-installer.exe   The wizard: open a .msixpkg and install the app inside."
    "msixpkg-native.exe      The command line frontend: install / uninstall / list / verify / repair / clean."
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
    "  An NSIS setup needs a writable %TEMP% before it can start; in containers,"
    "  sandboxes and locked-down machines that write is denied and the setup can only"
    "  say 'Error writing temporary file'. This zip needs no temporary folder."
    ""
    "Documentation: installer/nsis/README.md in the repository."
)
[IO.File]::WriteAllLines((Join-Path $stageDir 'README-portable.txt'), $portableReadme, (New-Object Text.UTF8Encoding($false)))
if (Test-Path $portablePath) { Remove-Item $portablePath -Force }
Compress-Archive -Path (Join-Path $stageDir '*') -DestinationPath $portablePath -CompressionLevel Optimal
Remove-Item $stageDir -Recurse -Force
$portableHash = (Get-FileHash -Algorithm SHA256 -Path $portablePath).Hash
"$portableHash *$portableName" | Set-Content -Path "$portablePath.sha256" -Encoding ASCII

$sizeMb = [Math]::Round((Get-Item $setupPath).Length / 1MB, 2)
$portableMb = [Math]::Round((Get-Item $portablePath).Length / 1MB, 2)
Write-Host ''
Write-Host "产物：$setupPath"
Write-Host "      版本 $productVersion（载荷 $Configuration：$((Get-Item $guiExe).Length) + $((Get-Item $cliExe).Length) 字节）"
Write-Host "      大小 $sizeMb MB"
Write-Host "      SHA-256 $hash"
Write-Host "      摘要文件 $hashPath"
Write-Host "便携包：$portablePath"
Write-Host "      大小 $portableMb MB（不需要可写的 %TEMP%，解压即用）"
Write-Host "      SHA-256 $portableHash"
Write-Host "      摘要文件 $portablePath.sha256"
