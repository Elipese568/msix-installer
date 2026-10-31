# NuGet 包：Msixpkg.Build

把「构建期把已签名的单项目 MSIX 打成 `.msixpkg`」这件事做成一个 **MSBuild 任务包**，
应用工程加一行 `PackageReference` 就够，不需要 `Import` 任何相对路径、也不需要额外的命令行步骤。

仓库里**只有这一个包**。

| 组件 | 是否上 NuGet | 说明 |
|---|---|---|
| `Msixpkg.Build` | ✅ **是**（MIT） | `build\Msixpkg.Build.targets` + `tasks\netstandard2.0\Msixpkg.Tasks.dll` |
| `Msixpkg.Core` | ❌ 否 | 作为**二进制附属**放进 `Msixpkg.Build` 包的 `tasks\netstandard2.0\Msixpkg.Core.dll` |
| `Msixpkg.Packer`（`msixpkg` CLI） | ❌ 否 | 开发机 / CI 的构建期工具，`dotnet build` 从源码产出即可，不是给终端用户的产物 |
| 原生安装器（`msixpkg-installer.exe` / `msixpkg-native.exe`） | ❌ 否 | 终端用户拿到的是自研 setup 或便携 zip，见 [`installer/setup/README.md`](../installer/setup/README.md) |

> 为什么 Core 不做成依赖：任务 DLL 是被宿主 MSBuild 进程在**自己的 load context** 里加载的，
> 它的依赖必须与它同目录才能解析。若再声明一个 `Msixpkg.Core` 包依赖，就会出现同一程序集的
> 两个来源（一个在 `tasks\` 下、一个从 `lib\` 引用进来），只会制造版本冲突。
> 因此 `Msixpkg.Build` 的 `.nuspec` 里**没有** `dependencies`，也没有 `lib\`。

## 1. 安装与最小配置

```xml
<ItemGroup>
  <PackageReference Include="Msixpkg.Build" Version="0.3.0" />
</ItemGroup>

<PropertyGroup>
  <MsixpkgEnabled>true</MsixpkgEnabled>
  <PackageCertificateKeyFile>certs\App.Dev.pfx</PackageCertificateKeyFile>
  <!-- 密码绝不入库：用命令行属性或本机用户级 .props 注入 -->
  <MsixpkgCertificatePassword Condition="'$(MsixDevCertPassword)' != ''">$(MsixDevCertPassword)</MsixpkgCertificatePassword>
  <MsixpkgDependencyDir>$(AppxPackageDir)Dependencies\</MsixpkgDependencyDir>
</PropertyGroup>
```

* `MsixpkgEnabled` 默认 `false`，必须显式打开；其余属性都有默认值。
* 任务挂在 `_GenerateAppxPackage` 与 `Build` 之后，所以 `dotnet build` 与 Visual Studio 都会产出容器。
* 完整属性表与真实 WinUI 3 工程的接入步骤见 [`integration-winui3.md`](integration-winui3.md)。

## 2. 包内布局

```
build\Msixpkg.Build.targets                 NuGet 自动导入（挂载点）
tasks\netstandard2.0\Msixpkg.Tasks.dll      MSBuild 任务（宿主的 .NET Framework / .NET 都能加载）
tasks\netstandard2.0\Msixpkg.Core.dll       二进制附属
LICENSE  PACKAGE.md
```

`tasks\netstandard2.0\` 这个路径不是随意取的：`Msixpkg.Build.targets` 里
`$(MSBuildThisFileDirectory)..\tasks\netstandard2.0\Msixpkg.Tasks.dll` 正是按包的布局解析任务程序集的
（同一个文件在仓库内则走 `bin\$(Configuration)\` 那条分支）。

## 3. 可复现构建

`MsixpkgCreatedUtc` 设成固定时间戳后，同一份输入在任意机器、任意时刻都产出**逐字节相同**的容器：

```xml
<MsixpkgCreatedUtc>2026-01-01T00:00:00Z</MsixpkgCreatedUtc>
```

v1 规定写出端一律用 `store`（不压缩），所以容器字节完全可复现 —— 这也是仓库里黄金样本
（`tests\vectors\Msixpkg.Sample_1.0.0.0_x64.msixpkg`）能做字节级比对的前提。
写出实现在 `src/Msixpkg.Core/Zip/StoredZipWriter.cs`；**不要**换回 `System.IO.Compression.ZipArchive`
（原因见 `docs/handoff.md` 的坑 #15）。

## 4. 本地打包与验收（不需要管理员）

```powershell
# 1) 打包：artifacts\nuget\Msixpkg.Build.<版本>.nupkg
powershell -ExecutionPolicy Bypass -File scripts\pack-nuget.ps1

