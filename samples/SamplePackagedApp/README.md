# SamplePackagedApp —— 真实 WinUI 3 单项目 MSIX 样例

由 `winapp new --name SamplePackagedApp --template winui --use-defaults` 生成，并接入了
[`Msixpkg.Build`](../../src/Msixpkg.Build/Msixpkg.Build.targets)，用于验证「真实工程构建即产出 `.msixpkg`」。

实测环境：VS 2026 18.10.3 / Windows 11 26300 / `Microsoft.Windows.SDK.BuildTools.MSIX` 1.7 /
TFM `net10.0-windows10.0.26100.0`。

## 为什么开发证书没有入库

`certs/App.Dev.pfx` 与 `artifacts/` 都被 .gitignore 排除：
前者是私钥（即使只用于开发也不该进仓库），后者是约 60 MB 的构建产物。

因此**新克隆的仓库需要先重新生成开发证书**，否则构建会在签名/打包步骤失败。

## 构建步骤

```powershell
cd samples\SamplePackagedApp

# 1) 生成与 Package.appxmanifest 里 Identity/@Publisher（= CN=AppPublisher）完全匹配的开发证书
#    本机实测：New-SelfSignedCertificate -Type Custom 会因 CSP 提供程序问题报
#    NTE_PROV_TYPE_NOT_DEF，所以用 winapp 生成。
winapp cert generate --manifest .\Package.appxmanifest --output .\certs\App.Dev.pfx `
  --password msixpkg-dev --valid-days 3650 --export-cer

# 2) 构建（证书密码通过命令行属性注入，绝不写进 csproj）
dotnet build SamplePackagedApp.csproj -c Release -p:Platform=x64 -p:MsixDevCertPassword=msixpkg-dev
```

预期日志（钩子时机正确的标志：产包后**立刻**打包）：

```
MakeAppx.exe pack … /p …\SamplePackagedApp_1.0.0.0_x64_Test\SamplePackagedApp_1.0.0.0_x64.msix
Msixpkg: packing …\SamplePackagedApp_1.0.0.0_x64_Test\SamplePackagedApp_1.0.0.0_x64.msix
Msixpkg: …\artifacts\msix\SamplePackagedApp_1.0.0.0_x64.msixpkg (2768D7D6-… 1.0.0.0 x64)
```

产物（约 60 MB，self-contained）：

```
artifacts\msix\SamplePackagedApp_1.0.0.0_x64_Test\SamplePackagedApp_1.0.0.0_x64.msix
artifacts\msix\SamplePackagedApp_1.0.0.0_x64.msixpkg
artifacts\msix\SamplePackagedApp_1.0.0.0_x64_Test\SamplePackagedApp_1.0.0.0_x64.cer
```

## 自检

```powershell
# 取证书指纹
src\Msixpkg.Packer\bin\Debug\net10.0\msixpkg.exe export-cert --pfx certs\App.Dev.pfx --password msixpkg-dev --output artifacts\smoke\appdev.cer

# 校验容器（--pin 用上一步输出/日志里的 MsixpkgPublisherThumbprint）
src\Msixpkg.Packer\bin\Debug\net10.0\msixpkg.exe verify artifacts\msix\SamplePackagedApp_1.0.0.0_x64.msixpkg `
  --host-arch x64 --host-build 26300 --pin <thumbprint>
```

## 已知无关告警

- `APPX0105 / APPX0107`：MSIX 工具链导入 PFX 时的告警。**实测非致命** —— 最终 `.msix` 仍带
  `AppxSignature.p7x`。把证书导入 `Cert:\CurrentUser\My` 可消除。
- `IL2104`（trim 告警）、`mspdbcmf.exe 未找到`（不生成符号包）：与本链路无关。

## 接入方式说明

本工程走的是**仓库内相对路径**（`ProjectReference` + `Import` `Msixpkg.Build.targets`），
好处是离线可构建、且改任务代码立刻生效。真实项目/跨仓库分发请改用 NuGet 包：

```xml
<ItemGroup>
  <PackageReference Include="Msixpkg.Build" Version="0.1.0" />
</ItemGroup>
```

包会自动导入 `build\Msixpkg.Build.targets`，不需要 `Import`。两种方式的验收都在
[`tests\e2e\nuget-package-e2e.ps1`](../../tests/e2e/nuget-package-e2e.ps1)（只用包的消费者视角）与
[`docs\nuget-package.md`](../../docs/nuget-package.md) 里。

## 本工程刻意不加入解决方案

`MsixpkgInstaller.slnx` 不含本工程：它依赖 Windows App SDK 还原且产物约 60 MB，
放进解决方案会让每次 `dotnet build` 都变重。需要时单独构建本工程即可；
薄验证样例请用 [`../MsixpkgBuildSample/`](../MsixpkgBuildSample/MsixpkgBuildSample.proj)。
