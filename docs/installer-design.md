# MsixpkgInstaller 原生安装器：设计蓝图（M3–M6 施工依据）

本文是 `src/MsixpkgInstaller.Native`（C++20 + C++/WinRT + Win32 GUI）的实现依据。
格式与校验规则见 [`msixpkg-format-v1.md`](msixpkg-format-v1.md)；威胁模型见
[`security-model.md`](security-model.md)。

## 1. 结论：可行，但必须修正 5 处语义

| # | 原始设想 | 必须修正为 | 理由 |
|---|---|---|---|
| 1 | 卸载：先移证书，再卸应用 | **先卸应用，再按策略回收证书** | 反过来会留下「应用还在、信任已丢」的不可自愈状态，重装/修复全部失败 |
| 2 | 整体提权后调 `AddPackageAsync` | **非提权主进程 + 提权证书助手进程** | MSIX 按用户注册；提权进程身份是管理员，会把包装给管理员账户 |
| 3 | 「装给所有用户」用 `AddPackageAsync` | 先 stage 到系统卷，再 `ProvisionPackageForAllUsersAsync` | 官方语义：provisioning 才面向所有用户，且要求管理员 + 系统卷 |
| 4 | 自动信任容器内的证书 | 必须叠加**发布者指纹钉扎 + 显式同意** | 否则等于把「代码签名信任发放权」交给 `.msixpkg` 文件本身 |
| 5 | 容器用 Authenticode 签名 | 容器用**自定义签名 + 只验密码学不验链** | `signtool` 签不了非 PE/Appx；而链信任在导入前必然失败（鸡生蛋） |

## 2. 工程结构（M3 已落地的实际布局）

```
src/MsixpkgInstaller.Native/
  CMakeLists.txt / CMakePresets.json     CMake + "Visual Studio 18 2026" 生成器；无任何 NuGet 依赖
  include/msixpkg/errors.h               稳定错误码 + Error 异常（与 C# 端逐字一致）
  src/internal.{h,cpp}                   字节/十六进制/宽窄字符串/小端/文件
  src/json.{h,cpp}                       与 C# 语义一致的最小 JSON（两端一致性由测试锁定）
  src/inflate.{h,cpp}                    自写 RFC1951 raw DEFLATE 解码器
  src/zip.{h,cpp}                        ZIP 中央目录解析 + store/deflate 流式读取 + open_memory
  src/crypto.{h,cpp}                     BCrypt：SHA-256/SHA-1、RSA(PKCS1/PSS)/ECDSA 验签、Base64
  src/certs.{h,cpp}                      crypt32 证书字段 + 自写 PKCS#7 证书集提取（含 PKCX 头部）
  src/container.{h,cpp}                  条目白名单与命名规则、清单模型、内容摘要、嵌套打开
  src/appx.{h,cpp}                       内层 .msix 的 Identity 与签名者证书
  src/verify.{h,cpp}                     校验序列与报告（GUI 需要一次拿到全部问题）
  tests/test_harness.h + tests/*.cpp     自写极简测试框架（零依赖）+ 以 tests/vectors 为输入
  build/                                 CMake 输出（.gitignore）
```

### 2.1 三个与初版蓝图的偏差（都是实测后改的）

