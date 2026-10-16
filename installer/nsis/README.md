# Msixpkg Installer 的分发安装包（NSIS）

这个目录里是**「安装包的安装包」**：用 NSIS 3 把 `msixpkg-installer.exe`（向导）与
`msixpkg-native.exe`（命令行）装到用户机器上，并配好它们的使用入口。

```
[开发机]  cmake --build --preset windows-x64-release   ──▶  msixpkg-installer.exe + msixpkg-native.exe
[开发机]  scripts\build-nsis-installer.ps1             ──▶  artifacts\setup\MsixpkgInstaller-<版本>-setup.exe
                                                              artifacts\setup\run-setup-with-local-temp.cmd
                                                              artifacts\setup\MsixpkgInstaller-<版本>-portable.zip
[用户机]  MsixpkgInstaller-0.2.0-setup.exe             ──▶  向导 + CLI + 快捷方式 + .msixpkg 关联 + PATH
[用户机]  MsixpkgInstaller-0.2.0-setup.exe /S ...      ──▶  静默（CI / 批量部署 / E2E）
[%TEMP% 不可写] run-setup-with-local-temp.cmd ...      ──▶  同 setup，但先把 TEMP 指到可写位置（§7）
[受限环境] 解压 portable.zip                           ──▶  向导 + CLI（完全不碰临时目录，§7）
```

- 脚本：[`MsixpkgInstaller.nsi`](MsixpkgInstaller.nsi)（UTF-8 无 BOM，**必须**用
  `makensis /INPUTCHARSET UTF8` 编译，否则中文文案会按系统 ANSI 代码页解码成乱码）
- PATH 组件：[`msixpkg-path.nsh`](msixpkg-path.nsh)（单独一份，好让单元测试直接调宏）
- 构建：`scripts\build-nsis-installer.ps1`（产出 setup exe、免提权启动器、便携 zip）
- 验收：`tests\e2e\nsis-installer-e2e.ps1`（46 项检查，不需要管理员）
  + `tests\nsis\path-logic.nsi`（PATH 逻辑的 13 个用例，由上面那个套件自动编译并运行）

## 1. 它做什么、不做什么

装完之后这台机器上会有：

| 东西 | 位置 |
|---|---|
| 向导 `msixpkg-installer.exe` | 安装目录（默认见 §2） |
| 命令行 `msixpkg-native.exe` | 同上 |
| 开始菜单 | `Msixpkg Installer` 组：打开安装器 / 管理已安装的应用 / 卸载 |
| 桌面快捷方式 | 可选（默认不勾） |
| `.msixpkg` 文件关联 | 双击 = 用向导打开并停在摘要页 |
| PATH | 可选（默认勾）：安装目录加进 PATH，让 CLI 在任意 shell 可用 |
| 「设置 → 应用」条目 | `Msixpkg Installer <版本>`（键名 `...\Uninstall\MsixpkgInstaller`） |
| 日志 | `%LOCALAPPDATA%\MsixpkgInstaller\setup.log`，写不了就回落 `%TEMP%\MsixpkgInstaller\setup.log`（UTF-16LE） |

它**刻意不做**的事（是设计边界，不是遗漏）：

1. 不安装任何 MSIX 应用 —— 那是 `msixpkg-installer.exe` 的职责；
2. 不碰证书存储 —— 证书只能由向导 / CLI 在用户明确同意后写进 `LocalMachine\TrustedPeople`
   （见 [`docs/security-model.md`](../../docs/security-model.md)）。这个安装包本身没有任何
   证书写入路径，安装它不会发放任何代码签名信任；
3. 不删 `%ProgramData%\Msixpkg` 下的安装记录（state）—— 那是**回收发布者证书**的记账，
   删了证书就永远留在受信任存储里。卸载时只提示位置，不删（E2E 里有专门一条断言盯着这个行为）；
4. 不动 ARP 里属于**已装应用**的条目（`msixpkg-native` 写的是 `Msixpkg.<IdentityName>`），
   只写自己这一条 `MsixpkgInstaller`，两者在「设置」里靠名字区分；
