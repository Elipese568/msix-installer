# 交接文档：剩余任务清单（M4–M7）

> 给新 session 的入口文档。**只读这一个文件就能接手**：里面包含现状、环境、如何构建与测试、
> 已经写好且**必须复用**的接口、剩余任务的验收标准，以及已经踩过的坑（不要重犯）。

- 目标产品：把 WinUI 3 应用打成单一 `.msixpkg`，再用原生安装器完成
  「导入发布者证书 → 安装 MSIX」「卸载应用 → 按引用计数回收证书」闭环。
- 规范性来源：[`docs/msixpkg-format-v1.md`](msixpkg-format-v1.md)（两端实现都必须照它写）。
- 安装器设计蓝图：[`docs/installer-design.md`](installer-design.md)（M4–M6 的依据）。
- 威胁模型与安全红线：[`docs/security-model.md`](security-model.md)。
- 真实工程接入：[`docs/integration-winui3.md`](integration-winui3.md)。

---

## 0. 当前状态（一句话）

**M0–M3 已完成并验证**：开发侧（打包器 + MSBuild 任务）已跑通真实 WinUI 3 工程；
原生侧（容器读 + 校验器）已完成，与 C# 端**跨语言摘要逐字节一致**。**M4–M6 尚未开始写代码。**

| 里程碑 | 内容 | 状态 |
|---|---|---|
| M0 | 工具链核对、工作区权限修复 | ✅ |
| M1 | 格式规范 v1 + `Msixpkg.Core` + 黄金样本 | ✅ 123 条测试 |
| M2 | `Msixpkg.Packer` CLI + `Msixpkg.Build`（MSBuild 任务）+ 真实 WinUI3 工程 | ✅ |
| M3 | 原生容器读 + 校验器 | ✅ 21 条原生测试（Debug/Release 均绿，Release 开 `/WX`） |
| M4 | `certstore` + `deployment` + `state` + `log` + CLI 前端 | ⏳ **下一个** |
| M5 | 引用计数 + `list/verify/repair/clean` + ARP 卸载入口 | ⏳ |
| M6 | Win32 GUI 向导 + `app.manifest` + `.rc` | ⏳ |
| M7 | Windows Sandbox E2E + CI + 文档定稿 | ⏳ |

仓库状态：`git` 已 `add` 101 个文件（**尚无任何 commit**）。工作树干净（无未跟踪残留）。
新 session 的第一件事见 **T0**。

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
| winapp CLI | 0.6.1（`winapp new` / `winapp cert generate` 可用；模板包 0.0.7-alpha） |
| 开发者模式 | 已开启 |

**环境的两个硬限制（踩过）**：

1. **`%TEMP%` 不可写**（受限执行环境拒绝）。所有需要临时目录的代码/脚本必须落在工作区内，
   例如 `artifacts/native-tests/`（原生测试）与 `artifacts/…`（CLI）。CLI 已实现可写性探测回落链
   （`ProgramData → %TEMP% → ./artifacts/temp`），新代码沿用同样思路。
2. **VSTest 的 testhost 不可用**（需要打开父进程句柄，被拒 `Win32Exception(5)`）。
   托管测试用 **xunit v3 的可执行测试程序**，不要改用 `dotnet test`。

---

## 2. 如何构建与测试（复制即用）

```powershell
cd D:\work\source\repos\MsixpkgInstaller

# 托管：构建 + 测试（123 条）
dotnet build MsixpkgInstaller.slnx
dotnet build tests\Msixpkg.Core.Tests
tests\Msixpkg.Core.Tests\bin\Debug\net9.0\Msixpkg.Core.Tests.exe      # 期望：总计 123 成功 123

# 原生：构建 + 测试（21 条）。必须在原生工程目录里跑，CMakePresets 在那里
cd src\MsixpkgInstaller.Native
cmake --preset windows-x64 -S .
cmake --build --preset windows-x64-debug
build\x64\Debug\msixpkg_native_tests.exe                              # 期望：总计 21 成功 21
# Release 也应当零警告（/WX 已开）
cmake --build --preset windows-x64-release
build\x64\Release\msixpkg_native_tests.exe

# 打包器 CLI 自检（对黄金样本，必须逐字节复现）
cd D:\work\source\repos\MsixpkgInstaller
src\Msixpkg.Packer\bin\Debug\net9.0\msixpkg.exe verify tests\vectors\Msixpkg.Sample_1.0.0.0_x64.msixpkg `
  --host-arch x64 --host-build 26300 `
  --pin 9cd702035dd941aa6fe12870550ffc61633281da08839cfb0dfb3a95e1b913b9
# 期望：result valid，digest e67cf3483677ed9d4f33b23e2c0ff7e20ca2648124dd1f248772bea13513b4fc
```

