# 端到端测试（`tests/e2e/`）

三层验证，从「谁都能跑」到「需要管理员 + 真实签名包」：

| 脚本 | 需要什么 | 覆盖什么 |
|---|---|---|
| [`gui-smoke.ps1`](gui-smoke.ps1) | 已构建的 `msixpkg-installer.exe`（GUI） | Wren 向导五页 + 同意页安全门 + 进度时间线 + 结果页三种形态 + 主题/材质 + 帧统计；只用 `--ui-*` 合成数据，**不安装任何东西** |
| [`msixpkg-negative.ps1`](msixpkg-negative.ps1) | 已构建的原生 exe | 篡改 / 截断 / 未钉扎 / 三个单缺陷负例向量 / 用法错误 / **失败必须可读上报** / `repair` 不改动任何东西 / 未提权不许碰证书 |
| [`nuget-package-e2e.ps1`](nuget-package-e2e.ps1) | .NET SDK（有 VS 更好） | `Msixpkg.Build` 包：包布局与元数据 / `PackageReference` 消费（空包缓存）/ 两个 MSBuild 宿主 / 产出与黄金样本**逐字节相同** / 条目一律 `store` |
| [`msixpkg-e2e.ps1`](msixpkg-e2e.ps1) | **管理员** + 真实已签名 MSIX（含其 PFX） | 完整闭环：打包 → 安装 → 包已注册 + 证书已受信 + ARP 条目 → 启动应用 → 卸载 → 包与证书都消失 → 重复卸载幂等 → 状态库丢失时的兜底 |
| [`msixpkg-e2e.wsb`](msixpkg-e2e.wsb) + [`sandbox-run.ps1`](sandbox-run.ps1) | 启用「Windows 沙盒」的机器 | 在干净系统里跑上面几套（二进制从宿主机映射进去） |

## 快速开始

```powershell
# 1) 先构建
cd src\MsixpkgInstaller.Native
cmake --preset windows-x64 -S .
cmake --build --preset windows-x64-debug

# 2) 负例套件（无需管理员，几分钟内跑完）
cd D:\work\source\repos\MsixpkgInstaller
powershell -ExecutionPolicy Bypass -File tests\e2e\msixpkg-negative.ps1

# 2b) GUI 烟测（无需管理员；只喂合成数据，不安装）
powershell -ExecutionPolicy Bypass -File tests\e2e\gui-smoke.ps1 `
    -GuiPath src\MsixpkgInstaller.Native\build\x64\Release\msixpkg-installer.exe
# 期望：checks 81, failures 0（断言按 resource.h 的 IDC_* 取控件，不依赖界面语言）

# 3) NuGet 包验收（无需管理员；会先打包，再用 PackageReference 样例构建并比对黄金样本）
powershell -ExecutionPolicy Bypass -File tests\e2e\nuget-package-e2e.ps1
# 期望：NuGet package accepted: 25 checks passed.
# 没有 VS 的机器上会跳过 .NET Framework 宿主那一段（CI 用 -RequireVsMsBuild 把跳过变成失败）
```

完整闭环需要一份**真正签名过**的 MSIX，而且容器必须用**同一张证书**打包 ——
容器校验是四重身份一致（容器签名者 == `publisher.cer` == 内层 `AppxSignature.p7x` 签名者
== Appx 清单 `Publisher`），随便换一张证书打包会在校验阶段就被拒：

```powershell
# 用仓库自带的测试证书签一个包（示例；换成你自己的包与证书）
winapp cert generate --output artifacts\e2e-full\certs
winapp sign --file App.msix --pfx artifacts\e2e-full\certs\App.Dev.pfx --password <pw>

src\Msixpkg.Packer\bin\Debug\net10.0\msixpkg.exe pack `
    --msix App.msix --pfx artifacts\e2e-full\certs\App.Dev.pfx --password <pw> `
    --cert-output artifacts\e2e-full\publisher.cer `
    --output artifacts\e2e-full\sample.msixpkg

# 在一个**提权**的 PowerShell 里：
powershell -ExecutionPolicy Bypass -File tests\e2e\msixpkg-e2e.ps1 `
    -ContainerPath artifacts\e2e-full\sample.msixpkg
```

脚本结束时会把自己在机器上留下的东西都清掉（包、证书、ARP 条目），
即使中途断言失败也一样 —— 因为残留一张受信任证书比测试失败更严重。

## Windows 沙盒

1. 把 [`msixpkg-e2e.wsb`](msixpkg-e2e.wsb) 里的 `HostFolder` 改成这台机器上的仓库绝对路径；
2. 双击它（或 `WindowsSandbox.exe tests\e2e\msixpkg-e2e.wsb`）。

两个已知前提（写在 `.wsb` 注释里了）：沙盒里**没有** VS / CMake / .NET SDK，所以只能映射
**构建产物**进去；而这些 exe 动态链接 MSVC 运行库，沙盒里没有，需要先把 `vc_redist.x64.exe`
放进去，或者把工程改成静态链接 CRT。

## 写这些脚本时踩过的坑（不要再踩）

1. **`.ps1` 里的非 ASCII 文本必须有 UTF-8 BOM**，否则 Windows PowerShell 5.1 按系统 ANSI
   解码，中文注释会变成乱码并直接导致语法错误；而任何用「读进来再写回去」的方式改文件的
   编辑器都会把 BOM 丢掉。本目录的脚本因此**一律只用 ASCII**，中文说明放在这个 README 里。
2. **`$ErrorActionPreference = 'Stop'` + 原生命令的 stderr = 终止性错误。**
   我们的 CLI 失败时往 stderr 写错误码，在 5.1 上会被变成 `NativeCommandError` 并中断脚本。
   脚本统一用 `'Continue'`，正确性完全由显式的退出码 / 输出断言保证。
3. **`%TEMP%` 可能不可写**：脚本把临时文件与数据根目录都放在仓库内的 `artifacts/e2e/` 下
   （通过 `MSIXPKG_DATA_ROOT` 覆盖），既不碰真实的 `%ProgramData%\Msixpkg`，也让断言可复现。
4. **`cmd /c "... & echo %ERRORLEVEL%"` 不可靠**：`%ERRORLEVEL%` 在整行解析时就展开了，
   打印的是执行前的旧值。要看真实退出码请直接用 `$LASTEXITCODE`，或分开两次调用。