5. 不自行改「默认应用」的 UserChoice（Windows 10 起系统不允许），只注册
   `RegisteredApplications` + `Capabilities` 让用户能在「默认应用」里选我们；
   卸载时也只在默认值/UserChoice 仍指向我们时才清理。

## 2. 安装模式（重要）

| 场景 | 模式 | 安装目录 | 注册表 |
|---|---|---|---|
| 已提权运行（管理员） | 为所有用户 | `%ProgramFiles%\Msixpkg Installer` | `HKLM` |
| 未提权运行 | 只为我 | `%LOCALAPPDATA%\Programs\Msixpkg Installer` | `HKCU` |
| 已存在机器级安装 | 跟随它（需要管理员，否则报错退出 5） | 旧目录（升级/修复） | `HKLM` |
| 已存在用户级安装 | 跟随它 | 旧目录 | `HKCU` |

安装器自身是 `asInvoker`（`RequestExecutionLevel user`），**不会**自己弹 UAC：

- 想装给所有用户：右键安装包 →「以管理员身份运行」；
- 机器级安装的**卸载**需要管理员：卸载器发现这一点时会用 `runas` 重新拉起
  `%INSTDIR%\Uninstall.exe`（带 `/ELEVATED` 防重入）；用户拒绝提权就退出 5，不留半截状态。

## 3. 命令行开关

| 开关 | 作用 |
|---|---|
| `/S` | 静默（NSIS 内建） |
| `/D=<目录>` | 指定安装目录（NSIS 内建，**必须最后一个参数且不加引号**） |
| `/CURRENTUSER` | 强制「只为我安装」（不需要管理员） |
| `/ALLUSERS` | 强制「为所有用户安装」（需要管理员，否则退出 5） |
| `/NOASSOC` | 不注册 `.msixpkg` 关联 |
| `/NOPATH` | 不改 PATH |
| `/NODESKTOP` | 不建桌面快捷方式 |

退出码：`0` 成功 / `4` 安装或卸载没能真正完成（文件写不进、删不掉）/ `5` 需要管理员 /
`6` 前置条件不满足（非 x64、低于 Windows 10）/ `7` 目标被占用（向导正在运行）。

```powershell
# 只为我静默安装到指定目录（CI / E2E 用的形式；/D= 必须是最后一个参数）
MsixpkgInstaller-0.2.0-setup.exe /S /CURRENTUSER /NODESKTOP /D=C:\tools\msixpkg

# 卸载（静默）：注意卸载器会把自己复制到 %TEMP% 再跑
& "$env:LOCALAPPDATA\Programs\Msixpkg Installer\Uninstall.exe" /S

# 想等它跑完并拿到真实退出码（脚本里推荐这种形式）
& "$env:LOCALAPPDATA\Programs\Msixpkg Installer\Uninstall.exe" /S "_?=$env:LOCALAPPDATA\Programs\Msixpkg Installer"
```

## 4. 构建

```powershell
# 1) 先有载荷（Release）
cd src\MsixpkgInstaller.Native
cmake --preset windows-x64 -S .
cmake --build --preset windows-x64-release

# 2) 编安装包（默认会自己准备 NSIS 工具链）
cd D:\work\source\repos\MsixpkgInstaller
powershell -ExecutionPolicy Bypass -File scripts\build-nsis-installer.ps1
#   -BuildNative   顺便跑上面的 cmake（本地省事）
#   -PayloadDir    CI 的构建目录不是 build\x64\Release 时指过去
#   -NsisDir       用自己装的 NSIS（目录里要有 makensis.exe 与 Include\MUI2.nsh）
#   -NoProvision   不允许联网下载 NSIS 工具链
#   -Sign -PfxPath <pfx> -PfxPassword ***   用 signtool 签名产物
```

产物：`artifacts\setup\MsixpkgInstaller-<版本>-setup.exe` + 同名 `.sha256`。
版本号来自 `src\MsixpkgInstaller.Native\res\msixpkg.rc` 的 `FILEVERSION`（单一事实来源），
载荷大小自动算进 ARP 的 `EstimatedSize`。编译用 `/WX`：**有告警就是失败**。

