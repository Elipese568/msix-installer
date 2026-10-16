# MsixpkgInstaller

把 Packaged WinUI 3 应用打成单一 `.msixpkg` 文件，再用一个原生安装器完成
「导入发布者证书 → 安装 MSIX」与「卸载应用 → 回收证书」的完整闭环。

```
[开发机 / CI]  WinUI3 单项目 MSIX ──msbuild──▶ .msix + .cer ──Msixpkg.Tasks──▶ App_1.0.0.0_x64.msixpkg
[用户机]       msixpkg.exe install App.msixpkg ─▶ 校验容器 ─▶ 导入 TrustedPeople ─▶ AddPackageByUriAsync
[用户机]       msixpkg.exe uninstall --family-name <PFN> ─▶ RemovePackageAsync ─▶ 引用计数后回收证书
```

设计依据与完整决策记录见 [`docs/`](docs/)；格式契约见
[`docs/msixpkg-format-v1.md`](docs/msixpkg-format-v1.md)（规范性来源，两端实现都必须照它写）。

> **接手开发请先读 [`docs/handoff.md`](docs/handoff.md)** —— 剩余任务清单（M4–M7）、
> 已存在且必须复用的接口、构建与测试命令、以及 14 条已经踩过的坑都在那一个文件里。

## 状态

| 里程碑 | 内容 | 状态 |
|---|---|---|
| M0 | 解除工作区权限阻塞、核对工具链 | ✅ 完成 |
| M1 | 格式规范 v1 + `Msixpkg.Core` + 黄金样本 | ✅ 完成（126 条测试全绿） |
| M2 | `Msixpkg.Packer` CLI + `Msixpkg.Build`（MSBuild 任务）+ 真实 WinUI3 工程接入 | ✅ 完成（两个宿主 + 真实工程均验证） |
| M3 | 原生容器读 + 校验器 | ✅ 完成（与 C# 摘要逐字节一致） |
| M4 | 证书助手 + 部署封装 + 状态库/日志 + 编排 + CLI 前端 | ✅ 完成 |
| M5 | 卸载引用计数 + `list/verify/repair/clean` + ARP 卸载入口 | ✅ 完成 |
| M6 | Win32 GUI 向导（五页、DPI/深色/本地化/无障碍） | ✅ 完成 |
| M7 | E2E（负例套件 + 完整闭环 + Windows Sandbox）+ CI + 文档定稿 | ✅ 完成 |

原生侧测试 **58 条**、托管侧 **126 条**，Debug 与 Release 双配置均绿，Release 开 `/WX`（零警告）；
负例 E2E 33 项检查全过（`tests/e2e/msixpkg-negative.ps1`，不需要管理员）。

已验证的关键事实：

- 打包器 CLI 用固定时间戳 `--created-utc` 能**逐字节复现**黄金样本
  （`EA972886…A986`），即同一份输入在任意机器上产出完全相同字节；
- **真实 WinUI 3 单项目 MSIX 工程全链路打通**（`winapp new` 生成，VS 2026 + Win11 26300 +
  `Microsoft.Windows.SDK.BuildTools.MSIX` 1.7）：`dotnet build` 与 VS 的 `MSBuild.exe`
  **两个宿主**都产出签名 `.msix`（60.8 MB，self-contained）→ `.msixpkg` → `verify --pin` 通过；
- **C++ 原生实现独立算出的内容摘要与 C# 完全一致**（`e67cf348…b4fc`），并在**没有任何证书存储**
  的前提下完成容器验签；自己实现的 raw DEFLATE 能正确解压真实 makeappx 产出的全部压缩条目；
- `PublisherId`（包族名推导）用本机真实已安装包交叉验证：`Microsoft.BioEnrollment` +
  `CN=Microsoft Windows, …` → `cw5n1h2txyewy`，与 `Get-AppxPackage` 完全一致；
