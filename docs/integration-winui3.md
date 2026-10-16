# 接入真实的 Packaged WinUI 3 工程

本文件描述真实 WinUI 3 单项目 MSIX 工程如何做到「构建即产出 `.msixpkg`」。
**下面所有目标名、属性名、路径都是实测结论**，不是推测 —— 实测环境：

| 项 | 值 |
|---|---|
| Visual Studio | 2026 18.10.3 Community |
| Windows | 11 专业版 build 26300 |
| 工程生成方式 | `winapp new --name SamplePackagedApp --template winui --use-defaults`（WinApp CLI 0.6.1） |
| TFM | `net10.0-windows10.0.26100.0` |
| MSIX 打包工具链 | `Microsoft.Windows.SDK.BuildTools.MSIX` 1.7.251221100 |
| 验证过的宿主 | `dotnet build`（CoreCLR）与 VS 的 `MSBuild.exe`（.NET Framework）**都通过** |

参考实现见 [`samples/SamplePackagedApp/`](../samples/SamplePackagedApp/SamplePackagedApp.csproj)。

## 1. 三个必须先知道的事实（都是踩出来的）

### 1.1 `GenerateAppxPackage` 这个目标不存在

SDK.BuildTools.MSIX 1.7 里真正产出 `.msix` 的目标是 **`_GenerateAppxPackage`**（带下划线）。
直接写 `<Target AfterTargets="GenerateAppxPackage">` 不会报错，但**永远不会执行**，表现为「构建成功但没有 `.msixpkg`」。

`Msixpkg.Build.targets` 因此挂两个钩子：`AfterTargets="_GenerateAppxPackage;Build"`
（前者在产包后立刻打包；后者兜底，因为该目标默认是通过 `PrepareForRunDependsOn` 挂在 `Build` 里的）。

### 1.2 产物路径是**属性**，不是项

| 名称 | 实际含义 |
|---|---|
| `$(AppxPackageOutput)` | **正确**：`_ComputeAppxPackageOutput` 算出的最终 `.msix` 路径（形如 `…_Test\<Name>_<Ver>_<arch>.msix`） |
| `@(FinalAppxPackageItem)` / `@(AllBuiltSideloadPackages)` | **正确**：`_GenerateAppxPackageFile` 填的产物项 |
| `@(AppxPackageFile)` | **错误**：这是打包用的文件映射表 `package.map.txt`，不是产物列表 |

`Msixpkg.Build.targets` 按 `MsixpkgInputMsix → @(FinalAppxPackageItem) → @(AllBuiltSideloadPackages)
→ $(AppxPackageOutput) → $(AppxPackageDir)**\*.msix` 顺序收集候选，由任务挑出真实存在且最新的那个 `.msix`；
**一个都没找到就报错**，绝不静默跳过。

### 1.3 真实 `AppxSignature.p7x` 不是普通 PKCS#7

实测：前 4 字节是魔数 `PKCX`（`50 4B 43 58`），其后才是 DER SignedData：

```
50 4B 43 58 | 30 82 05 FA | 06 09 2A 86 48 86 F7 0D 01 07 02 …
   "PKCX"     DER SEQUENCE   OID 1.2.840.113549.1.7.2 (signedData)
```

Windows 的证书集合导入路径（`X509Certificate2Collection.Import`）对它是 `CRYPT_E_NOT_FOUND`。
因此 `Msixpkg.Core` 自带一个零依赖的 DER 解析器（`Appx/Pkcs7CertificateReader.cs`），
原生实现用 crypt32 的 `CryptQueryObject` / `CryptMsgGetParam` 做同一件事。
真实签名的夹具与回归见 [`tests/vectors/real-appxsignature.p7x`](../tests/vectors/).

## 2. 应用工程需要打开的属性

```xml
<PropertyGroup>
  <!-- 单项目 MSIX -->
  <EnableMsixTooling>true</EnableMsixTooling>
  <WindowsPackageType>MSIX</WindowsPackageType>

  <!-- 命令行构建也要真的产包 -->
  <GenerateAppxPackageOnBuild>true</GenerateAppxPackageOnBuild>
  <AppxBundle>Never</AppxBundle>
  <UapAppxPackageBuildMode>SideloadOnly</UapAppxPackageBuildMode>
  <AppxPackageDir>$(MSBuildProjectDirectory)\artifacts\msix\</AppxPackageDir>

  <!-- 签名：证书 Subject 必须与 Package.appxmanifest 的 Identity/@Publisher 逐字符相等 -->
  <AppxPackageSigningEnabled>true</AppxPackageSigningEnabled>
  <PackageCertificateKeyFile>$(MSBuildThisFileDirectory)certs\App.Dev.pfx</PackageCertificateKeyFile>
  <PackageCertificatePassword Condition="'$(MsixDevCertPassword)' != ''">$(MsixDevCertPassword)</PackageCertificatePassword>

  <!-- 消掉 Windows App SDK 框架包依赖（否则必须提供 MsixpkgDependencyDir） -->
  <WindowsAppSDKSelfContained>true</WindowsAppSDKSelfContained>
</PropertyGroup>
```