### 为什么构建脚本要自己下载 NSIS

仓库不 vendor 工具链（几十 MB），所以脚本按需下载官方 3.10 的两个包到
`artifacts\tools\`，**校验 SHA-256** 后叠加成一份可用工具链：

| 包 | 作用 |
|---|---|
| `nsis-3.10.zip` | 完整发行版（`Include\MUI2.nsh`、字符串表、Plugins…） |
| `nsis-3.10-strlen_8192.zip` | **只是覆盖包**：`makensis.exe` + `Bin` + `Stubs`，把字符串上限提到 8192 |

必须叠加第二个包，原因见 §5 的第 3 条。脚本会在 `makensis /HDRINFO` 里验
`NSIS_MAX_STRLEN >= 8192`，不满足直接失败（缺了它宁可编不出来，也不要出一个会截断
用户 PATH 的安装包）。

## 5. 踩过的坑（都是实测出来的，别改回去）

| # | 坑 | 正确做法 |
|---|---|---|
| 1 | `FileOpen $9 "x.log" a` **不会**定位到文件末尾（实测：第二次写从 offset 0 覆盖） | 打开后必须 `FileSeek $9 0 END` 再写，否则日志只剩最后一次运行的最后一行 |
| 2 | NSIS 在启动时就消费掉 `/D=` 并把它从 `$CMDLINE` / `${GetParameters}` 里删掉 | 判断「用户有没有显式给目录」只能靠 `$INSTDIR` 是否仍等于 `InstallDir` 默认值；第一版漏了这步，`.onInit` 用默认值覆盖 `/D=`，**静默安装装到了别处还退 0** |
| 3 | 官方 NSIS 默认构建的 `NSIS_MAX_STRLEN=1024`，`ReadRegStr` 读长 PATH 会**静默截断**，写回去就是破坏用户的 PATH | 用官方 strlen_8192 覆盖包；运行时再用注册表报告的**真实字节数**挡第二次（越界就拒绝改动 PATH 并写日志） |
| 4 | NSIS 的 `SetOutPath`/`File` 失败**不会**让安装失败 → 「什么都没装」也退 0 | 复制完显式 `IfFileExists` 复查三个文件，缺了就 `SetErrorLevel 4` + `Abort`（E2E 里专门有一条盯着这个） |
| 5 | 卸载时 `RMDir "$INSTDIR"` 会因为目录里还剩 `Uninstall.exe`（以及自己可能是该目录的当前目录）而失败 | 卸载段先 `SetOutPath "$TEMP"`，再显式删 `Uninstall.exe`，最后 `RMDir`；用 `_?=` 调用时原件正在运行，删不掉是正常的（由调用方收尾） |
| 6 | `LangString` 的文本是运行时从字符串表读出来的，里面的 `$变量` **不会**展开 | 带路径/版本的消息一律「LangString 文案 + 运行时字符串拼接」 |
| 7 | 字符串里的双引号不能写 `\"` | 用 `$\"`（NSIS 的转义） |
| 8 | `${Using:StrFunc} StrStr` 才是正确声明形式；卸载器里要用 `${UnStrStr}`（StrFunc 另生成一个 `un.StrStr`，不能互借） | 见脚本头部；`${StrFunc} StrStr` 会被当成命令 `String` 报错 |
| 9 | NSIS 自上而下编译：`.onInit` 里引用 `${SEC_*}` 会「unknown variable」（Section 还没被处理） | 组件默认值放到 `ApplyComponentSwitches`，`.onInit` 里 `Call` 它（函数可以前向引用） |
| 10 | 受限执行环境里 **子进程**写不了 `%TEMP%`/`HKCU`（哪怕当前 shell 可以） | 测试脚本把 `TEMP`/`TMP` 指到工作区内、安装目录也用工作区路径；日志与 PATH 的失败路径要有明确上报 |
| 11 | 用 `_?=` 调用卸载器时，卸载器不会删除自己，目录会留在原地 | 这是 NSIS 的语义（调用方负责收尾），E2E 里按这个语义断言并自己清理 |
| 12 | **所有** NSIS 安装包在跑脚本之前就要能写 `%TEMP%`（stub 自建临时目录）；写不了就直接弹 `NSIS Error: Error writing temporary file`，**脚本一行都没跑**，所以安装包自己没法给更友好的提示。**本机实测**：装了 360 安全套装后，未签名的 exe 在 `C:` 上连临时文件都建不了（探针：`GetTempFileName` → 错误 5 = ACCESS_DENIED），而签名的 `pwsh`/`cmd` 正常、`D:` 上的临时目录也正常 —— 一个 39 KB 的空白 NSIS 安装包因此同样退 2 | 开发/测试：把 `TEMP`/`TMP` 指到工作区（`tests/e2e/nsis-installer-e2e.ps1` 就是这么做的）。用户机：给安装包加安全软件排除项、或签受信任的证书、或直接用便携 zip，见 §7 |

