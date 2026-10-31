# Msixpkg Installer 的分发安装包（自研 setup）

这个目录说明**「安装包的安装包」**：把 `msixpkg-installer.exe`（向导）与
`msixpkg-native.exe`（命令行）装到用户机器上，并配好开始菜单/桌面快捷方式、
`.msixpkg` 文件关联、PATH 与「设置 → 应用」卸载入口。

> 它已经**取代**了旧的 NSIS 管线：不再下载/安装 NSIS 工具链，也没有 `makensis`、
> `NSIS_MAX_STRLEN`、`/D=` 被提前消费、`%TEMP%` 不可写就弹 `NSIS Error` 这些限制。
> 安装进程、载荷格式、页面与卸载器全部在 `src/MsixpkgInstaller.Native/setup/` 里，
> 和向导共用 Wren 自绘 UI。

```
[开发机]  cmake --build --preset windows-x64-release
             ──▶ msixpkg-setup.exe（薄 exe）
             ──▶ msixpkg-uninstall.exe
             ──▶ msixpkg-installer.exe + msixpkg-native.exe
[开发机]  scripts\build-setup.ps1
             ──▶ artifacts\setup\MsixpkgInstaller-<版本>-setup.exe（薄 exe + 内嵌载荷）
             ──▶ artifacts\setup\MsixpkgInstaller-<版本>-portable.zip
[用户机]  双击 setup.exe ──▶ 选项页 ──▶ 进度页 ──▶ 完成页
[用户机]  setup.exe /S ... ──▶ 静默安装（CI / 批量部署）
[用户机]  Uninstall.exe /S ──▶ 卸载（「设置 → 应用」里那条入口）
```

## 1. 它解决掉的 NSIS 局限

| 旧 NSIS 的问题 | 现在 |
|---|---|
| 所有 NSIS 安装包在跑脚本**之前**就必须能写 `%TEMP%`；写不了只弹 `NSIS Error: Error writing temporary file`，脚本一行都没跑 | 载荷直接追加在 setup exe 末尾，用 `CreateFileW` 读自己，**完全不碰 `%TEMP%`**。E2E 里有一条「`TEMP=X:\不存在` 也能装」的断言 |
| `NSIS_MAX_STRLEN` 默认 1024，读长 PATH 会静默截断，写回去就是毁 PATH | C++ 先 `RegQueryValueExW` 问真实字节数，再按需分配；超过 1M 字符才拒绝改动，并写日志 |
| `File`/`SetOutPath` 失败不会让安装失败，于是「什么都没装」也退 0 | 每个载荷先写临时文件并校验 SHA-256，再 `MoveFileExW` 就位；就位后复查文件存在与大小，失败退 4 |
| `/D=` 在进入脚本前就被 NSIS 消费掉，默认目录会覆盖用户选择 | 自己的参数解析；`/D=` 优先级最高，显式给了就一个字都不改 |
| 卸载器要靠 NSIS 的 `_?=`、自删除语义与「原件正在跑所以删不掉」的怪圈 | 独立 `Uninstall.exe`：同步删程序文件/注册表/PATH/快捷方式，最后改名 + detached cmd 收尾自己的 exe 与空目录；`_?=` 只作为兼容参数接受 |
| 安装器 `asInvoker`，必须教用户「右键 → 以管理员身份运行」 | `asInvoker` 不变，但选「为所有用户安装」时用 `runas` 自己弹 UAC，提权后静默完成；不再要求右键 |
| MUI 界面固定、深色/高 DPI/无障碍受限 | 与向导共用 Wren：单 HWND、Mica/Acrylic 自动降级、深色/高对比度、PerMonitorV2、MSAA |
| 构建链依赖 SourceForge 下载的 3.10 + strlen_8192 覆盖包 | 只依赖本仓库的 CMake/MSVC；`build-setup.ps1` 不再联网准备工具链 |

## 2. 装完有什么

| 东西 | 位置 |
|---|---|
| 向导 `msixpkg-installer.exe` | 安装目录 |
| 命令行 `msixpkg-native.exe` | 同上 |
| 卸载器 `Uninstall.exe` | 同上（同时是 ARP 的 `UninstallString`） |
| 开始菜单 | `Msixpkg Installer` 组：打开安装器 / 管理已安装的应用 / 卸载 |
| 桌面快捷方式 | 可选（默认不勾；静默用 `/DESKTOP`） |
| `.msixpkg` 文件关联 | 默认勾：双击 = 用向导打开并停在摘要页（不会自动安装） |
| PATH | 默认勾：安装目录加入当前用户或机器 PATH；卸载时精确移除 |
| 「设置 → 应用」条目 | `Msixpkg Installer <版本>`（键 `...\Uninstall\MsixpkgInstaller`） |
| 日志 | `%LOCALAPPDATA%\MsixpkgInstaller\setup.log`，写不了用 `%TEMP%\MsixpkgInstaller\setup.log`；可用 `--log-dir` / `MSIXPKG_SETUP_LOG_DIR` 覆盖 |

