# Msixpkg Installer {{VERSION}} 安装与使用指引

Msixpkg Installer 是**用来安装 `.msixpkg` 应用的安装器**：一个 Win32 图形向导
（`msixpkg-installer.exe`）加一个命令行工具（`msixpkg-native.exe`）。本压缩包就是它的分发安装包。

它本身**不安装任何应用**，也**不会**主动改动证书存储；应用和发布者证书都只在你在向导里明确同意后才会处理。

## ⚠️ 安装本安装器需要「以管理员身份运行」

**右键 `{{SETUP_FILE}}` →「以管理员身份运行」，然后在 UAC 询问里点「是」。**

请不要直接双击。安装包自身以 `asInvoker` 运行，**不会自己弹 UAC**；只有你右键提权，它才能：

- 安装到 `%ProgramFiles%\Msixpkg Installer`，并为**所有用户**注册开始菜单、文件关联、
  PATH 和「设置 → 应用」里的卸载入口（写 `HKLM`）；
- 避开 360、火绒等主动防御对未提权程序写注册表 / 临时目录的拦截。

不提权时安装器会退化为「只为我安装」（`%LOCALAPPDATA%\Programs\Msixpkg Installer` + `HKCU`）。
在普通机器上这也算装上了，但文件关联、PATH、卸载入口可能缺失；在带主动防御的机器上这些写入
还可能直接失败（`setup.log` 会如实记录）。**之后的升级，以及机器级安装的卸载，同样需要管理员权限。**

## 压缩包内容

| 文件 | 说明 |
|---|---|
| `{{SETUP_FILE}}` | 安装程序 |
| `{{SETUP_FILE}}.sha256` | 安装程序的 SHA-256 校验值（供 `Get-FileHash` 比对） |
| `README.md` | 本说明 |

## 系统要求

- Windows 10 2004（build 19041）或更高版本，或 Windows 11；
- **仅支持 x64**（ARM64 请勿使用本包）；
- 安装时具有管理员权限（见上一节）；
- 磁盘占用很小：安装器只有两个可执行文件，外加少量注册表项。

## 安装步骤

1. 把 zip **完整解压**到本地目录（不要在压缩包预览窗口里直接运行）。
2. **右键** `{{SETUP_FILE}}` →**「以管理员身份运行」**，UAC 里点「是」。
3. 选择安装目录与组件：

   | 组件 | 默认 | 说明 |
   |---|---|---|
   | Msixpkg 安装器（必需） | 已选 | 向导、命令行、开始菜单快捷方式、「设置 → 应用」卸载入口 |
   | `.msixpkg` 文件关联 | 勾选 | 双击 `.msixpkg` 用向导打开并停在摘要页（仍需你确认后才安装） |
   | 把安装目录加入 PATH | 勾选 | 让 `msixpkg-native` 在任意新开的终端里可用 |
   | 桌面快捷方式 | 不勾 | 需要时自己勾选 |

4. 点击「安装」，完成页可勾选「立即运行 Msixpkg 安装器」。

安装位置与生效范围：

| 启动方式 | 安装目录 | 注册表 | 可见范围 |
|---|---|---|---|
| **以管理员身份运行（推荐）** | `%ProgramFiles%\Msixpkg Installer` | `HKLM` | 所有用户 |
| 未提权（直接双击） | `%LOCALAPPDATA%\Programs\Msixpkg Installer` | `HKCU` | 仅当前用户 |

安装日志：`%LOCALAPPDATA%\MsixpkgInstaller\setup.log`（该目录不可写时回落到
`%TEMP%\MsixpkgInstaller\setup.log`，UTF-16LE）。遇到问题时先看它。

## 静默安装（批量部署 / 脚本）

在**已经以管理员身份运行**的 PowerShell 或终端里执行：

```powershell
# 为所有用户安装（推荐）
.\{{SETUP_FILE}} /S /ALLUSERS /NODESKTOP

# 只为我安装（不需要管理员）
.\{{SETUP_FILE}} /S /CURRENTUSER /NODESKTOP

# 指定安装目录：/D= 必须放在最后，等号后不要加引号
.\{{SETUP_FILE}} /S /ALLUSERS /D=C:\Tools\Msixpkg
```

| 开关 | 作用 |
|---|---|
| `/S` | 静默安装 |
| `/ALLUSERS` | 为所有用户安装（需要管理员，否则退出码 5） |
| `/CURRENTUSER` | 只为我安装（不需要管理员） |
| `/D=<目录>` | 指定安装目录；**必须是最后一个参数**，且不要给目录加引号 |
| `/NOASSOC` | 不注册 `.msixpkg` 关联 |
| `/NOPATH` | 不修改 PATH |
| `/NODESKTOP` | 不创建桌面快捷方式 |

静默安装退出码：`0` 成功 / `4` 文件没能真正拷完 / `5` 需要管理员 / `6` 系统不满足要求
（非 x64、低于 Windows 10）/ `7` 目标文件被正在运行的向导占用。

## 安装之后：怎么用

### 图形向导

- **安装应用**：双击任意 `.msixpkg` 文件（若装了文件关联），向导会停在**摘要页**；
  检查名称、版本、发布者后点「安装」。也可以从开始菜单打开「Msixpkg 安装器」，再选择容器文件。
- **首次遇到某个发布者**：向导会要求你确认是否信任该发布者；确认后，容器内嵌的发布者证书会写入
  `LocalMachine\TrustedPeople`（**不会**写入 `Root`）。未命中钉扎白名单的发布者默认被拒绝，
  你可以勾选「记住」把它加入本机白名单。