---

## 3. 已经写好、**必须复用**的接口（不要重写）

### 3.1 托管侧（`src/Msixpkg.Core/`，`netstandard2.0` + `net9.0`，零第三方依赖）

| 类型 | 用途 |
|---|---|
| `MsixpkgManifest` / `MsixpkgIdentity` / `MsixpkgFileRef` / `MsixpkgPublisherInfo` | `msixpkg.json` 模型（严格 schema，未知成员拒绝） |
| `MsixpkgContainer` | 打开容器、`ReadEntryBytes/Text`、`ComputeEntrySha256`、`ComputeDigestEntries` |
| `MsixpkgValidator` | 30 步校验序列，返回 `MsixpkgValidationReport`（`Errors`/`Warnings`/`Manifest`/`PublisherCertificate`/`ContentDigestHex`） |
| `MsixpkgSigner` / `MsixpkgSignatureCodec` / `Thumbprints` | 容器签名与指纹 |
| `MsixpkgCertificates` | **PFX 加载的唯一入口**（见坑 #5） |
| `MsixpkgPacker` | `Pack(MsixpkgPackRequest)`：CLI 与 MSBuild 任务共用 |
| `AppxPackageReader` / `Pkcs7CertificateReader` | 内层 `.msix` 的清单与签名者（含 `PKCX` 头） |
| `MsixpkgErrorCodes` | **错误码字符串，原生侧必须逐字一致** |

### 3.2 原生侧（`src/MsixpkgInstaller.Native/`，CMake，零第三方依赖）

| 头文件 | 用途 |
|---|---|
| `include/msixpkg/errors.h` | `codes::*`（42 个错误码）+ `Error` 异常（`code()`/`message()`/`detail()`/`to_string()`） |
| `src/zip.h` | `zip::Archive::open(path)` / `open_memory(ptr,size)`（**嵌套容器零拷贝**）；`read_to_memory` / `read_stream`；`Limits` |
| `src/inflate.h` | `Inflater`（RFC1951 raw DEFLATE，输出回调式）+ `crc32` |
| `src/certs.h` | `Certificate::from_der`、`subject_string()`、`sha1/sha256_thumbprint()`、`has_code_signing_eku`、`not_before/not_after`、`public_key()`（CNG 句柄）；`extract_certificates_from_appx_signature` |
| `src/crypto.h` | `Sha256`、`sha256_hex`、`base64_decode/encode`、`verify_signature(key, alg, digest, sig)`、`signature_algorithm::*` |
| `src/container.h` | `Manifest`（模型 + `parse` + `validate`）、`Signature`、`Container::open`、`content_digest_hex`、`validate_entry_name`、`classify_entry` |
| `src/verify.h` | **安装器要调用的入口**：`ValidateOptions`、`Issue`、`Report`、`validate(path, options)`、`validate_memory(...)`；`ValidateOptions::extracted_main_package_path` 用于 TOCTOU 复检 |
| `src/appx.h` | `read_appx_identity`、`read_appx_signer_certificates`、`normalize_appx_version/architecture` |
| `src/internal.h` | `Bytes`、`to_hex_*`、`try_from_hex`、`is_lower_hex`、`split`、`widen_utf8/narrow_utf8`、`read_file/write_file` |

### 3.3 夹具（`tests/vectors/`，两端共享，**不要改动**）