> 实测：打开 self-contained 后产出的 `.msix` 约 **60 MB**（含 WebView2 / DirectML / onnxruntime 等）。
> 不想要这个体积就改回框架依赖模式并准备好 `Dependencies\` 目录。

## 3. 接入 Msixpkg.Build

**A) NuGet 包（推荐；跨仓库分发给别人时唯一合理的方式）**

```xml
<ItemGroup>
  <PackageReference Include="Msixpkg.Build" Version="0.1.0" />
</ItemGroup>
```

包内的 `build\Msixpkg.Build.targets` 由 NuGet 自动导入，**不需要**写 `Import`；
`Msixpkg.Core.dll` 作为二进制附属随包内的 `tasks\netstandard2.0\` 走，所以**不要**引用 `Msixpkg.Core`
（它不再发布成包）。包的布局、可复现构建与排错见 [`nuget-package.md`](nuget-package.md)。

**B) 仓库内相对路径（本仓库的样例与 CI 走这条，离线可用）**

```xml
<ItemGroup>
  <!--
    repo-local 接入方式：让任务程序集按当前 Configuration 先构建出来。
    必须加 —— Msixpkg.Build.targets 是从 bin\$(Configuration)\netstandard2.0 加载任务的，
    而 msbuild 在解析 targets 时不会替你构建这个工程。
    （改用 NuGet 包方式接入时不需要这一段。）
    Platform=x64 是全局属性、会被传播给被引用工程，所以 Msixpkg.Build.csproj 里设了
    AppendPlatformToOutputPath=false，保证输出始终在 bin\$(Configuration)\ 而不是 bin\x64\$(Configuration)\。
  -->
  <ProjectReference Include="$(MSBuildThisFileDirectory)..\..\src\Msixpkg.Build\Msixpkg.Build.csproj"
                    ReferenceOutputAssembly="false"
                    SkipGetTargetFrameworkProperties="true"
                    Private="false" />
</ItemGroup>

<Import Project="$(MSBuildThisFileDirectory)..\..\src\Msixpkg.Build\Msixpkg.Build.targets" />

<PropertyGroup>
  <MsixpkgEnabled>true</MsixpkgEnabled>
  <!-- 不需要设置 MsixpkgInputMsix：默认的候选解析会命中 $(AppxPackageOutput) -->
  <MsixpkgDependencyDir>$(AppxPackageDir)Dependencies\</MsixpkgDependencyDir>
  <MsixpkgDisplayName>我的应用</MsixpkgDisplayName>
</PropertyGroup>
```

可用属性（全部有默认值）：

| 属性 | 默认 | 说明 |
|---|---|---|
| `MsixpkgEnabled` | `false` | **必须显式置 true** |
| `MsixpkgInputMsix` | 空 | 显式指定输入；为空时走 §1.2 的候选解析 |
| `MsixpkgOutputDir` | `$(AppxPackageDir)` | 输出目录 |
| `MsixpkgOutputFileName` | `<msix 名>.msixpkg` | 输出文件名 |
| `MsixpkgCertificatePfx` | `$(PackageCertificateKeyFile)` | 与 Appx 签名共用一个配置 |
| `MsixpkgCertificatePassword` | `$(PackageCertificatePassword)` | 同上；空密码时任务会额外给一条明确提示 |
| `MsixpkgDependencyDir` | 空 | 依赖包搜索目录 |
| `MsixpkgMinOsBuild` / `MsixpkgInstallerMinVersion` | `19041` / `0.1.0` | 写入清单 |
| `MsixpkgIncludeCertificate` | `true` | 是否内嵌发布者证书（公网受信证书设为 false） |
| `MsixpkgAllowAllUsers` | `true` | 是否允许安装器使用 `--all-users` |
| `MsixpkgSign` | `true` | 是否给容器签名 |
| `MsixpkgCreatedUtc` | 空（当前时间） | 固定容器时间戳（ISO-8601，如 `2026-01-01T00:00:00Z`）⇒ 逐字节可复现的构建；格式非法会明确报错 |
| `MsixpkgDisplayName/Description/Homepage` | 空 | 供安装器 GUI 展示 |

任务输出项：`MsixpkgOutputFile`、`MsixpkgCertificateFile`、`MsixpkgPublisherThumbprint`、`MsixpkgPackageIdentity`。

## 4. 证书

```powershell
# 用 winapp 自动匹配清单里的 Publisher（推荐；本机 New-SelfSignedCertificate -Type Custom 会因
# CSP 提供程序问题报 NTE_PROV_TYPE_NOT_DEF）
winapp cert generate --manifest .\Package.appxmanifest --output .\certs\App.Dev.pfx `
  --password <Password> --valid-days 3650 --export-cer
```