### 已经核实、别再猜的两条

| 结论 | 依据 |
|---|---|
| `StrCpy $dst $src 0` 给的是**空串**，不是「整串复制」（`-1` 之类的写法才是另一回事） | 探针实测 + `tests/nsis/path-logic.nsi` 的 `leading-empty` / `middle-empty` 用例。PathScan 直接靠这一点正确处理空段，别照着旧文档去「修」它 |
| `makensis` 默认会把工作目录切到 `.nsi` 所在目录 | 想控制产物落点要么用绝对 `OutFile`（经 `build-defines.nsh` 注入），要么用 `/NOCD` 并自己设好 CWD（`tests\nsis\path-logic.nsi` 的跑法） |

## 6. 验收

```powershell
# 不需要管理员；会在工作区内真的装一次、卸一次
powershell -ExecutionPolicy Bypass -File tests\e2e\nsis-installer-e2e.ps1
#   -SetupPath / -PayloadDir / -WorkDirectory / -KeepWork
```

覆盖：静默安装与 `/D=`、载荷逐字节一致（SHA-256）、装完的 CLI 能跑、日志内容、
重复安装、`/NOASSOC /NOPATH`、`/ALLUSERS` 未提权退 5、不可创建的目标退 4、
ARP 路径卸载与 `_?=` 卸载、安装记录不被删除、注册表效果（安装记录 / ARP /
关联 / PATH 的写入与清理）、便携包的解压与可运行性，以及 PATH 逻辑的 13 个 NSIS 层用例
（空串、空段、重复、前缀陷阱、大小写、带引号条目；见
[`tests/nsis/path-logic.nsi`](../../tests/nsis/path-logic.nsi)）。

**环境差异要如实看**：这个仓库的受限执行环境不允许**子进程**写 `HKCU`，
所以上面的「注册表效果」那一段会打印 `SKIP` 并说明原因；作为补偿，套件会断言
安装器**如实上报**了这些失败（日志里有 `WARNING` / `FAILED`），而不是假装成功。
在普通机器或 CI 上跑同一个脚本，这些断言就会真的执行（CI 的 `nsis-package`
作业就是为此存在的，见 `.github/workflows/ci.yml`）。
同一套件的「安装」那几步必须把 `TEMP`/`TMP` 指向工作区内，否则 NSIS stub 连启动都
起不来（§5 第 12 条）——这是环境约束，不是安装包的问题，套件里已按此设置。

## 7. 双击起不来 / 非提权装不全：`%TEMP%` 与免提权启动器

**现象**：双击 `setup.exe` 只弹

```
NSIS Error
Error writing temporary file. Make sure your temp folder is valid.
```

**原因**：NSIS 的 stub 在跑脚本**之前**就要能写 `%TEMP%`（建自己的临时目录）。写不了时
连 `.onInit` 都没机会执行 —— 所以安装包自己来不及给任何更友好的提示。**任何** NSIS 安装包
都是这样，与本脚本无关：用一个 3 行脚本编出来的最小安装包在本机表现完全一样。

**本机实测的判定表**（同一台机器、同一个安装包）：