| 文件 | 说明 |
|---|---|
| `Msixpkg.Sample_1.0.0.0_x64.msixpkg` | 黄金容器（SHA-256 `EA972886…A986`，内容摘要 `e67cf348…b4fc`） |
| `golden-msixpkg.json` | 从黄金容器抽出的清单，用于两端序列化**逐字节重放** |
| `test-only.pfx` | 测试证书（密码 `msixpkg-test-only`），指纹 `9cd702035dd941aa6fe12870550ffc61633281da08839cfb0dfb3a95e1b913b9` |
| `sample-main.msix` / `sample-dependency.msix` | 合成主包/依赖包 |
| `real-appxsignature.p7x` | **真实** Appx 签名（`PKCX` + DER，签名者 `CN=AppPublisher`，1538 字节） |
| `deflate-sample.zip` | DEFLATE 夹具（小文本 + 256 KiB + 一个 store 条目） |

---

## 4. 剩余任务清单

每个任务给出：目标 → 交付物 → 关键 API（已调研，可直接照抄）→ 验收标准。

### T0（先做）提交现有成果

- **目标**：把当前 101 个已暂存文件提交，让新 session 从干净工作树开始。
- **验收**：`git log --oneline -1` 有提交；`git status` 干净。
- **建议**：提交信息按里程碑拆成 3 个 commit（M1 格式与核心 / M2 打包链 / M3 原生核心）或一个总 commit。

### T1（M4）证书存储助手 `certstore`

- **目标**：只写 `LocalMachine\TrustedPeople` 的增/查/删，**绝不写 `Root`**，并记录"导入前是否已存在"。
- **交付物**：`src/certstore.h` + `src/certstore.cpp`（加进 `CMakeLists.txt` 的 `MSIXPKG_CORE_SOURCES`）。
- **关键 API**（照抄即可）：
  ```cpp
  HCERTSTORE store = CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, nullptr,
          CERT_SYSTEM_STORE_LOCAL_MACHINE, L"TrustedPeople");
  CRYPT_HASH_BLOB hash{ (DWORD)sha1.size(), (BYTE*)sha1.data() };
  PCCERT_CONTEXT found = CertFindCertificateInStore(store,
          X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, 0, CERT_FIND_HASH, &hash, nullptr);
  PCCERT_CONTEXT ctx = CertCreateCertificateContext(
          X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, der.data(), (DWORD)der.size());
  CertAddCertificateContextToStore(store, ctx, CERT_STORE_ADD_USE_EXISTING, nullptr);
  PCCERT_CONTEXT dup = CertDuplicateCertificateContext(found);
  CertDeleteCertificateFromStore(dup);   // 失败即视为"未删除"，不重试
  ```
- **接口建议**：`bool is_trusted(sha1)`, `struct AddResult { bool added; bool existed_before; }`, `AddResult add(der, sha1)`, `bool remove(sha1)`。
- **验收**（**不要碰真实 TrustedPeople**）：单测用 `CERT_SYSTEM_STORE_CURRENT_USER` + 自定义存储名 `L"MsixpkgTest"`：
  1. 新增 → `existed_before == false`；
  2. 再新增同一证书 → `added == false, existed_before == true`；
  3. 查询命中；删除后查询未命中；
  4. 传入非证书字节 → `E_CERTIFICATE_MALFORMED`，不留残留。

### T2（M4）部署封装 `deployment`