要点：

- **有效期给足 5–10 年**；打包器在剩余不足 90 天时告警，已过期会告警并在安装期拒绝。
- 自签名证书通常**无法通过 RFC3161 时间戳**，因此不要指望「签名长期有效」，按证书轮换设计。
- 公网分发请换受信 CA / Azure Artifact Signing，并把 `MsixpkgIncludeCertificate` 设为 `false`：
  那时**安装器完全不碰证书存储**，格式与安装器都不用改。

## 5. 构建与产物（实测）

```powershell
dotnet build samples\SamplePackagedApp\SamplePackagedApp.csproj -c Release -p:Platform=x64 `
  -p:MsixDevCertPassword=<Password>
```

实测日志顺序（说明钩子时机正确）：

```
MakeAppx.exe pack /l /h sha256 /f obj\…\package.map.txt /o /p …\SamplePackagedApp_1.0.0.0_x64_Test\SamplePackagedApp_1.0.0.0_x64.msix
SamplePackagedApp -> …\SamplePackagedApp_1.0.0.0_x64_Test\SamplePackagedApp_1.0.0.0_x64.msix
Msixpkg: packing …\SamplePackagedApp_1.0.0.0_x64_Test\SamplePackagedApp_1.0.0.0_x64.msix
Msixpkg: …\artifacts\msix\SamplePackagedApp_1.0.0.0_x64.msixpkg (2768D7D6-… 1.0.0.0 x64)
```

产物（`artifacts/` 已在 .gitignore 中）：

```
artifacts\msix\SamplePackagedApp_1.0.0.0_x64_Test\SamplePackagedApp_1.0.0.0_x64.msix   已签名，约 60 MB
artifacts\msix\SamplePackagedApp_1.0.0.0_x64_Test\SamplePackagedApp_1.0.0.0_x64.cer
artifacts\msix\SamplePackagedApp_1.0.0.0_x64.msixpkg                                   默认输出到 $(AppxPackageDir)
```

自检：

```powershell
src\Msixpkg.Packer\bin\Debug\net10.0\msixpkg.exe verify artifacts\msix\<Name>.msixpkg `
  --host-arch x64 --host-build 26300 --pin <MsixpkgPublisherThumbprint>
```

## 6. 常见故障（均为实测）

| 现象 | 原因与处理 |
|---|---|
| 构建成功但没有 `.msixpkg` | `MsixpkgEnabled` 仍是默认 `false`；或没传 `-p:GenerateAppxPackageOnBuild=true` |
| 完全看不到 `Msixpkg:` 日志 | 钩子挂在 `GenerateAppxPackage` 上（该目标不存在）；改用 `_GenerateAppxPackage;Build` |
| `MSB4062 未能加载任务 …bin\Release\netstandard2.0\Msixpkg.Tasks.dll` | 没加 §3 的 `ProjectReference`（任务程序集没被构建）；或该工程被写成了 `bin\x64\Release\` |
| `E_CERTIFICATE_MALFORMED … Check the password` | 没传密码。用 `-p:MsixDevCertPassword=…`；空密码时任务会额外提示 `MsixpkgCertificatePassword` |
| `E_APPX_INVALID: … PKCS#7` | 真实 p7x 带 `PKCX` 头（见 §1.3）；用带 DER 解析器的 Msixpkg.Core |
| `E_APPX_SIGNER_MISMATCH` | 传的 `.pfx` 不是签这个 `.msix` 的那一张 |
| `E_PUBLISHER_MISMATCH` | 证书 Subject 与 `Identity/@Publisher` 不一致（`O=`、`C=` 的顺序与空格都要一致） |
| `APPX0105 / APPX0107` 告警 | **非致命**：实测包最终仍带 `AppxSignature.p7x`。可忽略；也可把证书导入 `Cert:\CurrentUser\My` 消除告警 |
| `A declared package dependency could not be found` | 依赖 `.msix` 不在 `MsixpkgDependencyDir`；或改为 `WindowsAppSDKSelfContained=true` |
| VS 里 F5 会重新注册开发包 | 装正式包前先 `winapp unregister`，避免与打包安装冲突 |
