# Msixpkg 安装器 {{VERSION}}

这个包用来安装 **Msixpkg 安装器**：用来安装 `.msixpkg` 包的工具（一个向导 + 一个命令行）。
它本身不会安装任何应用，也不会修改证书存储。

- 安装包：`{{SETUP_FILE}}`
- SHA-256：`{{SHA256}}`

## 安装

1. 双击 `{{SETUP_FILE}}`。
2. 选择「仅为我安装」（不需要管理员）或「为所有用户安装」。
   - 选「为所有用户安装」时，点「安装」后 Windows 会请求管理员批准（UAC）；
     无需右键「以管理员身份运行」。
3. 点「安装」，完成后可勾选「运行 Msixpkg 安装器」。

安装完成后：

- 双击任意 `.msixpkg` 文件，会用向导打开并停在摘要页（仍需你在同意页确认才会安装）；
- 开始菜单里有「Msixpkg 安装器」；
- 命令行 `msixpkg-native` 可用（如果安装时勾选了 PATH）。

## 静默安装

```bat
MsixpkgInstaller-{{VERSION}}-setup.exe /S /CURRENTUSER /NODESKTOP /D=C:\tools\msixpkg
```

| 开关 | 作用 |
|---|---|
| `/S` | 静默，不显示界面 |
| `/D=<目录>` | 指定安装目录 |
| `/CURRENTUSER` | 只为我安装 |
| `/ALLUSERS` | 为所有用户安装（会请求管理员批准） |
| `/NOASSOC` | 不注册 `.msixpkg` 关联 |
| `/NOPATH` | 不把安装目录加入 PATH |
| `/NODESKTOP` | 不创建桌面快捷方式（默认） |
| `/DESKTOP` | 创建桌面快捷方式 |

退出码：`0` 成功、`4` 没真正装完、`5` 需要管理员、`6` 参数或系统不满足、`7` 目标被占用。
日志在 `%LOCALAPPDATA%\MsixpkgInstaller\setup.log`（写不了时退到 `%TEMP%`）。

## 卸载

打开「设置 → 应用」，找到「Msixpkg 安装器」，点「卸载」。
或运行安装目录里的 `Uninstall.exe /S`。

卸载只删除这个安装器本身：程序文件、快捷方式、`.msixpkg` 关联、PATH 条目与卸载入口。
**已经安装的应用不会被卸载**；`%ProgramData%\Msixpkg` 下的安装记录会保留，
那是以后回收发布者证书的依据。

## 便携版

如果不想动注册表，或机器在容器 / Windows Sandbox / 被安全软件拦住的环境中，
解压 `MsixpkgInstaller-{{VERSION}}-portable.zip`，直接运行里面的 `msixpkg-installer.exe`。
代价是没有文件关联、开始菜单/桌面快捷方式与「设置 → 应用」条目；「卸载」就是删掉文件夹。

## 系统要求

- 64 位 Windows 10（2004 / build 19041）或更高版本。

## 说明

- 本安装器经过签名时，Windows 与安全软件会更信任它；未签名版本在部分带主动防御的机器上
  可能写不进文件关联 / PATH / 卸载入口，日志里会明确写 `WARNING` / `FAILED`。
- 本安装包不含任何应用与证书；`.msixpkg` 的发布者证书只会在你于向导中明确同意后写入。