它**刻意不做**：

1. 不安装任何 MSIX 应用 —— 那是 `msixpkg-installer.exe` 的职责；
2. 不碰证书存储 —— 证书只由向导/CLI 在用户明确同意后写 `LocalMachine\TrustedPeople`；
3. 不删 `%ProgramData%\Msixpkg` 下的安装记录 —— 那是回收发布者证书的记账；
4. 不改「默认应用」的 UserChoice，只注册 `RegisteredApplications` + `Capabilities` 让用户自己选；
5. 不卸载已安装的应用，也不动 `Msixpkg.<IdentityName>` 那些 ARP 条目。

## 3. 安装模式与自提权

| 场景 | 模式 | 安装目录 | 注册表 |
|---|---|---|---|
| 已提权运行 | 为所有用户 | `%ProgramFiles%\Msixpkg Installer` | `HKLM` |
| 未提权运行 | 只为我 | `%LOCALAPPDATA%\Programs\Msixpkg Installer` | `HKCU` |
| 已有机器级安装 | 跟随它；需要管理员 | 旧目录 | `HKLM` |
| 已有用户级安装 | 跟随它 | 旧目录 | `HKCU` |

- 默认：已有机器级安装 → 所有用户；已有用户级 → 当前用户；都没有 → 已提权装所有用户，
  否则只装当前用户。
- GUI 里选「为所有用户安装」而当前未提权：点「安装」时 `ShellExecuteExW(runas)` 重新拉起
  自己（带 `/ELEVATED` 防重入，子进程用 `/S` 静默完成），父进程显示「正在等待管理员批准…」。
  用户取消 UAC → 退 5，不留半截。
- 静默 `/ALLUSERS` 未提权也会尝试自提权；**测试/受管环境**用 `/NOELEVATE` 或
  `MSIXPKG_SETUP_NO_ELEVATE=1` 让它直接退 5（E2E 就是这么测的）。
- 机器级安装的卸载需要管理员：`Uninstall.exe` 发现这一点会用 `runas` 重新拉起自己。

## 4. 命令行开关

| 开关 | 作用 |
|---|---|
| `/S` | 静默 |
| `/D=<目录>` | 指定安装目录（优先级最高；值里有空格也可以） |
| `/CURRENTUSER` | 强制只为我安装（HKCU + `%LOCALAPPDATA%\Programs`） |
| `/ALLUSERS` | 强制为所有用户安装（需要管理员/会请求 UAC） |
| `/NOASSOC` | 不注册 `.msixpkg` 关联（不会删掉已有的一份） |
| `/NOPATH` | 不改 PATH |
| `/DESKTOP` | 创建桌面快捷方式 |
| `/NODESKTOP` | 不创建桌面快捷方式（默认） |
| `/NOELEVATE` | 绝不请求提权，未提权时 `/ALLUSERS` 直接退 5（测试用） |
| `/ELEVATED` | 内部：提权重入标记，防止无限递归 |
| `/AUTO` | 诊断/自动化：停在某页后自动执行主动作（GUI E2E 用） |
| `--verify-payload` | 校验内嵌载荷（名称/大小/SHA-256）后退出，0 = 完整 |
| `--ui-page <welcome\|options\|progress\|finish\|confirm>` | 强制打开某页（截图 / 无障碍回归） |
| `--ui-result <success\|warning\|failure\|cancelled>` | 给结果页塞合成结果 |
| `--ui-progress <0..100>` | 合成进度条/时间线/日志 |
| `--ui-theme <auto\|light\|dark\|hc>` | 强制外观 |
| `--ui-material <auto\|solid\|mica\|acrylic>` | 强制材质 |
| `--ui-report <路径>` | 退出前写页面、主题、帧统计与 MSAA 子对象清单 |
| `--exit-after <ms>` | 到点自动关窗（无人值守截图/烟测） |
| `--log-dir <目录>` | 日志目录（环境变量 `MSIXPKG_SETUP_LOG_DIR` 同效） |

退出码：`0` 成功 / `4` 安装或卸载没真正完成（文件写不进/删不掉）/ `5` 需要管理员或提权被拒 /
`6` 参数、平台或前置条件不满足（包括安装前取消）/ `7` 目标被占用（向导正在运行）。