| 项 | 蓝图 | 实际 | 原因 |
|---|---|---|---|
| ZIP 解压 | vendored miniz | **自写 inflate** | 无法下载第三方代码；且 `cabinet.dll` 的 Compression API 只给 MSZIP/XPRESS/LZMS，没有 raw DEFLATE，而真实 `.msix` 有 335/344 个条目是 DEFLATE |
| X.509 解析 | 自写 DER | **crypt32** | 让系统解析 ASN.1 远比手写可靠；自写 DER 只保留「从 AppxSignature.p7x 取证书」这一段（Windows 的证书集合导入路径读不了 `PKCX` 头） |
| 构建系统 | vcxproj + CppWinRT NuGet | **CMake** | M3 零第三方依赖；M4/M6 需要的 C++/WinRT 投影头由 Windows SDK 自带（`Include\<ver>\cppwinrt\winrt\`），无需 NuGet |

### 2.2 后续里程碑的落点

`gui/`、`cli/`、`res/`（`.rc` 与字符串表）、`core/certstore`、`core/deployment`、`core/state`、`core/log`
尚未创建，都属于 M4–M6（见 §10）。Win32 GUI 与 `.rc` 资源可以直接加进同一个 CMake 工程
（`target_sources` + `win32` 子系统），无需切换构建系统。


## 3. 进程与权限模型

| 阶段 | 进程 | 权限 | 职责 |
|---|---|---|---|
| ① 解析/校验/解包/同意 | 主进程 | `asInvoker`（**不提权**） | 容器校验、钉扎检查、解包到受保护暂存目录、GUI 同意页 |
| ② 证书 | `--add-cert` 助手（`ShellExecuteEx` + `runas`，`SW_HIDE`） | 提权 | 复核指纹 == 钉扎值 → 写 `LocalMachine\TrustedPeople` → 原子写状态 |
| ③ 部署 | 主进程 | 不提权 | `AddPackageByUriAsync` → 注册给**当前交互用户**（语义正确） |
| ④ 卸载 | 主进程 + 条件性 `--remove-cert` 助手 | 不提权 / 提权 | `RemovePackageAsync` → 引用计数判定 → 条件移除证书 |

- `--all-users`：③ 之前由提权阶段把包 stage 到系统卷，再 `ProvisionPackageForAllUsersAsync`；
  卸载对应 `DeprovisionPackageForAllUsersAsync`。**默认关闭**（文档明确 clean reprovision 会移除该包之前的安装）。
- 单个 exe、多子命令；提权只走高 UAC 的子命令，绝不整程序提权。
- WinRT 部署 API 在 MTA 线程 `co_await`：`winrt::init_apartment(winrt::apartment_type::multi_threaded)`。
- 最低 OS：Windows 10 2004（build 19041）—— `AddPackageByUriAsync` 的下限。

## 4. 前端与编排分离

```cpp
struct ProgressEvent  { Phase phase; uint32_t percent; std::wstring detail; };
struct ConsentRequest { PublisherInfo publisher; PackageInfo pkg; bool alreadyInstalled; bool pinned; bool allUsersAllowed; };
struct ConsentDecision{ bool approved; bool rememberPublisher; bool allUsers; };

struct IProgressSink    { virtual void OnPhase(Phase, std::wstring_view) = 0;
                          virtual void OnProgress(uint32_t percent) = 0;
                          virtual void OnWaitingForElevation() = 0; };
struct IConsentProvider { virtual ConsentDecision Ask(ConsentRequest const&) = 0; };
struct ICancelSource    { virtual bool IsCancelled() const = 0; };

Installer orchestrator{ progress, consent, cancel };   // 对 UI 零依赖
```

- 编排跑在 `std::jthread`；GUI 的 `IProgressSink` 只做
  `PostMessage(hwnd, WM_APP_PROGRESS, ...)`（事件对象用 `shared_ptr` 传递避免悬垂），
  UI 线程在消息里更新控件 → **UI 永不阻塞**。
- `IConsentProvider::Ask` 从编排线程向 UI 线程发同步请求，等用户点击后返回。
- UAC 被取消（`ShellExecuteEx` 返回 `ERROR_CANCELLED` = 1223）是**正常分支**：回到同意页，不残留状态。
- 三个前端共用一套编排：`gui`（向导）、`console`（`--json --log`）、`silent`（`--quiet`，CI）。
- 取消：`IAsyncOperation::Cancel()`；已 staged 的残留由 `repair` 收敛。

## 5. 安装 / 卸载状态机

```
安装
  Validate → Stage → Consent → [Elevate: AddCert] → Journal("cert-added")
        → AddDependencies → AddMainPackage → Journal("registered") → Cleanup

卸载
  1 读状态库 → PackageFamilyName / 主包 FullName / 证书指纹
  2 EnsureNotRunning（ForceTargetAppShutdown）
  3 RemovePackageAsync(fullName, RemovalOptions::None)          ← 先卸应用
  4 证书回收判定，四条全满足才删：
      a) certAddedByInstaller == true
      b) 状态库中无其它 installId 引用同一指纹
      c) 无其它已注册包的 Publisher == 该证书 Subject
      d) 策略允许且已获同意
  5 满足 → 提权助手 CertDeleteCertificateFromStore；否则只记日志
  6 删除状态记录、清理暂存与过期日志
