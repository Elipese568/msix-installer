# 交接文档：现状、接口、坑与遗留工作

> 给新 session 的入口文档。**只读这一个文件就能接手**：里面包含现状、环境、如何构建与测试、
> 已经写好且**必须复用**的接口、还没做完的事，以及已经踩过的坑（不要重犯）。
>
> 本轮（M4–M7）的施工细节与新坑记录在
> [`installer-design.md`](installer-design.md) §10–§11；格式契约见
> [`msixpkg-format-v1.md`](msixpkg-format-v1.md)；威胁模型见 [`security-model.md`](security-model.md)。

- 目标产品：把 WinUI 3 应用打成单一 `.msixpkg`，再用原生安装器完成
  「导入发布者证书 → 安装 MSIX」「卸载应用 → 按引用计数回收证书」闭环。
- 安装器设计蓝图：[`installer-design.md`](installer-design.md)（M4–M6 的依据，已全部落地）。
- 真实工程接入：[`integration-winui3.md`](integration-winui3.md)。

---

## 0. 当前状态（一句话）

**M0–M7 全部完成**：托管侧（打包器 + MSBuild 任务 + 132 条测试）、
原生侧（容器读 + 校验器 + 证书存储 + 部署封装 + 状态库 + 编排 + CLI + Win32 向导，
**72 条原生测试**，Debug/Release 双配置零警告）、E2E 脚本与 CI 都已就位，
构建期打包工具也已经作为 `Msixpkg.Build` NuGet 包产出并通过消费者视角验收。

| 里程碑 | 内容 | 状态 |
|---|---|---|
| M0 | 工具链核对、工作区权限修复 | ✅ |
| M1 | 格式规范 v1 + `Msixpkg.Core` + 黄金样本 | ✅ 126 条托管测试（现 132 条） |
| M2 | `Msixpkg.Packer` CLI + `Msixpkg.Build`（MSBuild 任务）+ 真实 WinUI3 工程 | ✅ |
| M3 | 原生容器读 + 校验器 | ✅ 与 C# 摘要逐字节一致 |
| M4 | `certstore` + `deployment` + `state` + `log` + `interface.h` + CLI | ✅ |
| M5 | 引用计数 + `list/verify/repair/clean` + ARP 卸载入口 | ✅ |
| M6 | GUI 向导（**Wren 自绘**，`-DMSIXPKG_GUI_LEGACY=ON` 可回退老 Win32 控件版）+ `app.manifest` + `res/*.rc` | ✅ |
| M7 | Windows Sandbox E2E + CI + 文档定稿 | ✅ |
| 分发 | 自研原生 setup（`installer/setup/`）：把向导 + CLI + `Uninstall.exe` 装到用户机器，含快捷方式、`.msixpkg` 关联、PATH、ARP 卸载入口、自提权；另出便携 zip | ✅ 45 项 E2E + 90 项 GUI 烟测 + 72 条原生测试；注册表效果断言需在普通机器/CI 上跑（受限环境子进程写不了 HKCU） |
| NuGet | `Msixpkg.Build` 任务包（构建期自动产出 `.msixpkg`；Core 作为二进制附属随包） | ✅ MIT；25 项包验收（包布局 + PackageReference 消费 + 两个 MSBuild 宿主下与黄金样本逐字节一致）；发布走 `nuget-publish.yml`（tag / 手动触发，OIDC） |

**在本机能验证的范围内全部验证过**（详见 §5）；真实安装 / 卸载闭环在本机受限执行环境里
跑不了 —— 见 §5 第 3 条，脚本已经写好，需要在提权环境或 Windows Sandbox 里执行。

---

## 1. 环境事实（实测，不要凭经验假设）

| 项 | 值 |
|---|---|
| OS | Windows 11 专业版 **build 26300**（x64） |
| Visual Studio | **2026 18.10.3** Community，路径 `C:\Program Files\Microsoft Visual Studio\18\Community` |
| MSVC | 14.51.36231（另有 14.44.35207） |
| Windows SDK | 10.0.26100.0（`signtool.exe` / `makeappx.exe` / **自带 cppwinrt 投影头**） |
| .NET SDK | 10.0.401（另有 9.0.100）。**没有 .NET 8 SDK** |
| PowerShell | **只有 Windows PowerShell 5.1，没有 pwsh 7** |
| cmake | 4.2.1（识别 `Visual Studio 18 2026` 生成器） |
| winapp CLI | 0.6.1（`winapp new` / `winapp cert generate`） |
| 开发者模式 | 已开启 |
| 当前进程权限 | **非管理员**；HKCU / HKLM 的**注册表写**被拒（`ERROR_ACCESS_DENIED`），
`Windows.Management.Deployment` 的包仓库访问也被拒 |
| 未签名的 exe | 本机装了 **360 安全套装**（`Get-MpComputerStatus` 显示 Defender 实时保护为关）。
实测（同一段探针，父进程无论是 `pwsh` 还是计划任务，结果一致）：
**任何未签名 exe**（随手 `csc` 编的探针、早期 NSIS 安装包）在 `C:` 上**建不了临时文件**
（`GetTempFileName` → 错误 5 = ACCESS_DENIED），并且**写不了注册表**（HKCU/HKLM 都 Access denied）；
签名的 Microsoft 程序（`pwsh`/`cmd`）不受影响，`D:` 上的临时文件也不受影响。
**自研 setup 不再依赖 `%TEMP%`**（载荷从自己的 exe 里读），所以这个环境的双击安装是能装文件的；
但「安装记录 / ARP / 关联 / PATH」仍是 Access denied，会在日志里报 WARNING/FAILED。
断言必须由被测程序自己产生效果（见 `tests/e2e/setup-installer-e2e.ps1` 的 `registryEffective`），
别用「shell 能写」代替；要跑全注册表断言得给安装包加安全软件排除项或签受信任的证书。 |