- **管理已安装的应用**：开始菜单 →「管理已安装的应用」（等价于 `msixpkg-installer.exe --manage`）。
- **卸载应用**：在「设置 → 应用」里找到该应用点卸载（安装器注册的入口会自动带上包族名），
  或在管理页里卸载。卸载完成后，安装器会按引用计数决定是否回收当初导入的发布者证书。

### 命令行

```powershell
msixpkg-native --version
msixpkg-native help

# 查看、体检、清理
msixpkg-native list
msixpkg-native verify
msixpkg-native repair      # 只输出修复建议，不改动系统
msixpkg-native clean

# 安装（--dry-run 只打印计划，不碰系统）
msixpkg-native install App.msixpkg --pin <发布者证书SHA256> --require-pin --dry-run
msixpkg-native install App.msixpkg --require-pin

# 卸载
msixpkg-native uninstall --family-name <PackageFamilyName>
```

如果安装时没有勾选 PATH，或当前终端是安装前就打开的，请用完整路径：

```powershell
& "$env:ProgramFiles\Msixpkg Installer\msixpkg-native.exe" list
# 用户级安装（未提权）则在：
# & "$env:LOCALAPPDATA\Programs\Msixpkg Installer\msixpkg-native.exe" list
```

命令行退出码：`0` 成功 / `1` 未预期错误 / `2` 容器校验失败 / `3` 用法错误 / `4` I/O 错误 /
`5` 部署失败 / `6` 被取消（含拒绝提权）/ `7` 前置条件不满足。

## 卸载本安装器

- **图形方式**：「设置 → 应用」→ 找到 **Msixpkg 安装器 {{VERSION}}**（英文界面显示为
  **Msixpkg Installer {{VERSION}}**）→ 卸载；或开始菜单 →「卸载 Msixpkg 安装器」。
- 也可以直接运行 `%ProgramFiles%\Msixpkg Installer\Uninstall.exe`；机器级安装的卸载器会请求管理员权限。
- **静默卸载**（管理员终端）：

  ```powershell
  & "$env:ProgramFiles\Msixpkg Installer\Uninstall.exe" /S
  ```

卸载**只删除这个安装器本身**（程序文件、快捷方式、文件关联、PATH 条目、卸载入口）：

- **不会**卸载你已经安装的应用；
- **不会**删除 `%ProgramData%\Msixpkg` 下的安装记录 —— 那是以后回收发布者证书的依据。

## 校验安装包完整性

建议在运行前核对 SHA-256：

```powershell
Get-FileHash .\{{SETUP_FILE}} -Algorithm SHA256

# 期望值：{{SHA256}}
```

也可以直接比对同目录的 `{{SETUP_FILE}}.sha256`。如果哈希不一致，**不要运行**，重新获取安装包。

## 常见问题

1. **Windows 提示「已保护你的电脑」/「未知发布者」**
   安装包未使用受信任证书签名时会出现。确认来源与上面的 SHA-256 无误后，点「更多信息」→
   「仍要运行」。长期方案是给 `setup.exe` 签一个受信任的证书；自签名证书只适合内部分发。

2. **双击后只弹 `NSIS Error: Error writing temporary file`**
   NSIS 安装包在脚本执行前就必须能写 `%TEMP%`；未提权 + 安全软件主动防御拦临时文件时就会这样。
   **请按第一节右键「以管理员身份运行」**。仍然失败时：把 zip 解压到非系统盘、给安装包/解压目录
   加安全软件白名单，或改用便携版（不在本包内）。

3. **没有弹出 UAC**
   如果系统策略是 `ConsentPromptBehaviorAdmin=0`（或 `EnableLUA=0`），提权本来就不会弹窗。
   安装器会把生效策略写进 `setup.log`：`elevation policy: EnableLUA=… ConsentPromptBehaviorAdmin=…`。

4. **安装提示需要管理员权限（退出码 5）**
   使用了 `/ALLUSERS`，或机器上已有「为所有用户」的安装。请右键「以管理员身份运行」；
   如果只想装一份自己的副本，改用 `/CURRENTUSER`。

5. **退出码 7：目标文件被占用**
   有正在运行的 Msixpkg 安装器或正在进行的安装/卸载任务。关闭它们后重试。

6. **安装 `.msixpkg` 时被拒绝**
   发布者不在钉扎白名单里（默认拒绝）。用 `--pin <SHA256>` 显式指定，或由管理员把发布者
   加入 `HKLM\SOFTWARE\MsixpkgInstaller\AllowedPublishers`。不要为了绕过提示而把证书写进 `Root`。

7. **找不到 `msixpkg-native`**
   安装时没勾选 PATH，或当前终端在安装前就已打开。重开终端，或用上一节的完整路径。

## 安全说明

1. 本安装包只安装工具本身：不安装应用、不写证书存储、不删除安装记录。
2. 只有在你确认安装某个 `.msixpkg` 时，安装器才可能把该容器的发布者证书写入
   `LocalMachine\TrustedPeople`；**永不写入 `Root`**。
3. 容器内四重身份必须一致（容器签名者、`publisher.cer`、内层 `.msix` 签名者、清单 `Publisher`），
   并且默认只接受已钉扎的发布者。
4. 自签名证书只适合内部分发；公网分发请迁移到受信任 CA 或 Azure Artifact Signing。
5. 安装日志在 `%LOCALAPPDATA%\MsixpkgInstaller\setup.log`，可用于排查提权与注册表写入问题。

---

来源仓库的更多细节：`docs/security-model.md`（威胁模型与证书回收判定）、
`docs/msixpkg-format-v1.md`（`.msixpkg` 格式契约）、`installer/nsis/README.md`（分发安装包设计）。