```

回滚规则：失败时删除**本次操作创建的**物（证书若「本次新增」则删）；**依赖包不回滚**
（框架包是与其它应用共享的，只记日志）。

## 6. 关键 API（可直接照抄）

### 6.1 证书存储（crypt32）

```cpp
// 只写 TrustedPeople，绝不写 Root；只写 LocalMachine（部署栈不认用户级存储）
HCERTSTORE store = CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, nullptr,
        CERT_SYSTEM_STORE_LOCAL_MACHINE, L"TrustedPeople");

CRYPT_HASH_BLOB hash{ (DWORD)sha1.size(), (BYTE*)sha1.data() };
PCCERT_CONTEXT found = CertFindCertificateInStore(store,
        X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, 0, CERT_FIND_HASH, &hash, nullptr);

// 写入幂等；先查是为了记录 certExistedBefore（决定卸载是否有权删除）
PCCERT_CONTEXT ctx = CertCreateCertificateContext(
        X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, der.data(), (DWORD)der.size());
CertAddCertificateContextToStore(store, ctx, CERT_STORE_ADD_USE_EXISTING, nullptr);

// 删除：必须先 Duplicate 取得自有引用
PCCERT_CONTEXT dup = CertDuplicateCertificateContext(found);
CertDeleteCertificateFromStore(dup);
```

单元测试**不要碰真实 TrustedPeople**：用 `CERT_SYSTEM_STORE_CURRENT_USER` + 自定义存储名
`L"MsixpkgTest"` 做增/查/删测试（部署只认 LocalMachine，但存储 CRUD 逻辑可完全隔离测试）。

### 6.2 部署（C++/WinRT）

```cpp
using namespace winrt::Windows::Management::Deployment;

PackageManager pm;
AddPackageOptions opts;
opts.ForceTargetAppShutdown(true);
opts.ForceUpdateFromAnyVersion(downgrade);          // 仅 --allow-downgrade
opts.RetainFilesOnFailure(false);
// Windows 11 build 23504+ 才有 ExpectedDigests（UniversalApiContract v15）：
//   用 IAppxDigestProvider::GetDigest 取摘要后 Insert(mainUri, digest)，
//   让部署栈自己校验我们读的那个文件；Win10 上退化为「自算 SHA-256 + 暂存目录收紧 ACL」。
#if MSIXPKG_EXPECTED_DIGESTS
opts.ExpectedDigests().Insert(mainUri, digest);
#endif
for (auto const& d : dependencies) opts.DependencyPackageUris().Append(Uri(d));

auto op = pm.AddPackageByUriAsync(Uri(mainUri), opts);
op.Progress([](auto const&, DeploymentProgress const& p) { /* 0..100 → IProgressSink */ });
DeploymentResult r = co_await op;
if (auto e = r.ExtendedErrorCode()) throw winrt::hresult_error(e, r.ErrorText());
```

- URI 用 `file:///…`。
- 卸载：`pm.FindPackagesForUser(L"", familyName)` 取 `Package::Id().FullName()` 再
  `RemovePackageAsync(fullName, RemovalOptions::None)`；找不到即视为已卸载。
- 错误码映射表在实现期从 `winerror.h` 的 `ERROR_INSTALL_*` / `APPX_*` 生成，
  每个码配一句可操作提示（证书不受信 / 包正在使用 / 已装更高版本 / 需启用 sideload）。

### 6.3 内层 Appx 签名者提取

```cpp
// AppxSignature.p7x 是 AppxSip 的私有 CMS 结构，不是普通 detached CMS。
// 这里只做「身份提取」，与容器内 .cer 的指纹比对；密码学校验交给部署栈。
CryptQueryObject(CERT_QUERY_OBJECT_FILE, msixPath, CERT_QUERY_CONTENT_FLAG_PKCS7_SIGNED_EMBED, ...);
CryptMsgGetParam(hMsg, CMSG_CERT_PARAM, 0, ...);   // 取签名者证书，比 SHA-256 指纹
```