- 三个只有跑真实工程才会暴露的坑（细节见 [`docs/integration-winui3.md`](docs/integration-winui3.md)）：
  1. `GenerateAppxPackage` 目标**不存在**，真实目标是 `_GenerateAppxPackage`（挂错钩子会静默不打包）；
  2. 产物路径是属性 `$(AppxPackageOutput)`；`@(AppxPackageFile)` 是打包映射表 `.txt`，不是产物；
  3. 真实 `AppxSignature.p7x` 带 4 字节魔数 `PKCX`，`X509Certificate2Collection.Import`
     读不了（`CRYPT_E_NOT_FOUND`），因此两端都必须自己走 DER。

## 原生安装器用法

```powershell
# 打印将要执行的动作（不碰系统）
msixpkg-native install App.msixpkg --pin <发布者SHA256> --require-pin --dry-run

# 安装；未命中钉扎白名单时默认拒绝，交互式前端会要求显式输入 trust
msixpkg-native install App.msixpkg --pin <发布者SHA256> --require-pin

# 卸载：先卸应用，再按四条判定决定是否回收证书
msixpkg-native uninstall --family-name <PFN>

# 运维
msixpkg-native list            # 本安装器装过什么
msixpkg-native verify          # 状态库 vs 实际系统是否一致
msixpkg-native repair          # 只输出修复建议，绝不擅自改动
msixpkg-native clean           # 清暂存残留与过期日志

# 提权助手（通常由安装器自己用 runas + SW_HIDE 拉起，不要手工跑）
msixpkg-native cert-add --container App.msixpkg --thumbprint-sha1 <sha1>
msixpkg-native cert-remove --thumbprint-sha1 <sha1>
```

退出码：`0` 成功 / `1` 未预期 / `2` 容器校验失败 / `3` 用法 / `4` I/O /
`5` 部署失败 / `6` 被取消（含拒绝提权）/ `7` 前置条件不满足。

## 仓库结构

```
src/
  Msixpkg.Core/            .msixpkg 读写、签名、校验、打包服务（netstandard2.0 + net9.0，零第三方依赖）
  Msixpkg.Packer/          打包器 CLI：pack / verify / inspect / extract / export-cert / dependencies
  Msixpkg.Build/           MSBuild 任务（Msixpkg.Tasks.dll）+ 可导入的 Msixpkg.Build.targets
  MsixpkgInstaller.Native/ C++20 原生安装器（零第三方依赖；CMake）
    cli/                   命令行前端：install / uninstall / list / verify / repair / clean
    gui/                   Win32 向导（五页）+ res/ 字符串表与图标
    src/                   容器读、校验器、certstore、deployment(C++/WinRT)、state、log、
                           installer 编排、ops、arp
    tests/                 自写极简测试框架 + 50 条原生测试
tests/
  Msixpkg.Core.Tests/      xunit v3 单元测试（126 条）
  vectors/                 两端共享的夹具（黄金容器、测试证书、合成 .msix、真实 p7x、
                           三个单缺陷负例向量）
  e2e/                     负例套件 + 完整安装闭环 + Windows Sandbox 配置
scripts/                   生成脚本（部署错误码表）
docs/
  msixpkg-format-v1.md     格式规范（规范性来源：条目白名单、摘要算法、30 步校验顺序、错误码）
  integration-winui3.md    真实 Packaged WinUI 3 工程的接入步骤与实测故障排查
  installer-design.md      原生安装器设计蓝图（进程模型、状态机、API、GUI 页面）
  security-model.md        威胁模型、钉扎、证书回收判定、审计字段
samples/
  SamplePackagedApp/       真实 WinUI 3 单项目 MSIX 工程（winapp new 生成并接入 Msixpkg.Build）
  MsixpkgBuildSample/      薄 MSBuild 接入样例（两个宿主下都能跑的最小验证）
```

## 构建与测试

本机实测环境：

| 项 | 版本 |
|---|---|
| OS | Windows 11 专业版 build 26300（x64） |
| VS | Visual Studio 2026 18.10.3 Community（NativeDesktop + ManagedDesktop + Win11 SDK 26100） |
| .NET SDK | 10.0.401（另有 9.0.100 / 10.0.100-preview5 / 10.0.100-rc1） |
| Windows SDK | 10.0.26100.0（`signtool.exe` / `makeappx.exe`） |
| PowerShell | **Windows PowerShell 5.1**（本机无 pwsh 7，所有脚本按 5.1 兼容编写） |
| cmake | 4.2.1（原生工程 CMake 备选方案） |

