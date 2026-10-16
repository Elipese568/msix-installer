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
**58 条原生测试**，Debug/Release 双配置零警告）、E2E 脚本与 CI 都已就位，
构建期打包工具也已经作为 `Msixpkg.Build` NuGet 包产出并通过消费者视角验收。

| 里程碑 | 内容 | 状态 |
|---|---|---|
| M0 | 工具链核对、工作区权限修复 | ✅ |
| M1 | 格式规范 v1 + `Msixpkg.Core` + 黄金样本 | ✅ 126 条托管测试（现 132 条） |
| M2 | `Msixpkg.Packer` CLI + `Msixpkg.Build`（MSBuild 任务）+ 真实 WinUI3 工程 | ✅ |
| M3 | 原生容器读 + 校验器 | ✅ 与 C# 摘要逐字节一致 |
| M4 | `certstore` + `deployment` + `state` + `log` + `interface.h` + CLI | ✅ |
| M5 | 引用计数 + `list/verify/repair/clean` + ARP 卸载入口 | ✅ |
| M6 | Win32 GUI 向导 + `app.manifest` + `res/*.rc` | ✅ |
| M7 | Windows Sandbox E2E + CI + 文档定稿 | ✅ |
| 分发 | NSIS 安装包（`installer/nsis/`）：把向导 + CLI 装到用户机器，含快捷方式、`.msixpkg` 关联、PATH、ARP 卸载入口；另出便携 zip（`%TEMP%` 写不了的容器/沙箱用） | ✅ 46 项 E2E + PATH 逻辑 13 个 NSIS 用例；注册表效果断言需在普通机器/CI 上跑（受限环境子进程写不了 HKCU） |
| NuGet | `Msixpkg.Build` 任务包（构建期自动产出 `.msixpkg`；Core 作为二进制附属随包） | ✅ MIT；25 项包验收（包布局 + PackageReference 消费 + 两个 MSBuild 宿主下与黄金样本逐字节一致）；发布到 nuget.org 待接 |

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
**任何未签名 exe**（NSIS 安装包、随手 `csc` 编的探针）在 `C:` 上**建不了临时文件**
（`GetTempFileName` → 错误 5 = ACCESS_DENIED），并且**写不了注册表**（HKCU/HKLM 都 Access denied）；
签名的 Microsoft 程序（`pwsh`/`cmd`）不受影响，`D:` 上的临时文件也不受影响。
后果：NSIS 安装包默认双击必然弹 `Error writing temporary file`（stub 是未签名 exe，`%TEMP%` 在 C:），
把 `TEMP/TMP` 指到工作区就能装文件，但「安装记录 / ARP / 关联 / PATH」仍在日志里报 WARNING/FAILED。
所以断言必须由被测程序自己产生效果（见 `tests/e2e/nsis-installer-e2e.ps1` 的 `registryEffective`），
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

# 原生：构建 + 测试（58 条）。必须在原生工程目录里跑，CMakePresets 在那里
cd src\MsixpkgInstaller.Native
cmake --preset windows-x64 -S .
cmake --build --preset windows-x64-debug
build\x64\Debug\msixpkg_native_tests.exe                              # 期望：总计 58 成功 58
cmake --build --preset windows-x64-release                            # /WX 已开，必须零警告
build\x64\Release\msixpkg_native_tests.exe

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

# NuGet 包：打包 + 包布局 + PackageReference 消费 + 黄金样本逐字节比对（不需要管理员）
powershell -ExecutionPolicy Bypass -File tests\e2e\nuget-package-e2e.ps1
# 期望：25 项检查全过（两个 MSBuild 宿主都产出 EA972886…A986；找不到 VS 时第二个宿主会跳过）