## 7. 状态库与日志

- `%ProgramData%\Msixpkg\state\<installId>.json`：installId、主包 FullName/FamilyName、
  证书指纹（SHA-1/SHA-256）、Subject、`certAddedByInstaller`、`certExistedBefore`、
  各载荷 SHA-256、依赖包清单、时间戳、结果。**原子写**（临时文件 + `ReplaceFile` + flush）。
- `%ProgramData%\Msixpkg\logs\installer-<ts>.log`：每步 + 每个指纹 + 每个 HRESULT。
- 状态库目录 ACL = Administrators + SYSTEM 独占（防普通用户伪造状态导致误删证书或绕过引用计数）。
- 子命令：`install / uninstall / list / verify / repair / clean`。
  `repair` 重算「状态库 vs 实际系统」并给修复建议；`clean` 清暂存与过期日志。
- 兜底：状态库丢失时允许
  `uninstall --family-name <PFN> --cert-thumbprint <sha1>` 手工指定。

## 8. GUI 向导（Win32 原生）

**技术栈**：纯 Win32 + `comctl32`（v6 manifest 依赖）+ `dwmapi`；不用 MFC/ATL（可选 WIL 作 RAII）。
子系统 `WINDOWS`，入口 `wWinMain`。

**观感与外壳**

- `app.manifest`：PerMonitorV2 DPI、`supportedOS`、UTF-8 code page、`asInvoker`、`longPathAware`、comctl32 v6。
- 深色模式：`DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, ...)`，
  初值读 `AppsUseLightTheme`；常量以 `dwmapi.h`/官方文档为准。
- 背景：Win11 用 `DWMWA_SYSTEMBACKDROP_TYPE` = Mica（`DWMSBT_MAINWINDOW`），
  圆角用 `DWMWA_WINDOW_CORNER_PREFERENCE`；Win10 回落纯色/直角（靠返回值判定，不假设系统版本）。
- 基准 720×480 dp，按 DPI 缩放；进度条 owner-draw 或 `PROGRESS_CLASS`；品牌图用 WIC。
- 文案走 `.rc` 字符串表（zh-CN / en-US 两套）。
- 无障碍：全部标准控件类（`BUTTON`/`STATIC`/`SYSLINK`）+ `WS_TABSTOP`/`WS_GROUP`，不吞 `WM_GETOBJECT`。

**页面状态机**

| 页面 | 内容 |
|---|---|
| `PAGE_SUMMARY` | 应用名/版本/架构/发布者/体积/将安装的依赖清单 |
| `PAGE_CONSENT` | 安全关键页：明示「将把发布者证书写入**本机受信任人员存储**，使以此证书签名的任何应用都能安装」；展示 Subject、SHA-256 指纹（可复制）、有效期、是否已在存储中、是否命中钉扎白名单；「我了解并同意」复选框（未勾选则禁用安装按钮）；`--all-users` 复选框（仅当 `allowAllUsers`）；未命中钉扎 → 默认拒绝，仅当勾选「信任此发布者（将记住）」并二次确认后继续，并写审计日志 |
| `PAGE_PROGRESS` | 阶段列表（校验/解包/证书/依赖/主包/注册）+ 真实百分比 + 「正在等待管理员确认…」+ 取消按钮（取消 → 回滚 → 返回上一页） |
| `PAGE_RESULT` | 成功：启动应用（`IApplicationActivationManager::ActivateApplication`，AUMID = PFN + `!` + `Application/@Id`）/ 打开日志 / 关闭；失败：错误摘要 + 可操作建议 + 日志路径 + 复制详情 |
| `PAGE_MANAGE` | 列出本安装器装过的包（名称/版本/证书指纹/状态），提供 卸载 / 修复 / 打开日志 / 打开「设置→应用」 |

**其它行为**

- 单实例：`CreateMutex(L"Global\\MsixpkgInstaller.Single")`；已存在则 `SetForegroundWindow` + `WM_COPYDATA` 传参。
- 拖放：`DragAcceptFiles` + `WM_DROPFILES` → 载入并跳到 `PAGE_CONSENT`。
- 提权助手一律 `SW_HIDE`，避免「UAC 之后又弹黑框」。
- 退出码与 CLI 一致，CI 复用同一 exe。