```powershell
# 构建全部托管项目
dotnet build MsixpkgInstaller.slnx

# 托管单元测试（xunit v3；测试是普通可执行程序，不经 VSTest testhost）
dotnet build tests\Msixpkg.Core.Tests
tests\Msixpkg.Core.Tests\bin\Debug\net9.0\Msixpkg.Core.Tests.exe

# 原生核心 + 安装器 + 原生测试（CMake + VS 2026 生成器；二进制目录 src\MsixpkgInstaller.Native\build）
cd src\MsixpkgInstaller.Native
cmake --preset windows-x64 -S .
cmake --build --preset windows-x64-debug
build\x64\Debug\msixpkg_native_tests.exe          # 期望：总计 50 成功 50
cmake --build --preset windows-x64-release        # Release 开 /WX，必须零警告
build\x64\Release\msixpkg_native_tests.exe

# 端到端（负例套件不需要管理员）
cd D:\work\source\repos\MsixpkgInstaller
powershell -ExecutionPolicy Bypass -File tests\e2e\msixpkg-negative.ps1
```

CI 见 [`.github/workflows/ci.yml`](.github/workflows/ci.yml)：托管 + 原生 + 负例 E2E 三段。
真实安装/卸载闭环需要管理员与真实签名包，按 [`tests/e2e/README.md`](tests/e2e/README.md) 在专用机器或
Windows Sandbox 里跑。

> 为什么用 xunit v3 的独立测试程序：VSTest 的 `testhost` 需要打开父进程句柄，
> 在受约束的执行环境里会被拒绝（`Win32Exception(5)`）。xunit v3 直接产出可执行测试程序，绕过该限制。
>
> 原生侧为什么用 CMake 而不是 vcxproj：M3 只依赖 Windows 自带库（`bcrypt` / `crypt32`）与自写代码，
> M4 起再加 `WindowsApp`（C++/WinRT 部署栈）与 `shell32`，**全程零第三方依赖**。
> C++/WinRT 投影头**由 Windows SDK 自带**（`<SDK>\Include\<ver>\cppwinrt\`），因此没有引入
> `Microsoft.Windows.CppWinRT` 包；CMake 从注册表 `KitsRoot10` 定位 SDK
> （不能写 `$ENV{ProgramFiles(x86)}` —— CMake 的 `$ENV{}` 名字不允许括号，会直接语法错误）。

重新生成黄金样本（仓库改为写入时才需要，普通测试运行只读）：见
[`tests/vectors/README.md`](tests/vectors/README.md)。

## 安全要点（勿跳过）

1. `.msixpkg` 会自动把内嵌证书写进**本机**受信任存储 —— 这等价于发放代码签名信任。
   因此安装器必须做**发布者指纹钉扎**（默认拒绝未命中白名单的发布者），只写
   `LocalMachine\TrustedPeople`，**永不写 `Root`**。
   白名单有三个来源：管理员部署的 `HKLM\SOFTWARE\MsixpkgInstaller\AllowedPublishers`、
   命令行显式 `--pin <sha256>`、以及用户自己勾选过「信任此发布者（将记住）」的记录
   （`%ProgramData%\Msixpkg\publishers.json`，删掉条目即可让安装器重新询问）。
2. 容器内必须四重身份一致：容器签名者 == `publisher.cer` == 内层 `.msix` 的
   `AppxSignature.p7x` 签名者 == Appx 清单的 `Publisher`。
3. 容器签名只做**密码学验证**，不依赖链信任（否则鸡生蛋）；链信任由钉扎 + Windows 部署栈提供。
4. 自签名证书只适合内部分发。公网分发请迁移到受信 CA / Azure Artifact Signing ——
   那时容器里没有 `publisher` 条目，安装器完全不碰证书存储，格式与安装器都无需改动。

细节见 [`docs/security-model.md`](docs/security-model.md)。
