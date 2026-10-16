# MsixpkgPackageSample —— 消费者视角的 NuGet 接入样例

这个样例证明「打包工具作为 NuGet 包」真的能用，而且产出的容器与仓库里的黄金样本**逐字节相同**。

它刻意与仓库解耦：

- **没有** `Import` 仓库里的 `Msixpkg.Build.targets`；
- **没有** `ProjectReference` 到 `src\Msixpkg*`；
- 唯一的接入点是 `<PackageReference Include="Msixpkg.Build" Version="0.1.0" />`。

```powershell
# 1) 先打包（包只在本地 feed 里，NuGet 上没有）
powershell -ExecutionPolicy Bypass -File scripts\pack-nuget.ps1

# 2) 再构建本样例：MsixpkgPack 会在 Build 之后自动产出 artifacts\
dotnet build samples\MsixpkgPackageSample\MsixpkgPackageSample.csproj -c Release

# 3) 想连「两个 MSBuild 宿主 + 包布局 + 逐字节比对」一起验，直接跑验收脚本
powershell -ExecutionPolicy Bypass -File tests\e2e\nuget-package-e2e.ps1
```

## 关键配置与真实应用工程一致

| 属性 | 值 | 说明 |
|---|---|---|
| `MsixpkgEnabled` | `true` | 必须显式打开 |
| `MsixpkgInputMsix` | `tests\vectors\sample-main.msix` | 真实工程里不用写：默认候选解析会命中 Appx 打包产物 |
| `MsixpkgCertificatePfx` | `tests\vectors\test-only.pfx` | 真实工程里就是给 `.msix` 签名的那个 `.pfx` |
| `MsixpkgDependencyDir` | `tests\vectors\` | 依赖按「包名 + 架构」匹配，目录里多放别的 `.msix` 也安全 |
| `MsixpkgCreatedUtc` | `2026-01-01T00:00:00Z` | **可复现构建**：固定时间戳，否则每次构建的容器字节都不同 |
| `MsixpkgDisplayName/Description/Homepage` | 与 `tests\vectors\README.md` 里的复现参数逐字一致 | 这三个值进清单，所以也影响字节 |

`NuGet.config` 里 `<clear />` 掉了机器上的全局源，只留本地 feed：

- 验收不该受开发机上别的源与缓存影响；
- 本样例除 `Msixpkg.Build` 外没有任何 `PackageReference`，所以离线也能复现。

真实项目把这一行换成 nuget.org 即可（`Msixpkg.Build` 发布后）。

## 产物

| 文件 | 说明 |
|---|---|
| `artifacts\Msixpkg.Sample_1.0.0.0_x64.msixpkg` | SHA-256 必须等于 `EA97288615F600731F659E5DF8C44F138759691889C09B3D340A6918F781A986`（黄金样本） |
| `artifacts\Msixpkg.Sample_1.0.0.0_x64.cer` | 从签名证书导出的发布者证书 |

## 注意

- `Msixpkg.Build` 的版本号与本工程 `PackageReference` 里钉的版本必须一致；改版本时两处一起改
  （`scripts\pack-nuget.ps1 -Version` 只改包，不改这里）。
- Visual Studio 的 MSBuild 有个坑：`/t:Restore,Rebuild` 写在**同一次**调用里时，刚还原的包的
  `build\*.targets` 来不及被导入（求值发生在 Restore 之前），表现为「构建成功但没有 `.msixpkg`」。
  分开跑 Restore 与 Rebuild 即可，`tests\e2e\nuget-package-e2e.ps1` 就是这么做的。