```powershell
# 只为我静默安装到指定目录（CI / E2E 用）
MsixpkgInstaller-0.3.0-setup.exe /S /CURRENTUSER /NODESKTOP /D=C:\tools\msixpkg

# 所有用户（会请求 UAC）
MsixpkgInstaller-0.3.0-setup.exe /S /ALLUSERS

# 卸载（静默；卸载器会自己收尾 exe 与空目录）
& "$env:LOCALAPPDATA\Programs\Msixpkg Installer\Uninstall.exe" /S
```

## 5. 载荷格式

`msixpkg-setup.exe` 发布时 = 薄 exe（CMake 产物）+ 追加归档：

```
[原 exe 字节][entry0][entry1][entry2][index][footer 80]
footer: magic "MSXSETUP" | u32 version=1 | u32 flags=0 | u64 index_offset |
        u64 index_size | u64 payload_size | u64 reserved | u8 SHA256(index)[32]
index:  u32 count; count × { u16 name_len; name[utf8]; u64 offset; u64 size; u8 sha256[32] }
```

- 三个条目：`msixpkg-installer.exe`、`msixpkg-native.exe`、`Uninstall.exe`；
- 读取端（`setup/setup_payload.h`）校验 footer 魔数/版本/偏移/索引摘要，每个条目在写盘时
  边写边算 SHA-256，对不上就不落盘；
- **签名会在 footer 之后再追加 Authenticode 证书表**，所以读取端从文件尾部向前找魔数，
  再用索引摘要排除证书表里的偶发匹配；不是「footer 必须在 EOF-80」；
- 打包端在 `scripts/build-setup.ps1`；两边格式必须一起改；
- **签名必须发生在追加之后**：Authenticode 覆盖整个文件，先签薄 exe 会被载荷作废。
  构建脚本在签名后会再跑一次 `--verify-payload` 兜底。

## 6. 构建与验收

```powershell
# 1) 薄 exe + Uninstall.exe + 向导 + CLI
cd src\MsixpkgInstaller.Native
cmake --preset windows-x64 -S .
cmake --build --preset windows-x64-release

# 2) 追加载荷、生成 .sha256 与便携 zip
cd D:\work\source\repos\MsixpkgInstaller
powershell -ExecutionPolicy Bypass -File scripts\build-setup.ps1
#   -BuildNative 顺便跑 cmake
#   -PayloadDir  CI 的构建目录不是 build\x64\Release 时指过去
#   -Sign -PfxPath <pfx> -PfxPassword ***
#   -SkipPortable 不打 zip

# 3) 真装一次 / 卸一次（不需要管理员；注册表断言按环境自动 SKIP）
powershell -ExecutionPolicy Bypass -File tests\e2e\setup-installer-e2e.ps1

# 4) 页面与无障碍烟测（合成数据，不安装）
powershell -ExecutionPolicy Bypass -File tests\e2e\setup-gui-smoke.ps1

# 5) 分发 zip（setup + sha256 + 中文 README）
powershell -ExecutionPolicy Bypass -File scripts\pack-installer-zip.ps1
```

产物：

| 产物 | 用途 |
|---|---|
| `MsixpkgInstaller-<版本>-setup.exe` | 真安装包（内嵌载荷） |
| `MsixpkgInstaller-<版本>-setup.exe.sha256` | 摘要 |
| `MsixpkgInstaller-<版本>-portable.zip` | 解压即用：容器 / Sandbox / 被安全软件拦住的机器；不写注册表、不要临时目录 |
| `MsixpkgInstaller-<版本>-dist.zip` | setup + 摘要 + 面向最终用户的中文 README |

## 7. 日志与排错

- 日志是 UTF-16LE、追加、每行 `[setup]` 或 `[uninstall]` 前缀；静默安装时它是唯一现场记录。
- 每一步（目录、模式、写文件、快捷方式、记录、关联、PATH）都会落一行；失败写成
  `WARNING`/`FAILED`，不会假装成功。
- 「设置 → 应用」写不进去、开始菜单建不了：通常是安全软件的主动防御拦截了未签名 exe。
  程序文件已经装好时退出码仍是 0，但日志与完成页会显示警告；签名或加信任白名单可根治。
- 安装目录被正在运行的向导占用：退 7，日志写 `msixpkg-installer.exe is locked`。
- `%TEMP%` 不可写**不是**安装失败的原因（载荷不经过临时目录）；只有日志会回落到
  `%TEMP%`，两个候选都写不了时界面显示「没有日志」。

## 8. 安全边界（与旧包一致）

- setup 自身只写文件、注册表、快捷方式、PATH；**没有任何证书写入路径**；
- 不删除安装记录，不卸载应用；
- 公网分发请给 setup 签名；自签名只适合内部分发；
- 安装器 `asInvoker`：只有用户选择「所有用户」或机器级卸载时才请求 UAC。