# 分发安装包（NSIS）：构建 + 安装/卸载验收（都不需要管理员）
powershell -ExecutionPolicy Bypass -File scripts\build-nsis-installer.ps1
# 期望：产物 artifacts\setup\MsixpkgInstaller-0.2.0-setup.exe + .sha256（/WX 零告警）
powershell -ExecutionPolicy Bypass -File tests\e2e\nsis-installer-e2e.ps1
# 期望：checks 46, failures 0, skipped 2（本机受限环境跳过注册表断言，脚本会打印原因）
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
| `gui/` + `res/` + `app.manifest` | Win32 向导（SUMMARY / CONSENT / PROGRESS / RESULT / MANAGE）；标准控件 + WS_TABSTOP + IsDialogMessage 键盘导航 |

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
2. **GUI 剩下三处的实地验收**（已在模拟输入下验证过安全门，见 §5）：
   PROGRESS / RESULT 两页要到得了才能看，而到达它们必须真的开始安装；
   另外值得人工过一遍 150% / 200% DPI 与「取消 UAC 后回到同意页」。
3. ~~`Msixpkg.Build` 打成真正的 NuGet 包（目前是仓库内相对路径 `Import`；跨仓库分发才需要）。~~
   **已完成**：`src/Msixpkg.Build` 产出 `Msixpkg.Build` 包（MIT；`build\Msixpkg.Build.targets` +
   `tasks\netstandard2.0\{Msixpkg.Tasks.dll, Msixpkg.Core.dll}`，Core 作为二进制附属随包分发，
   不声明成 NuGet 依赖）。`Msixpkg.Core` 与 `Msixpkg.Packer` 都不再 `IsPackable`：前者随包走，
   后者是开发机 / CI 的构建期 CLI。本地验收：`scripts\pack-nuget.ps1` +
   `tests\e2e\nuget-package-e2e.ps1`（见 [`nuget-package.md`](nuget-package.md)）。
   **发到 nuget.org 的流水线还没接** —— 用户明确「后面再做」。
4. ~~`.msixpkg` 文件关联（`HKLM\Software\Classes`）与「双击安装」体验。~~
   **已完成**：`.msixpkg` 关联（ProgID `Msixpkg.Package` → `msixpkg-installer.exe "%1"`，
   双击停在摘要页，绝不自动安装）由分发安装包 `installer/nsis/` 负责，
   连同快捷方式、PATH 与「设置 → 应用」条目，见 [`installer/nsis/README.md`](../installer/nsis/README.md)。
5. `--all-users` 的完整验证（需要管理员 + 系统卷；`ProvisionPackageForAllUsersAsync` 已实现）。
6. `ExpectedDigests`（Windows 11 23504+ / UniversalApiContract v15）作为 TOCTOU 的第二道保险；
   目前用「部署前重算 SHA-256 + 暂存目录收紧 ACL」等价覆盖。
7. 本地化完整性检查：`res/strings.rc` 的 zh-CN / en-US 两套字符串需要逐条比对（缺一条会回退到英文）。
8. 无障碍复核：控件都是标准类（`STATIC`/`BUTTON`/`PROGRESS_CLASSW`/`WC_LISTVIEWW` + `WS_TABSTOP`），
   但在本次的 UIA 客户端里它们统一被报成 `ControlType.Pane`（名字与 enabled 正确）。
   这**可能**只是本机 MSAA→UIA 桥的客户端问题，也可能需要显式提供 UIA provider；
   结论要用 Accessibility Insights / Inspect 这类工具在另一台机器上确认。

---

## 5. 本轮验证到了什么程度（诚实清单）