**环境的三个硬限制（都踩过）**：

1. **`%TEMP%` 不可写**。所有需要临时目录的代码 / 脚本必须落在工作区内，
   例如 `artifacts/native-tests/`、`artifacts/e2e/`；数据根目录也实现了
   `MSIXPKG_DATA_ROOT → %ProgramData% → %LOCALAPPDATA% → ./artifacts/data` 的可写性探测回落链。
2. **VSTest 的 testhost 不可用**（需要打开父进程句柄，被拒 `Win32Exception(5)`）。
   托管测试用 **xunit v3 的可执行测试程序**，不要改用 `dotnet test`。
3. **注册表写与包仓库访问被拒**。因此：证书存储的单测用 `CERT_STORE_PROV_MEMORY`；
   状态库测试用 `MSIXPKG_DATA_ROOT`；真实安装/卸载必须换到提权环境或沙盒。

---

## 2. 如何构建与测试（复制即用）

```powershell
cd D:\work\source\repos\MsixpkgInstaller

# 托管：构建 + 测试（132 条）
dotnet build MsixpkgInstaller.slnx
dotnet build tests\Msixpkg.Core.Tests
tests\Msixpkg.Core.Tests\bin\Debug\net10.0\Msixpkg.Core.Tests.exe      # 期望：总计 132 成功 132

# 原生：构建 + 测试（72 条）。必须在原生工程目录里跑，CMakePresets 在那里
cd src\MsixpkgInstaller.Native
cmake --preset windows-x64 -S .
cmake --build --preset windows-x64-debug
build\x64\Debug\msixpkg_native_tests.exe                              # 期望：总计 72 成功 72
cmake --build --preset windows-x64-release                            # /WX 已开，必须零警告
build\x64\Release\msixpkg_native_tests.exe

# GUI：Wren 五页 + MSAA 无人值守烟测（不需要容器也能跑空态）
build\x64\Debug\msixpkg-installer.exe --ui-page summary `
  --ui-report artifacts\ui-report\summary.txt --exit-after 1500
# 期望 exit 0；报告里有 presenter= / frames= / childCount= / child[i] id= role=name=state=
powershell -ExecutionPolicy Bypass -File ..\..\tests\e2e\gui-smoke.ps1 `
  -GuiPath build\x64\Release\msixpkg-installer.exe
# 期望：checks 81, failures 0（断言按 resource.h 的 IDC_* 取控件，不依赖界面语言）
# 完整逐页截图（PrintWindow）与交互驱动见 §5；浅色/深色/HC 用 --ui-theme 强制。

# 老 Win32 控件版（回退路径）仍要能编：
cmake -S . -B build\legacy -G "Visual Studio 18 2026" -A x64 `
  -DMSIXPKG_GUI_LEGACY=ON -DMSIXPKG_WARNINGS_AS_ERRORS=ON
cmake --build build\legacy --config Debug --target msixpkg_native_gui

# 安装器 CLI（原生）
build\x64\Debug\msixpkg-native.exe help

# 打包器 CLI 自检（对黄金样本，必须逐字节复现）
cd D:\work\source\repos\MsixpkgInstaller
src\Msixpkg.Packer\bin\Debug\net10.0\msixpkg.exe verify tests\vectors\Msixpkg.Sample_1.0.0.0_x64.msixpkg `
  --host-arch x64 --host-build 26300 `
  --pin 9cd702035dd941aa6fe12870550ffc61633281da08839cfb0dfb3a95e1b913b9
# 期望：result valid，digest e67cf3483677ed9d4f33b23e2c0ff7e20ca2648124dd1f248772bea13513b4fc

# 原生侧对同一份黄金样本的看法必须一致（跨语言门禁）
src\MsixpkgInstaller.Native\build\x64\Debug\msixpkg-native.exe install `
  tests\vectors\Msixpkg.Sample_1.0.0.0_x64.msixpkg `
  --pin 9cd702035dd941aa6fe12870550ffc61633281da08839cfb0dfb3a95e1b913b9 --require-pin --dry-run
# 期望：打印计划并 exit 0（没有 true 就拒绝是正确行为）

# 端到端：负例套件（不需要管理员，几十秒）
powershell -ExecutionPolicy Bypass -File tests\e2e\msixpkg-negative.ps1
# 期望：checks 33, failures 0

# NuGet 包：打包 + 包布局 + PackageReference 消费 + 黄金样本逐字节比对（不需要管理员；只在本机跑）
powershell -ExecutionPolicy Bypass -File tests\e2e\nuget-package-e2e.ps1
# 期望：25 项检查全过（两个 MSBuild 宿主都产出 EA972886…A986；找不到 VS 时第二个宿主会跳过）