# 2) 验收：包布局 + 元数据 + PackageReference 消费 + 两个 MSBuild 宿主 + 与黄金样本逐字节比对
powershell -ExecutionPolicy Bypass -File tests\e2e\nuget-package-e2e.ps1
# 期望：25 项检查全过
```

**验收刻意只在本机跑**：CI（[`ci.yml`](../.github/workflows/ci.yml)）负责托管/原生/E2E/setup 四段验证，
NuGet 那条流水线（[`nuget-publish.yml`](../.github/workflows/nuget-publish.yml)）只负责「构建 + 发布」。
需要复核包本身时，跑上面第二条命令即可。

验收样例是 `samples\MsixpkgPackageSample`：它**不** `Import` 仓库里的 targets、**不** `ProjectReference`
仓库里的工程，只用 `PackageReference`；`NuGet.config` 里 `<clear />` 掉全局源，只留本地 feed
（`artifacts\nuget`），因此不受开发机上别的源与缓存影响。

在「不允许命名管道」的受限环境（沙箱 / 容器）里跑 `dotnet build` 需要
`-m:1 -nodeReuse:false -p:UseSharedCompilation=false`；上面两个脚本已经内置（对两个小工程无成本）。

## 5. 版本与发布（CI 负责构建与发布）

**已发布版本：`0.3.0`** — <https://www.nuget.org/packages/Msixpkg.Build/0.3.0>
（由 tag `v0.3.0` 触发；上一版 `0.1.0` 的发布记录：
[run 38944111342](https://github.com/Elipese568/msix-installer/actions/runs/38944111342)）。
包 ID + 版本一旦发布不可覆盖，所以后续每次改动都要换版本号。

版本号的事实来源是 `Directory.Build.props` 的 `0.3.0`；发布时由工作流覆盖
（`scripts\pack-nuget.ps1 -Version <版本>`）。样例工程里钉的版本要同步改。

发布走 [`.github/workflows/nuget-publish.yml`](../.github/workflows/nuget-publish.yml)：

| 触发 | 版本来源 |
|---|---|
| 打 tag（`git tag v0.2.0 && git push origin v0.2.0`） | tag 名去掉 `v` → `0.2.0` |
| Actions → nuget-publish → Run workflow（`version` 留空） | `Directory.Build.props` 里的默认版本 |
| 手动 `workflow_dispatch` 填 `version` | 就用填的那个 |

`dry_run=true` 时只构建并把 `.nupkg` 作为 artifact 传上来，不碰 nuget.org。

### 5.1 一次性准备（**已配置完成**，换仓库/换账号时才需要重做）

1. **nuget.org** → 用户名菜单 → **Trusted Publishing** → Add a new policy：

   | 字段 | 值 |
   |---|---|
   | Repository Owner | `Elipese568` |
   | Repository | `msix-installer` |
   | Workflow File | `nuget-publish.yml`（**只填文件名**，不要带 `.github/workflows/`） |
   | Environment | 留空（工作流没有用 GitHub environment） |

2. **GitHub 仓库** → Settings → Secrets and variables → Actions → New repository secret：
   `NUGET_USER` = 你在 nuget.org 的**用户名（profile name）**，不是邮箱。

工作流凭 GitHub OIDC 换一个 **1 小时有效的临时 API key** 再 `dotnet nuget push`：
仓库里**不存任何长期密钥**，也不需要 `NUGET_API_KEY`。临时 key 用过即废，所以登录步骤紧挨推送步骤。

### 5.2 注意事项

* nuget.org 上「包 ID + 版本」**发布后不可覆盖**：重跑同一版本会被 `--skip-duplicate` 跳过，
  要改内容就得换版本号。
* 包的 `RepositoryUrl` / `projectUrl` 指向 `Elipese568/msix-installer`，`.nuspec` 的 `repository`
  字段带的是打包时的 commit —— 发布前确认工作已经推上去。
* 首次发布建议先用 `dry_run=true` 跑一次，核对 artifact 里的包内容。
* 私有仓库的 Trusted Publishing 策略会先「临时生效」7 天，期间成功发布一次才永久激活；
  本仓库是 public，一般直接可用。

## 6. 排错

| 现象 | 原因 / 处理 |
|---|---|
| 加完 `PackageReference` 后一次 `msbuild /t:Restore,Rebuild` 构建成功但**没有** `.msixpkg` | 包的 `build\*.targets` 是在**求值阶段**由 `obj\*.nuget.g.targets` 导入的，而它由 Restore 生成；同一次调用里 Restore 不会触发重新求值。分开跑 `Restore` 与 `Rebuild`（`tests\e2e\nuget-package-e2e.ps1` 就是这么做的） |
| `Msixpkg: no input .msix was found` | Appx 打包那一步没跑：确认 `GenerateAppxPackageOnBuild=true`、`AppxPackageSigningEnabled=true`，或显式给 `MsixpkgInputMsix` |
| `Msixpkg: no certificate password was supplied` | `.pfx` 有密码但没给 `MsixpkgCertificatePassword`（或 `PackageCertificatePassword`） |
| 容器里的证书与清单发布者不一致 | 签名 `.pfx` 的 Subject 必须与 `Package.appxmanifest` 的 `Identity/@Publisher` **逐字符**相等 |
| 找不到任务程序集（`MSB4062`） | 包没还原成功（看 `obj\project.assets.json`），或有人把 `MsixpkgTasksAssembly` 指到了不存在的路径 |