| 项 | 状态 |
|---|---|
| 托管 132 条测试 | ✅ 全绿 |
| 原生 58 条测试（Debug / Release，`/WX`） | ✅ 全绿、零警告 |
| 原生 CLI 对黄金样本的校验（跨语言门禁） | ✅ `--pin --require-pin --dry-run` exit 0 |
| 负例 E2E（`tests/e2e/msixpkg-negative.ps1`） | ✅ 33 项检查全过（含「dry-run 连目录都不建」） |
| `Msixpkg.Build` NuGet 包：包布局 / 元数据 / PackageReference 消费 / 两个 MSBuild 宿主下与黄金样本**逐字节相同** / 全部条目 store | ✅ `tests/e2e/nuget-package-e2e.ps1`：checks 25, failures 0 |
| 包发布到 nuget.org | ⚠️ **未接**（用户决定「后面再做」；本地 feed 上的消费验收已通过） |
| GUI：窗口起来、SUMMARY 内容正确 | ✅ 截图 + UIA 名称逐项核对 |
| GUI：同意页的安全门 | ✅ 程序化断言：未勾「我了解并同意」→ 安装按钮禁用、信任框也禁用；未钉扎时**必须**再勾「信任此发布者」按钮才可用；取消勾选立刻回到禁用 |
| GUI：PROGRESS / RESULT / MANAGE 的视觉 | ⚠️ MANAGE 有截图；PROGRESS / RESULT 到不了（要真装一次） |
| 证书存储真实增删（`LocalMachine\TrustedPeople`） | ⚠️ **未在本机跑**（需要管理员；单测用内存存储覆盖同一套 CRUD 逻辑） |
| 真实 MSIX 安装 / 卸载闭环 | ⚠️ **未在本机跑**（包仓库访问被拒；脚本已写好，需提权环境） |
| ARP 条目真实写入 | ⚠️ **未在本机跑**（HKLM / HKCU 注册表写都被拒） |
| 钉扎白名单（注册表）+ TOFU 记忆 | ⚠️ 注册表侧未在本机跑（写不了 HKLM）；**记忆与合并逻辑有单测**（`policy_tests.cpp`） |
| NSIS 分发安装包：静默安装 / `/D=` / 载荷逐字节一致 / 日志 / 重复安装 / 组件开关 / `/ALLUSERS` 未提权退 5 / 不可创建目标退 4 / 两种卸载路径 / 安装记录不被删 / PATH 逻辑 13 个用例 | ✅ `tests/e2e/nsis-installer-e2e.ps1`：checks 46, failures 0（其中 7 项是 PATH 单测、4 项是便携包、3 项是免提权启动器） |
| NSIS 分发安装包：安装记录 / ARP / `.msixpkg` 关联 / PATH 的**写入与清理** | ⚠️ 本机跑不了（受限环境不允许**子进程**写 HKCU）；套件会打印 SKIP 并改为断言「安装器如实上报了这些失败」，真实断言由 CI 的 `nsis-package` 作业在普通机器上跑 |

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

### 6.2 M4–M7 期间新踩的

**完整表见 [`installer-design.md`](installer-design.md) §11**，其中最要命的两条：

- `winrt::hresult_error` **不继承 `std::exception`**：逃出部署层后进程会静默 `abort`
  （退出码 3、stdout 缓冲来不及 flush、零输出）。部署层所有对外函数都包在 `guard_winrt` 里。
- `AddPackageByUriAsync` 会返回 **S_OK 但操作指针为 null**，对空对象调 `.Progress(...)`
  直接访问违例（`0xC0000005`，`/EHsc` 下 `catch(...)` 也接不住）。用 `require_operation()` 先验有效。

### 6.3 分发安装包（NSIS）期间新踩的

完整表（11 条，含复现手法）见 [`installer/nsis/README.md`](../installer/nsis/README.md) §5。
最容易再犯的四条：

- **`FileOpen $9 "x.log" a` 不会定位到末尾**（实测第二次写从 offset 0 覆盖）：必须
  `FileSeek $9 0 END`，否则日志只剩最后一次运行的最后一行。
- **NSIS 启动时就消费掉 `/D=` 并从 `$CMDLINE` 里删掉它**：判断「用户是否显式给了目录」
  只能靠 `$INSTDIR` 是否仍等于 `InstallDir` 默认值。漏了这步，`.onInit` 会用默认目录覆盖
  `/D=`，静默安装装到别处**还退 0**。
- **`SetOutPath`/`File` 失败不会让安装失败**：复制后必须自己 `IfFileExists` 复查，
  否则「什么都没装」也报成功。
- **官方 NSIS 默认 `NSIS_MAX_STRLEN=1024`**：读长 PATH 会静默截断，写回去就是破坏用户的
  PATH；必须用官方 `strlen_8192` 覆盖包，并在运行时按注册表报告的真实字节数再挡一次。

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