- **目标**：C++/WinRT 调用 `PackageManager`，安装/卸载/（可选）provisioning，并映射错误码。
- **交付物**：`src/deployment.h/.cpp`。**C++/WinRT 投影头由 SDK 自带**：
  `C:\Program Files (x86)\Windows Kits\10\Include\10.0.26100.0\cppwinrt\winrt\`
  （`CMakeLists.txt` 里加 `target_include_directories(... PRIVATE "${SDK_INCLUDE}/cppwinrt")` 并
  `target_link_libraries(... windowsapp)`；**不需要 NuGet**）。
- **关键 API**：
  ```cpp
  winrt::init_apartment(winrt::apartment_type::multi_threaded);
  using namespace winrt::Windows::Management::Deployment;
  PackageManager pm;
  AddPackageOptions opts;
  opts.ForceTargetAppShutdown(true);
  opts.ForceUpdateFromAnyVersion(allow_downgrade);
  opts.RetainFilesOnFailure(false);
  for (auto const& d : dependencies) opts.DependencyPackageUris().Append(Uri(d));
  auto op = pm.AddPackageByUriAsync(Uri(mainUri), opts);      // file:///…
  op.Progress([](auto const&, DeploymentProgress const& p) { /* 0..100 */ });
  DeploymentResult r = co_await op;
  if (auto e = r.ExtendedErrorCode()) { /* 映射提示 */ }
  // 卸载：pm.FindPackagesForUser(L"", familyName) → Package::Id().FullName() → RemovePackageAsync(fullName, RemovalOptions::None)
  ```
- **错误码映射**：从 `%ProgramFiles(x86)%\Windows Kits\10\Include\10.0.26100.0\um\winerror.h`
  提取 `ERROR_INSTALL_*` / `APPX_*`（建议写个小脚本生成一个 `switch`），每个码配一句可操作提示。
- **验收**：命令行 `msixpkg-native install --msix <解包出的 .msix> --dry-run` 能打印将要执行的动作；
  真机安装用**测试证书签名的自签名包**（见 T4 的授权问题）。

### T3（M4）状态库 `state` + 日志 `log` + 前端接口 `interface.h`

- **交付物**：`src/state.h/.cpp`、`src/log.h/.cpp`、`src/interface.h`、`src/installer.h/.cpp`、`src/cli/main.cpp`
  （`cli` 与 `gui` 目录可后建；CLI 先加 `add_executable(msixpkg_native ...)`）。
- **状态库**：`%ProgramData%\Msixpkg\state\<installId>.json`，字段见
  [`installer-design.md`](installer-design.md) §7；**原子写**（临时文件 + `ReplaceFile` + flush）；
  目录 ACL = Administrators + SYSTEM 独占。
- **前端接口**（编排与 UI 零依赖，照 §4 抄）：
  ```cpp
  struct IProgressSink { virtual void OnPhase(Phase, std::wstring_view) = 0;
                         virtual void OnProgress(uint32_t percent) = 0;
                         virtual void OnWaitingForElevation() = 0; };
  struct IConsentProvider { virtual ConsentDecision Ask(ConsentRequest const&) = 0; };
  struct ICancelSource { virtual bool IsCancelled() const = 0; };
  ```
- **编排**：`jthread` 上跑；失败回滚**只删本次创建的物**；依赖包不回滚（框架包共享）。
- **验收**：
  1. `state` 单测：写→读→原子性（写一半被中断不留半文件）；
  2. `msixpkg-native install <container>` 全流程（校验 → 解包到受保护暂存 → 提权导证书 → 加依赖 → 装主包 → 记账）；
  3. `msixpkg-native uninstall --family-name <PFN>`：**先卸应用**，再按四条判定决定是否删证书。

### T4（M5）引用计数与运维子命令

- **目标**：证书回收的四条判定（`certAddedByInstaller` / 无其它 installId 引用 / 无其它包 Publisher 相符 /
  策略允许且已同意）；`existed_before == true` 时**永不删除**。
- **交付物**：`list` / `verify` / `repair` / `clean` 子命令；`repair` 只输出建议，不擅自改动。
- **ARP 卸载入口**（需管理员）：
  `HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\Msixpkg.<IdentityName>`
  的 `DisplayName/DisplayIcon/DisplayVersion/Publisher/UninstallString`；否则用户在"设置→应用"里
  卸载 MSIX 会**绕过我们的卸载器**，证书永久残留。
- **验收**：
  1. 同一证书被两个容器共用 → 卸第一个时证书**不删**，卸第二个才删；
  2. `existed_before` 为真时永不删；
  3. 伪造/删除状态库后 `uninstall --family-name <PFN> --cert-thumbprint <sha1>` 仍可用；
  4. "设置→应用"里能看到我们的条目且 `UninstallString` 指向本 exe。

### T5（M6）Win32 GUI 向导

- **交付物**：`app.manifest`（`asInvoker` + PerMonitorV2 DPI + `longPathAware` + UTF-8 + comctl32 v6）、
  `res/*.rc`（zh-CN / en-US 字符串表 + 图标）、`gui/*.cpp`（`wWinMain` + 页面状态机 + sink/consent 实现）。
- **页面**：`PAGE_SUMMARY` / `PAGE_CONSENT`（安全关键）/ `PAGE_PROGRESS` / `PAGE_RESULT` / `PAGE_MANAGE`，
  细节见 [`installer-design.md`](installer-design.md) §8。
- **关键点**：编排在线程跑、UI 只收 `WM_APP_PROGRESS`（`shared_ptr` 传事件，避免悬垂）；
  提权助手 `runas` + `SW_HIDE`；UAC 被取消（`ERROR_CANCELLED` = 1223）是**正常分支**；
  单实例 `CreateMutex(L"Global\\MsixpkgInstaller.Single")`；拖放 `.msixpkg`；深色/Mica 靠
  `DwmSetWindowAttribute` 返回值判定后回落。
- **验收**：向导四页可达；未勾选同意时安装按钮禁用；取消 UAC 回到同意页且无残留状态；
  150%/200% DPI 与深浅色无截断；键盘 Tab 顺序完整；结果页"启动应用"能拉起应用。

### T6（M7）E2E + CI + 文档

- **交付物**：`tests/e2e/`（PowerShell + Windows Sandbox `.wsb`）、CI workflow、文档定稿。
- **E2E 脚本**（在干净 Sandbox 里）：
  1. `install` → 断言 `Get-AppxPackage` 存在、`Cert:\LocalMachine\TrustedPeople` 含指纹；
  2. 结果页/命令行启动应用 → 断言进程出现；
  3. `uninstall` → 断言包与证书（无共用时）都消失；
  4. 断言"设置→应用"ARP 条目存在。
- **负例**：篡改载荷、未钉扎发布者、非管理员运行、截断文件、更高版本已装、应用运行中卸载、状态库被删。

### T7 补齐原生侧的缺测用例

当前原生测试未覆盖（C# 侧已覆盖）：未签名容器、证书过期/未生效、错误 EKU。
补法：在 C# 侧 `VectorGenerator.GenerateAll()` 里多产几个负例向量（`MsixpkgWriteOptions.Sign=false`、
用 `TestCertificates.CreateEphemeral(..., notBefore: 过去)`、`codeSigningEku: false`），
提交进 `tests/vectors/` 后在原生侧加断言。

---

## 5. 已知坑（**不要重犯**，每条都是实测踩出来的）

| # | 坑 | 正确做法 |
|---|---|---|
| 1 | PowerShell 5.1 的 `Get-Content -Raw` 按 ANSI 读 UTF-8 源文件 | 中文注释会变乱码、GBK 双字节吃掉 CR 导致**注释与代码行合并**。改文件用 `read`/`edit` 工具；必须用 PS 时显式 `[IO.File]::ReadAllText(path, [Text.Encoding]::UTF8)` + `WriteAllText(..., UTF8Encoding($false))` |
| 2 | XML 注释里出现 `--` | MSBuild 直接报 `MSB4025`（我在 `--manifest`、`--project` 上各踩一次） |
| 3 | `%TEMP%` 不可写 | 临时文件放 `artifacts/…`；CLI 用可写性探测回落 |
| 4 | `EphemeralKeySet` 枚举值 | 是 **`0x20`**，不是 `0x2000`。写错会被 `X509CertificateLoader` 以 `ArgumentException` 拒绝；退回 `Exportable` 会"加载成功但私钥不可用"，错误延迟到签名阶段才暴露 |
| 5 | PFX 加载 | 一律走 `MsixpkgCertificates.LoadSigningCertificateFromFile`（含候选标志组合 + **私钥可用性探测**）。.NET 9 上必须走 `X509CertificateLoader` 且**不能**用 `Exportable\|EphemeralKeySet`（严格校验会拒绝） |
| 6 | `BCryptCreateHash` 的哈希对象缓冲区 | 必须是**成员**（要活到哈希结束）；放构造函数局部变量会得到"句柄有效但 `BCryptHashData` 失败" |
| 7 | `BCryptVerifySignature` + PKCS#1 | 必须传 `BCRYPT_PKCS1_PADDING_INFO{SHA256}`；传 NULL → `STATUS_INVALID_PARAMETER (0xC000000D)` |
| 8 | `f(ptr->data(), ptr->size(), std::move(ptr))` | 实参求值顺序未定义；按值传递的 `unique_ptr` 形参可能先被构造，之后就**在空指针上取值**（`0xC0000005`）。先取裸值再 move |
| 9 | 真实 `AppxSignature.p7x` | 前 4 字节是魔数 `PKCX`，其后才是 DER；`X509Certificate2Collection.Import` 读不了（`CRYPT_E_NOT_FOUND`） |
| 10 | MSBuild 挂载点 | `GenerateAppxPackage` 目标**不存在**（SDK.BuildTools.MSIX 1.7）；真实目标是 `_GenerateAppxPackage`，产物路径是**属性** `$(AppxPackageOutput)`，`@(AppxPackageFile)` 是 `package.map.txt` |
| 11 | CRC vs 哈希 | 翻转条目字节时原生侧报 `E_CONTAINER_INVALID`（ZIP CRC），C# 报 `E_HASH_MISMATCH`（不校验 CRC）。规范 §5 允许两条路径，**GUI 必须都当完整性失败** |
| 12 | `Platform=x64` 会传播给被引用工程 | 让任务工程输出落到 `bin\x64\Release\`；已用 `AppendPlatformToOutputPath=false` 修正 |
| 13 | .NET 无 .NET 8 SDK | 托管目标用 `net9.0`；`Msixpkg.Core` 多目标 `netstandard2.0;net9.0`（后者给 CLI/测试，前者给 MSBuild 的 .NET Framework 宿主） |
| 14 | 托管测试运行器 | 用 xunit v3 可执行程序；`dotnet test`（VSTest）在本机不可用 |

---

## 6. 安全红线（实现时不得妥协）

1. **绝不写证书存储的 `Root`**，只写 `LocalMachine\TrustedPeople`；`publisher.cer` 缺失时完全不碰证书存储。
2. **发布者指纹钉扎**：未命中白名单默认拒绝；仅 `--trust-publisher <thumbprint>` + 同意 UI 才 TOFU，并写审计日志。
3. **四重身份交叉校验**：容器签名者 == `publisher.cer` 指纹 == 内层 `AppxSignature.p7x` 签名者指纹 ==
   Appx 清单 `Publisher`。
4. **卸载顺序**：先 `RemovePackageAsync`，再按四条判定决定是否删证书；`existed_before == true` 永不删。
5. **提权只用于证书**：主进程 `asInvoker`，只有 `--add-cert` / `--remove-cert` 子命令走高 UAC 且 `SW_HIDE`。
6. **`--allow-unsigned` 只在 `#ifdef MSIXPKG_DEV` 下编译**（Release 构建不存在该路径）。
7. **TOCTOU**：暂存目录 `%ProgramData%\Msixpkg\staging\<random>`，ACL 仅 Administrators + SYSTEM；
   部署前用 `ValidateOptions::extracted_main_package_path` 复检大小与 SHA-256；
   Windows 11 23504+ 可再加 `AddPackageOptions.ExpectedDigests`。

## 7. 需要用户确认的事项（动手前先问）

1. **是否允许本机证书存储被真实修改**？M4 的端到端验证需要往 `LocalMachine\TrustedPeople`
   写入测试证书（需管理员）。不给授权就只能验证到"生成待执行动作 + dry-run"。
2. 是否现在写 **ARP 卸载入口**（会往 `HKLM\...\Uninstall` 写条目）。
3. 是否允许在测试机上**真实安装/卸载**一个用测试证书签名的 MSIX（当前首选对象：
   `samples/SamplePackagedApp`，60 MB，self-contained）。
4. 是否需要把 `Msixpkg.Build` 做成真正的 NuGet 包（当前是仓库内相对路径 `Import`；
   跨仓库分发才需要打包）。