| 启动方式 | `TEMP` | 结果 |
|---|---|---|
| 非提权（资源管理器 / 任务计划程序 / 会话子进程） | 仓库工作区内 | **exit 0**，三个文件都装好 |
| 非提权 | `%LOCALAPPDATA%\Temp`、`D:\某个目录` 等 | **exit 2**，弹上面那条 NSIS 错误 |
| 提权（右键「以管理员身份运行」） | 任意 | 正常（提权令牌不受该拦截约束） |

**为什么会这样**：本机同时装着**火绒 HIPS**（`HipsDaemon` / `HipsTray` / `wsctrlsvc`）与
**360 安全套装**（Defender 实时保护是关的）。这类主动防御默认拦截**未签名**程序创建临时
文件与写注册表（`GetTempFileName` → 错误 5 = `ACCESS_DENIED`），提权进程与已信任的程序
（例如 `reg.exe`）则放行 —— 已实测：同一个非提权上下文里 `reg.exe` 能写 `HKCU`，而安装包
的注册表写入被拒（`setup.log` 里会留下 `WARNING - install records could not be written` 等）。

**结论**：在**本机这种带主动防御的环境里，非提权装不全（关联 / PATH / ARP 写不进去）是
预期的**；在普通机器上，`setup.exe` 非提权双击就是「只为我安装」，四项全都写 `HKCU`，属正常。
「以管理员身份运行能装」也符合预期：提权令牌绕过了拦截。

### 不用管理员也能跑起来的三条路

1. **`run-setup-with-local-temp.cmd`（构建脚本生成在 setup 旁边，推荐）**
   先把 `TEMP`/`TMP` 指到安装包自己目录下的 `.installer-temp`（失败再退到
   `%LOCALAPPDATA%\MsixpkgInstaller\temp`，最后才是原 `%TEMP%`），再启动 setup；参数原样
   转发（`/S /CURRENTUSER /D=...` 都行）。有了可写的临时目录，非提权也能**复制程序文件**；
   但注册表那几项仍可能被主动防御拒绝 —— 那时它们会**如实写进 `setup.log`**（退出码仍是 0，
   因为程序文件确实装好了）。
   E2E 里有这条对照：同一个「不可用的 `%TEMP%`」下，直接跑 exit 2，用启动器 exit 0。
2. **便携包**（见下）：完全不碰临时目录，也不注册任何东西。
3. **根治**：给 `setup.exe` **签一个受信任的证书**（`-Sign`），或把安装包/证书加入
   火绒、360 的信任白名单。这是唯一能让「非提权 + 全量安装（含关联/PATH/ARP）」在带主动
   防御的机器上成立的办法。自签名证书只适合内部分发，且需要显式让安全软件信任它。

## 7.1 便携包：完全不依赖临时目录

`artifacts\setup\MsixpkgInstaller-<版本>-portable.zip` 里就是那两个 exe 加一份说明：

```
msixpkg-installer.exe   向导
msixpkg-native.exe      CLI
README-portable.txt     英文说明（保持 ASCII，任何机器上都能读）
```

容器、Windows Sandbox、被组策略锁死的机器、以及上面那种被主动防御拦截的环境，用它即可
绕开全部安装机制。**代价**：没有 `.msixpkg` 文件关联、没有开始菜单/桌面快捷方式、没有
「设置 → 应用」条目；「卸载」＝删掉那个目录。想在任何 shell 里用 CLI，把解压目录加进 PATH。

## 8. 分发建议

- 自签名证书只适合内部分发；公网分发请用受信 CA / Azure Artifact Signing 签这个
  `setup.exe`（`-Sign` 会调 `signtool`）。注意：**签名不能替代** `.msixpkg` 自己的
  发布者钉扎 —— 那一条在 `msixpkg-installer.exe` 里，与本安装包无关。
- 批量部署：`setup.exe /S /ALLUSERS /NOASSOC /NODESKTOP`（管理员上下文），
  卸载用 `"...\Uninstall.exe" /S`；如果目标机器连 `%TEMP%` 都写不了，改推便携 zip。