# 发布 Msixpkg.Build 到 nuget.org：由 CI 做（OIDC，无需长期密钥）
git tag v0.3.0; git push origin v0.3.0     # 或在 Actions 里手动 Run workflow（可 dry_run=true）
# 需要先在 nuget.org 建信任策略 + 仓库加 Secret NUGET_USER（已配好），见 docs/nuget-package.md §5

# 分发安装包（自研 setup）：构建 + 安装/卸载验收（都不需要管理员）
powershell -ExecutionPolicy Bypass -File scripts\build-setup.ps1
# 期望：产物 artifacts\setup\MsixpkgInstaller-0.3.0-setup.exe + .sha256 + portable.zip（/WX 零告警）
powershell -ExecutionPolicy Bypass -File tests\e2e\setup-installer-e2e.ps1
# 期望：checks 45, failures 0, skipped 2（本机受限环境跳过注册表断言，脚本会打印原因）
powershell -ExecutionPolicy Bypass -File tests\e2e\setup-gui-smoke.ps1
# 期望：checks 90, failures 0
```

真实安装 / 卸载闭环见 [`../tests/e2e/README.md`](../tests/e2e/README.md)（需要提权 + 真实签名包）。

---

## 3. 已经写好、**必须复用**的接口（不要重写）

### 3.1 托管侧（`src/Msixpkg.Core/`，`netstandard2.0` + `net10.0`，零第三方依赖）

| 类型 | 用途 |
|---|---|
| `MsixpkgManifest` / `MsixpkgIdentity` / `MsixpkgFileRef` / `MsixpkgPublisherInfo` | `msixpkg.json` 模型（严格 schema，未知成员拒绝） |
| `MsixpkgContainer` | 打开容器、`ReadEntryBytes/Text`、`ComputeEntrySha256`、`ComputeDigestEntries` |
| `MsixpkgValidator` | 30 步校验序列 → `MsixpkgValidationReport`（`Errors`/`Warnings`/`Manifest`/`PublisherCertificate`/`ContentDigestHex`） |
| `MsixpkgSigner` / `MsixpkgSignatureCodec` / `Thumbprints` | 容器签名与指纹 |
| `MsixpkgCertificates` | **PFX 加载的唯一入口**（见坑 #5） |
| `MsixpkgPacker` / `MsixpkgPackageBuilder` | `Pack(...)` / `Write(...)`：CLI 与 MSBuild 任务共用 |
| `Zip.StoredZipWriter` | **容器写出的唯一入口**（只写 store）。不要退回 `System.IO.Compression.ZipArchive`，见坑 #15 |
| `AppxPackageReader` / `Pkcs7CertificateReader` | 内层 `.msix` 的清单与签名者（含 `PKCX` 头） |
| `MsixpkgErrorCodes` | **错误码字符串，原生侧必须逐字一致** |

### 3.2 原生侧（`src/MsixpkgInstaller.Native/`，CMake，零第三方依赖）

| 文件 | 用途 |
|---|---|
| `include/msixpkg/errors.h` | `codes::*`（42 个容器格式错误码）+ `Error` 异常 |
| `src/internal.h` | 字节 / 十六进制 / 宽窄字符串 / 命令行参数引用（`quote_argument`）/ 文件读写 |
| `src/json.h` | 与 C# 语义一致的最小 JSON（含 `to_json` 规范化输出） |
| `src/inflate.h` / `src/zip.h` | RFC1951 raw DEFLATE；ZIP 中央目录 + `open_memory`（嵌套容器零拷贝） |
| `src/crypto.h` / `src/certs.h` | BCrypt 摘要与验签；crypt32 证书字段 + PKCS#7 证书集提取 |
| `src/container.h` / `src/appx.h` | 清单模型与校验、内层 `.msix` 身份 / 签名者 / `Application/@Id` |
| `src/verify.h` | **校验入口**：`ValidateOptions` / `Issue` / `Report` / `validate(...)` |
| `src/certstore.h` | `LocalMachine\TrustedPeople` 的增 / 查 / 删（位置是枚举，不可能写出 `Root`） |
| `src/deployment.h` | C++/WinRT 部署栈封装（**头文件不含任何 Windows/WinRT 头**，纯函数层可单测） |
| `src/deployment_errors.generated.h` | 由 `scripts/generate-deployment-errors.ps1` 从 SDK `winerror.h` 生成 |
| `src/state.h` | 状态库（原子写、可写性回落链、`InstallationRecord`） |
| `src/log.h` | 追加式日志（主进程与提权助手共写同一份） |
| `src/interface.h` | `IProgressSink` / `IConsentProvider` / `ICancelSource`（**前端契约，GUI 只依赖它**） |
| `src/installer.h` | 编排 + `evaluate_certificate_reclaim`（证书回收四条判定，verify/repair/GUI 复用） |
| `src/arp.h` | 「设置 → 应用」卸载入口（HKLM 优先、HKCU 回落） |
| `src/policy.h` | 发布者钉扎策略：机器级白名单（注册表两种布局）+ TOFU 记忆（`<数据根>\publishers.json`） |
| `src/ops.h` | `list` / `verify` / `repair` / `clean` 的实现（**repair 只输出建议**） |
| `src/installer.h` 的 `Installer::plan()` | **只读预演**：一次给出「校验结果 + `ConsentRequest` + 动作清单」，零副作用；dry-run / 摘要页 / 同意页共用 |
| `cli/main.cpp` | 八个子命令 + 退出码 0–7（GUI 之外的另一种前端） |
| `gui/` + `res/` + `app.manifest` | **Wren 自绘向导**（SUMMARY / CONSENT / PROGRESS / RESULT / MANAGE）：`gui_wizard_wren.cpp` + `gui_widgets.cpp`；单 HWND、绝对 DIP 布局、Host 负责焦点/键盘/DPI/MSAA；窗口级消息用 `SetWindowSubclass`。`-DMSIXPKG_GUI_LEGACY=ON` 回退 `gui_wizard.cpp`（标准控件 + `IsDialogMessage`） |

### 3.3 夹具（`tests/vectors/`，两端共享）

| 文件 | 说明 |
|---|---|
| `Msixpkg.Sample_1.0.0.0_x64.msixpkg` | 黄金容器（内容摘要 `e67cf348…b4fc`） |
| `golden-msixpkg.json` | 抽出的清单，用于两端序列化**逐字节重放** |
| `test-only.pfx` | 测试证书（密码 `msixpkg-test-only`），SHA-256 `9cd70203…13b9` |
| `sample-main.msix` / `sample-dependency.msix` | 合成主包 / 依赖包 |
| `real-appxsignature.p7x` | **真实** Appx 签名（`PKCX` + DER，签名者 `CN=AppPublisher`） |
| `deflate-sample.zip` | DEFLATE 夹具 |
| `negative-unsigned.msixpkg` / `negative-expired.msixpkg` / `negative-wrong-eku.msixpkg` | 三个**单缺陷**负例向量（重新生成见 `tests/vectors/README.md`） |

---

## 4. 还没做的事（下一轮的候选）

按优先级：

1. **在真实机器上跑一次完整闭环**：`tests/e2e/msixpkg-e2e.ps1` 需要提权 + 一份真实签名包；
   本机受限环境跑不了（见 §5）。这是唯一没有端到端验证过的部分。
2. **GUI 的真实安装/卸载闭环**：Wren 版五页已经用合成数据 + PrintWindow + MSAA 驱动逐页验收过
   （含同意页安全门、结果页三种形态、进度时间线、浅色/深色/高对比度）；真正开始安装后的
   PROGRESS → RESULT 切换仍要在一台允许 MSIX 部署的机器上过一遍。150% / 200% DPI 也还值得人工看。
3. ~~`Msixpkg.Build` 打成真正的 NuGet 包（目前是仓库内相对路径 `Import`；跨仓库分发才需要）。~~
   **已完成**：`src/Msixpkg.Build` 产出 `Msixpkg.Build` 包（MIT；`build\Msixpkg.Build.targets` +
   `tasks\netstandard2.0\{Msixpkg.Tasks.dll, Msixpkg.Core.dll}`，Core 作为二进制附属随包分发，
   不声明成 NuGet 依赖）。`Msixpkg.Core` 与 `Msixpkg.Packer` 都不再 `IsPackable`：前者随包走，
   后者是开发机 / CI 的构建期 CLI。本地验收：`scripts\pack-nuget.ps1` +
   `tests\e2e\nuget-package-e2e.ps1`（见 [`nuget-package.md`](nuget-package.md)）。
   **发布流水线已接并已发出版本**：[`.github/workflows/nuget-publish.yml`](../.github/workflows/nuget-publish.yml)
   用 nuget.org 的 **Trusted Publishing（OIDC）**，由 tag `v*` 或手动 `workflow_dispatch` 触发，
   仓库里不存长期密钥。`v0.1.0` 已于 2026-10-17 发布：
   <https://www.nuget.org/packages/Msixpkg.Build/0.1.0>（发布记录
   [run 38944111342](https://github.com/Elipese568/msix-installer/actions/runs/38944111342)）。
   `v0.3.0` 于 2026-10-31 发布：<https://www.nuget.org/packages/Msixpkg.Build/0.3.0>
   （同一次 tag 也挂了 GitHub Release 的 setup.exe / portable.zip / dist.zip）。
   下一次发布只需 `git tag v<新版本> && git push origin v<新版本>`（记得同步改
   `Directory.Build.props` 的 `Version`、`res/msixpkg.rc` 的 `FILEVERSION`、
   `src/installer.h` 的 `kInstallerVersion`，以及 `samples\MsixpkgPackageSample` 里钉的版本）。
   **包的验收（那 25 项）刻意只留在本机脚本里，不占 CI。**
4. ~~`.msixpkg` 文件关联（`HKLM\Software\Classes`）与「双击安装」体验。~~
   **已完成**：`.msixpkg` 关联（ProgID `Msixpkg.Package` → `msixpkg-installer.exe "%1"`，
   双击停在摘要页，绝不自动安装）由分发安装包 `installer/setup/` 负责，
   连同快捷方式、PATH 与「设置 → 应用」条目，见 [`installer/setup/README.md`](../installer/setup/README.md)。
5. `--all-users` 的完整验证（需要管理员 + 系统卷；`ProvisionPackageForAllUsersAsync` 已实现）。
6. `ExpectedDigests`（Windows 11 23504+ / UniversalApiContract v15）作为 TOCTOU 的第二道保险；
   目前用「部署前重算 SHA-256 + 暂存目录收紧 ACL」等价覆盖。
7. 本地化完整性检查：`res/strings.rc` 的 zh-CN / en-US 两套字符串需要逐条比对（缺一条会回退到英文）。
8. 无障碍复核：Wren 版已通过真实 MSAA 客户端验证根对象 + 子对象（角色/名字/状态/默认动作），
   Tab 链、Ctrl+A/C、Table 行语义由 Host 提供；但本机 UIA 客户端（`AutomationElement.FromHandle`）
   仍看不到子元素 —— 这在老标准控件版也一样，可能是本机/会话的 UIA 桥限制，需要换一台普通桌面
   机器用 Accessibility Insights 复核。`--ui-report` 导出的是 Host 内部的 MSAA 条目；真实
   `WM_GETOBJECT` 路径用 oleacc 查询单独验证过（见 §6.4）。

---

## 5. 本轮验证到了什么程度（诚实清单）

| 项 | 状态 |
|---|---|
| 托管 132 条测试 | ✅ 全绿 |
| 原生 72 条测试（Debug / Release，`/WX`） | ✅ 全绿、零警告 |
| 原生 CLI 对黄金样本的校验（跨语言门禁） | ✅ `--pin --require-pin --dry-run` exit 0 |
| 负例 E2E（`tests/e2e/msixpkg-negative.ps1`） | ✅ 33 项检查全过（含「dry-run 连目录都不建」） |
| `Msixpkg.Build` NuGet 包：包布局 / 元数据 / PackageReference 消费 / 两个 MSBuild 宿主下与黄金样本**逐字节相同** / 全部条目 store | ✅ `tests/e2e/nuget-package-e2e.ps1`：checks 25, failures 0 |
| 包发布到 nuget.org | ✅ **`Msixpkg.Build 0.3.0` 已发布**（<https://www.nuget.org/packages/Msixpkg.Build/0.3.0>）；首发 `0.1.0`：tag `v0.1.0` → `nuget-publish.yml` → OIDC 临时 key → `dotnet nuget push` 全部成功（[run 38944111342](https://github.com/Elipese568/msix-installer/actions/runs/38944111342)）；发布前的本机验收 25/25 |
| GUI（Wren）：五页渲染 | ✅ `--ui-page` + PrintWindow 截图：SUMMARY / CONSENT / PROGRESS / RESULT / MANAGE；空态、载入成功、深色/浅色/高对比度（`--ui-theme`）都看过 |
| GUI（Wren）：同意页的安全门 | ✅ 用真实 MSAA 驱动：未勾「我了解并同意」→ 安装按钮禁用、信任框禁用；勾同意 → 信任框可用但安装仍禁用（未钉扎）；再勾「信任此发布者」→ 安装才可用 |
| GUI（Wren）：同意页流程与复制 | ✅ 一次「下一步」直接进入安装（`preapproved_consent` 生效）；详情 `TextView` 可 Tab 聚焦、Ctrl+A/C；未钉扎警告在复选框上方回流 |
| GUI（Wren）：进度页时间线 / 动画 | ✅ `--ui-progress 42`：前两阶段对勾、第三阶段进行中、进度条平滑到 42%、日志可见；`--ui-progress 100`：全部完成、无障碍值 100% |
| GUI（Wren）：结果页三种形态 | ✅ `--ui-result success/failure/cancelled`：标题、状态徽记、正文、日志行、按钮 enabled 状态正确；卸载成功另有「卸载完成」文案 |
| GUI（Wren）：MSAA | ✅ `AccessibleObjectFromWindow(OBJID_CLIENT)`：根 + 子对象可枚举，角色/名字/状态/默认动作正确；真实 `WM_GETOBJECT` 路径有一次性请求计数 |
| GUI（Wren）：UIA 桥 | ⚠️ 本机 `AutomationElement.FromHandle` 仍看不到子元素（老标准控件版同样如此），需换普通桌面机器用 Accessibility Insights 复核 |
| GUI（Wren）：真实安装 / 卸载闭环 | ⚠️ 未在本机跑（包仓库访问被拒）；PROGRESS → RESULT 的真实切换待提权环境 |
| 老 Win32 控件版（`-DMSIXPKG_GUI_LEGACY=ON`） | ✅ 仍可编译（Debug，`/WX`），只作为逐页回退路径 |
| 证书存储真实增删（`LocalMachine\TrustedPeople`） | ⚠️ **未在本机跑**（需要管理员；单测用内存存储覆盖同一套 CRUD 逻辑） |
| 真实 MSIX 安装 / 卸载闭环 | ⚠️ **未在本机跑**（包仓库访问被拒；脚本已写好，需提权环境） |
| ARP 条目真实写入 | ⚠️ **未在本机跑**（HKLM / HKCU 注册表写都被拒） |
| 钉扎白名单（注册表）+ TOFU 记忆 | ⚠️ 注册表侧未在本机跑（写不了 HKLM）；**记忆与合并逻辑有单测**（`policy_tests.cpp`） |
| setup 分发安装包：静默安装 / `/D=` / 载荷逐字节一致 / 日志 / 重复安装 / 组件开关 / `/ALLUSERS` 未提权退 5 / 不可创建目标退 4 / 两种卸载路径 / 安装记录不被删 / 无 `%TEMP%` 安装 / GUI 自动安装卸载 | ✅ `tests/e2e/setup-installer-e2e.ps1`：checks 45, failures 0（本机；注册表断言 SKIP） |
| setup 分发安装包：安装记录 / ARP / `.msixpkg` 关联 / PATH 的**写入与清理** | ⚠️ 本机跑不了（受限环境不允许**子进程**写 HKCU）；套件会打印 SKIP 并改为断言「安装器如实上报了这些失败」，真实断言由 CI 的 `setup-package` 作业在普通机器上跑 |
| setup / Uninstall.exe 页面与无障碍 | ✅ `tests/e2e/setup-gui-smoke.ps1`：90 项（页面、控件 ID、状态、主题、帧统计、底部按钮真实鼠标点击） |

> 这不是「没测」而是「本机测不了」：受限执行环境拒绝了注册表写与包仓库访问。
> 代码里对这两种拒绝都有明确的上报路径（`guard_winrt` → 稳定错误码 + 可操作提示），
> 并且负例套件专门验证了「失败必须可读地上报，不能静默消失」。

---

## 6. 已知坑（**不要重犯**）

### 6.1 M0–M3 期间踩的（仍然有效）

| # | 坑 | 正确做法 |
|---|---|---|
| 1 | PowerShell 5.1 的 `Get-Content -Raw` 按 ANSI 读 UTF-8 源文件 | 中文注释会变乱码、GBK 双字节吃掉 CR 导致**注释与代码行合并**。改文件用 `read`/`edit` 工具；必须用 PS 时显式 `[IO.File]::ReadAllText(path, [Text.Encoding]::UTF8)` + `WriteAllText(..., UTF8Encoding($false))` |
| 2 | XML 注释里出现 `--` | MSBuild 直接报 `MSB4025` |
| 3 | `%TEMP%` 不可写 | 临时文件放 `artifacts/…`；代码用可写性探测回落 |
| 4 | `EphemeralKeySet` 枚举值 | 是 **`0x20`**，不是 `0x2000` |
| 5 | PFX 加载 | 一律走 `MsixpkgCertificates.LoadSigningCertificateFromFile`（含候选标志组合 + 私钥可用性探测）；.NET 9 上必须走 `X509CertificateLoader` 且**不能**用 `Exportable\|EphemeralKeySet` |
| 6 | `BCryptCreateHash` 的哈希对象缓冲区 | 必须是**成员**（要活到哈希结束）；放构造函数局部变量会得到「句柄有效但 `BCryptHashData` 失败」 |
| 7 | `BCryptVerifySignature` + PKCS#1 | 必须传 `BCRYPT_PKCS1_PADDING_INFO{SHA256}`；传 NULL → `STATUS_INVALID_PARAMETER (0xC000000D)` |
| 8 | `f(ptr->data(), ptr->size(), std::move(ptr))` | 实参求值顺序未定义；先取裸值再 move，否则会在空指针上取值（`0xC0000005`） |
| 9 | 真实 `AppxSignature.p7x` | 前 4 字节是魔数 `PKCX`，其后才是 DER；`X509Certificate2Collection.Import` 读不了 |
| 10 | MSBuild 挂载点 | `GenerateAppxPackage` 目标**不存在**；真实目标是 `_GenerateAppxPackage`，产物路径是属性 `$(AppxPackageOutput)` |
| 11 | CRC vs 哈希 | 翻转条目字节时原生侧报 `E_CONTAINER_INVALID`（ZIP CRC），C# 报 `E_HASH_MISMATCH`；**GUI 必须都当完整性失败** |
| 12 | `Platform=x64` 会传播给被引用工程 | 用 `AppendPlatformToOutputPath=false` 修正 |
| 13 | .NET 无 .NET 8 SDK | 托管目标用 `net10.0`；`Msixpkg.Core` 多目标 `netstandard2.0;net10.0` |
| 14 | 托管测试运行器 | 用 xunit v3 可执行程序；`dotnet test`（VSTest）在本机不可用 |
| 15 | **`ZipArchive` + `CompressionLevel.NoCompression` 在两个宿主上语义不同** | .NET (Core) 写 `method 0`（store），**.NET Framework 写 `method 8`**（deflate 里包 stored block，每条目 +5 字节）。于是同一个工程 `dotnet build` 与 VS 构建出的 `.msixpkg` 字节不同 —— 而规范要求写出端必须 store。容器写出必须走 `Zip.StoredZipWriter`，别退回 `ZipArchive` |
| 16 | 新增带中文的 `.ps1` 没有 UTF-8 **BOM** | Windows PowerShell 5.1 对无 BOM 文件按 ANSI（GBK）解析，中文注释会被拆成半个字符，直接**语法错误**。新建脚本后确认前三字节是 `EF BB BF`（`write`/`edit` 工具不会保留 BOM，改动后要重新补） |
| 17 | CI 里写死 CMake 生成器 `-G "Visual Studio 17 2022"` | 托管运行器会换镜像：`windows-latest` 自 2026-06 起是 Windows Server 2025 + **VS 2026**，写死的名字直接 `could not find any instance of Visual Studio`。用 `scripts\detect-vs-generator.ps1`（vswhere 探测主版本 → 生成器名，写进 `$GITHUB_ENV`），三个原生作业共用 |
| 18 | 自附加载荷与签名的顺序 | Authenticode 覆盖整个文件：**先 pack 再 sign**。反过来会让签名在追加载荷的瞬间失效；`build-setup.ps1` 把签名放在打包之后 |
| 19 | **GUI 的提权助手用了 `current_executable_path()`** | GUI（`msixpkg-installer.exe`）会把自己以 `runas` 拉起当 `cert-add` 助手，而它不认识这个子命令 → 走进单实例交接 → 与等提权返回的父进程**三方互等死锁**：GUI 永久「未响应」，日志停在 `elevating: …`，证书永远导不进去（`LocalMachine\TrustedPeople` 里始终没有那张证书）。助手必须是**同目录的 CLI**：`helper_executable_path()`；只有 `msixpkg-native.exe` 自己运行时才用自己。另外那行日志以前硬编码打印 `msixpkg-native`，会把这类错误盖住 —— 现在打印真实路径 |
| 20 | **在 `WM_COPYDATA` / 拖放处理里等编排线程** | 发送方是同步 `SendMessage`，它等我们返回；我们等编排线程；编排线程可能正等那个发送方退出。规则：**消息处理里绝不阻塞 UI 线程** —— 忙碌时回 `kCopyDataBusy`（这个常量以前定义了却从没被回过），发送端用 `SendMessageTimeoutW(SMTO_ABORTIFHUNG\|SMTO_BLOCK, 10s)` 并检查返回码 |
| 21 | **`peek_data_roots()` 与 `resolve_data_roots()` 选的根可能不同** | 预演按「目录已存在」挑根、安装按「可写」挑根。在「`%ProgramData%\Msixpkg` 存在但普通用户不可写」的机器上，TOFU 记录写进 `%LOCALAPPDATA%\Msixpkg\publishers.json` 却从 `%ProgramData%` 读 → 「信任此发布者（将记住）」**永远不生效**，每次安装都要求重勾（日志里始终是 `pinning: NOT pinned`）。读「记住的发布者」要合并所有已存在的根（`publisher_policy_roots()`）；`MSIXPKG_DATA_ROOT` 覆盖时只认那一个根 |
| 22 | **ARP 的 `UninstallString` 是 GUI 的命令行** | 它是 `"<安装器路径>" uninstall --family-name <PFN>`（`arp.cpp` 用 `current_executable_path()` 拼的）。GUI 不认 `uninstall` 时会把它当容器路径 → 用户点「卸载」只看到一个 `E_IO`。GUI 现在解析 `uninstall [--family-name\|--install-id]`；其余 CLI 专属子命令（`cert-add` / `cert-remove` / `list` / `verify` / `repair` / `clean`）一律**立刻 exit 3**，绝不落到「容器路径」上（那条路就是坑 19/20 的死锁入口）。诊断写父控制台（`AttachConsole`），**不许弹 MessageBox**：被 `SW_HIDE` 拉起时它是个看不见、又永不消失的模态框 |
| 23 | **「看不到 UAC」先看策略，不要先怀疑代码** | `HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Policies\System\ConsentPromptBehaviorAdmin = 0` 是「**不提示直接提升**」（[MS-GPSB] 2.2.11.2），`EnableLUA=0` 更是彻底没有 UAC —— 这两种机器上 `ShellExecuteEx(runas)` **永远不会有窗口**，助手静默提权跑起来。本机就是策略 0（`PromptOnSecureDesktop=0`），所以 10-17 那三次「卡在 `elevating:` 且没有 UAC」是**旧 GUI 把自己当助手**的死锁，不是提权被拦。现在日志会先打一行 `elevation policy: EnableLUA=… ConsentPromptBehaviorAdmin=…`；`ShellExecuteEx` 返回 `ERROR_ACCESS_DENIED` 会被单独记成 `elevation blocked before any UI`（策略/AppLocker 在弹窗前就拒），与「启动失败」区分开。另：策略把确认框放在普通桌面（`PromptOnSecureDesktop=0`）时窗口可能开在向导后面，所以提权前 GUI 会 `SetForegroundWindow` + `FlashWindowEx` 提示用户 |

### 6.2 M4–M7 期间新踩的

**完整表见 [`installer-design.md`](installer-design.md) §11**，其中最要命的两条：

- `winrt::hresult_error` **不继承 `std::exception`**：逃出部署层后进程会静默 `abort`
  （退出码 3、stdout 缓冲来不及 flush、零输出）。部署层所有对外函数都包在 `guard_winrt` 里。
- `AddPackageByUriAsync` 会返回 **S_OK 但操作指针为 null**，对空对象调 `.Progress(...)`
  直接访问违例（`0xC0000005`，`/EHsc` 下 `catch(...)` 也接不住）。用 `require_operation()` 先验有效。

### 6.3 分发安装包（自研 setup）期间新踩的

完整设计见 [`installer/setup/README.md`](../installer/setup/README.md)。最容易再犯的几条：

- **注册表 API 直接返回 `LSTATUS`，不设置 `GetLastError`**：一开始用 `GetLastError()` 格式化
  `RegCreateKeyExW` 失败，结果把上一条无关调用的 183/6 当成了真实原因（实际是 Access denied=5）。
  所有 `Reg*` 错误都必须用返回值。
- **诊断页不能触发真实动作**：`--ui-page finish --ui-result success` 曾让完成页的
  「运行安装器」复选框生效，`ShellExecute` 拉起一个向导把测试卡住。诊断/自动模式必须显式
  标记 `diagnostics_`，并跳过「运行安装器 / 自删除」这类副作用。
- **UIA 的 InvokePattern 对 Wren 自绘控件是 no-op**：`winapp ui invoke btn-0011` 会返回成功，
  但按钮的 `on_click` 不会跑（没有子 HWND）。GUI 集成测试用 `/AUTO` 让页面自己执行主动作，
  不要假装点击成功。
- **自删除不能同步做**：正在运行的 exe 删不掉自己。卸载器先把自己改名成
  `~uninstall-<pid>.tmp`，再起一个 detached `cmd`（`ping` 一秒后 `del` + `rmdir`），
  进程退出后由它收尾。
- **日志目录要有覆盖入口**：受限环境里 `%LOCALAPPDATA%` 写不了、`%TEMP%` 也可能不可用；
  `--log-dir` / `MSIXPKG_SETUP_LOG_DIR` 让 E2E 把日志固定在 workspace 里。

### 6.4 Wren 迁移期间新踩的

- **页面 Panel 铺满客户区会吃掉底部按钮的点击**：`Panel::hit_test` 先问子节点、再问自己的
  `bounds`，而页面是后添加的（在 z 序上层）。setup 第一版把每页设成 760×560，结果底部
  「安装/取消/上一步」看得到、点不到（键盘 Enter 仍然能用，所以烟测没发现）。页面的
  `bounds` 必须止于 footer 上方（setup 用 0..496，footer 从 502 开始），
  `setup-gui-smoke.ps1` 现在会真的发 `WM_LBUTTONDOWN/UP` 点「取消」来钉住这条。

- **硬件 Canvas 没有文字度量后端**：`Host::render` 只给 GDI 帧的 Canvas 设了
  `text_metrics`，D2D 硬件 Canvas 的 `measure_text_width` 于是退回 GDI+，而它没有
  `Gdiplus::Graphics` —— 任何非空 `TextView` 第一帧就崩在 `Gdiplus::MeasureString`
  （`0xC000041D` / `0xC0000005`，Event Log 的 fault offset 落在 gdiplus）。
  修法是在 `Host::render` 里对两种 Canvas 都 `set_text_metrics(&text_engine_)`。
  症状很容易误判成「某个页面的控件坏了」：SUMMARY/PROGRESS 能亮，CONSENT/RESULT/MANAGE 黑屏。
- **禁用的主按钮仍画强调色描边**：`Button::paint` 只看 `primary_` 不看 `enabled`，于是
  结果页失败态的「启动应用」看起来像拿到了焦点。先判 `!enabled` 再走主/次描边。
- **MSAA 只接受 `CHILDID_SELF`**：根对象的 `validate()` 把 `get_accName(1)` 直接判
  `E_INVALIDARG`，只支持「先 `get_accChild(1)` 拿子对象再问 `CHILDID_SELF`」。
  用 oleacc 直接查子对象能过，但 UIA 的 MSAA 桥/部分客户端用「简单子 id」形式就枚举不到。
  现在根对象同时接受 `1..childCount` 与 `CHILDID_SELF`，`entry_for()` 负责解析。
- **截图**：DComp 窗口不能用 `CopyFromScreen`（本机会抓到桌面）；用
  `PrintWindow(hwnd, dc, PW_RENDERFULLCONTENT=2)` 进 `Bitmap` 再存 PNG。GDI 回退路径同样适用。
- **UIA 客户端在本机看不到子元素**：MSAA 根/子对象都正常、UIA 桌面树也正常，但
  `AutomationElement.FromHandle` 对本窗口返回 0 个 descendant。老标准控件版同样表现，
  因此先按「本机/会话的桥限制」记录，别据此断定 MSAA 没实现。
- **主窗口不在可见桌面上时**：`IsWindowVisible`/`GetWindowRect`/`PrintWindow` 都正常，
  但屏幕抓图与 UIA 桌面树找不到它。这是受限执行环境的窗口站问题，不是布局 bug。

---

## 7. 安全红线（实现时不得妥协）

1. **绝不写证书存储的 `Root`**，只写 `LocalMachine\TrustedPeople`；`publisher.cer` 缺失时完全不碰证书存储。
2. **发布者指纹钉扎**：未命中白名单默认拒绝；只有「同意提供方明确批准 **且** 勾选信任此发布者」
   才继续，并写审计日志。
3. **四重身份交叉校验**：容器签名者 == `publisher.cer` 指纹 == 内层 `AppxSignature.p7x` 签名者指纹
   == Appx 清单 `Publisher`。
4. **卸载顺序**：先 `RemovePackageAsync`，再按四条判定决定是否删证书；`existed_before == true` **永不删**。
5. **提权只用于证书**：主进程 `asInvoker`，只有 `cert-add` / `cert-remove` 子命令走高 UAC 且 `SW_HIDE`；
   已提权时在进程内直接做，不再弹 UAC。
6. **`--allow-unsigned` 只在 `#ifdef MSIXPKG_DEV` 下编译**（Release 构建不存在该路径）。
7. **TOCTOU**：暂存目录 ACL 仅 Administrators + SYSTEM；部署前用
   `ValidateOptions::extracted_main_package_path` 复检大小与 SHA-256。
8. **回滚只删本次创建的物**：依赖包（框架包共享）不回滚；包**已注册**之后绝不回滚证书
   （否则刚装好的应用会失去信任），只记 `partial` 交给 `repair`。
9. **`repair` 只输出建议**，绝不擅自改动系统。