## 9. 「设置 → 应用」卸载入口

必须写 ARP 条目（需管理员）：

```
HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\Msixpkg.<IdentityName>
  DisplayName        = <名称>（含证书清理）
  DisplayIcon        = <exe 路径>
  DisplayVersion     = <版本>
  Publisher          = <发布者>
  UninstallString    = "<exe>" uninstall --family-name <PFN>
```

否则用户在系统设置里卸载 MSIX 包会**绕过我们的卸载器**，证书永远留在受信任区，
直接破坏「引用计数回收」策略。副作用：系统设置里会同时出现 MSIX 条目与我们的条目，
用 `DisplayName` 后缀区分并在文档说明。可选再注册 `.msixpkg` 文件关联（`HKLM\Software\Classes`）。

## 10. 实现进度

### M3 ✅ 完成（21 条原生测试全绿）

已完成并验证：

- **ZIP**：中央目录解析（含 ZIP64 回退）、条目白名单与限额、store/deflate 流式读取、
  `open_memory` 用于**嵌套**容器（外层 store 的内层 `.msix` 直接借用映射区间）；
- **自写 inflate**：真实 makeappx 产出的 335 个 DEFLATE 条目全部正确解压
  （含 2.7 KB 的 `AppxManifest.xml` 与 1.5 KB 的 `AppxSignature.p7x`），
  并用 256 KiB 高压缩比夹具做逐字节比对；
- **密码学**：BCrypt SHA-256/Base64、RSA PKCS#1 v1.5 验签（`BCRYPT_PKCS1_PADDING_INFO`
  必须传，否则 `STATUS_INVALID_PARAMETER`）；
- **X.509**：crypt32 解析 Subject（`CERT_NAME_STR_REVERSE_FLAG` 后与 .NET 形式一致，已由测试锁定）、
  两种指纹、EKU、有效期、自签名判定；公钥用 `CryptImportPublicKeyInfoEx2` 导入 CNG；
- **PKCS#7**：从真实 `AppxSignature.p7x`（`PKCX` 头 + DER）提取到 `CN=AppPublisher` 签名者证书；
- **校验序列**：清单 schema 门禁、installer/OS/架构门禁、声明文件大小与哈希、
  证书四项（指纹/Subject/EKU/有效期）+ 钉扎、容器签名（摘要/指纹/密码学）、
  内层 Appx 四项一致性 + 签名者指纹比对、已解包主包的 TOCTOU 复检；
- **跨语言一致性**：C++ 独立算出的内容摘要与 C# 的 `e67cf3483677ed9d4f33b23e2c0ff7e20ca2648124dd1f248772bea13513b4fc`
  完全一致；错误码字符串与 C# 端逐字一致。

新发现并已写入规范的一条跨实现差异：翻转条目数据字节时，原生侧由 ZIP 层 CRC-32 先拦下
（`E_CONTAINER_INVALID`），而 .NET 的 `ZipArchive` 不校验 CRC，会走到 `E_HASH_MISMATCH`。
规范 §5 明确允许两条探测路径，GUI 必须把两者视为同一类完整性失败。

### 待办

1. **M4**：`certstore`（crypt32 增/查/删 TrustedPeople）+ `deployment`（C++/WinRT `PackageManager`）
   + `state`（原子 JSON 状态库）+ `log` + CLI 前端 + `interface.h`；命令行跑通一次安装/卸载。
2. **M5**：引用计数、`list/verify/repair/clean`、ARP 卸载入口。
3. **M6**：GUI 向导（四页 + 管理页、DPI/深色/本地化/无障碍）+ `app.manifest` + `.rc`。
4. **M7**：Windows Sandbox E2E、CI、文档定稿。

尚未在原生侧覆盖的用例（当前由 C# 套件覆盖）：
未签名容器、证书过期/未生效、错误 EKU 的容器 —— 需要 C# 侧生成对应负例向量后再补测。

> 单元测试的证书存储用例请用 `CERT_SYSTEM_STORE_CURRENT_USER` 下的自定义存储，
> 避免污染真实的 `LocalMachine\TrustedPeople`。

