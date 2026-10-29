; ============================================================================
; Msixpkg Installer —— 用 NSIS 3 构建的分发安装包（"用安装包安装这个安装器"）
; ============================================================================
;
; 这个脚本打包的是 **Msixpkg 安装器本身**，也就是 .msixpkg 的安装器，因此它是
; 「安装包的安装包」。它负责把两个可执行文件装到机器上，并配好它们的使用入口：
;
;   msixpkg-installer.exe   向导前端（SUMMARY / CONSENT / PROGRESS / RESULT / MANAGE）
;   msixpkg-native.exe      命令行前端（install / uninstall / list / verify / repair / clean）
;
; 装完之后的一台机器上，用户能看到/用到的东西：
;   * 开始菜单 → Msixpkg Installer → 打开安装器 / 管理已安装的应用 / 卸载
;   * 可选：桌面快捷方式
;   * 可选：.msixpkg 文件关联（双击 = 用向导打开该容器）
;   * 可选：安装目录加入 PATH（让 msixpkg-native 在任意 shell 里可用）
;   * 「设置 → 应用」里的卸载入口（ARP）
;
; 它**刻意不做**的事（这几条是设计边界，不是遗漏）：
;   * 不安装任何 MSIX 应用 —— 那是 msixpkg-installer.exe 的职责；
;   * 不碰证书存储 —— 证书只能由向导/CLI 在用户明确同意后写入 TrustedPeople
;     （见 docs/security-model.md）；安装器本身没有任何证书写入路径；
;   * 不删 %ProgramData%\Msixpkg 下的状态库 —— 那是「证书回收」的记账，删了就没法回滚；
;   * 不写 ARP 里属于**已装应用**的条目（arp.cpp 写的是 Msixpkg.<IdentityName>），
;     只写自己这一条（MsixpkgInstaller），两者在「设置」里靠名字区分。
;
; ---------------------------------------------------------------------------
; 怎么构建
; ---------------------------------------------------------------------------
;   powershell -ExecutionPolicy Bypass -File scripts\build-nsis-installer.ps1
;     - 自动准备 NSIS 工具链（本机没有时下载官方包到 artifacts\tools\）
;     - 版本号从 src\MsixpkgInstaller.Native\res\msixpkg.rc 读，单一事实来源
;     - 产物写到 artifacts\setup\MsixpkgInstaller-<版本>-setup.exe
;
; 已经装好 NSIS 时也可以直接编（**必须**带 /INPUTCHARSET UTF8，本文件是 UTF-8 无 BOM）：
;   makensis /INPUTCHARSET UTF8 installer\nsis\MsixpkgInstaller.nsi
;
; ---------------------------------------------------------------------------
; 静默安装与开关（供 CI / E2E 使用，见 tests\e2e\nsis-installer-e2e.ps1）
; ---------------------------------------------------------------------------
;   /S                静默（NSIS 内建）
;   /D=<目录>         指定安装目录（NSIS 内建，必须是最后一个参数且不加引号）
;   /CURRENTUSER      强制「只为我安装」（HKCU + %LOCALAPPDATA%，不需要管理员）
;   /ALLUSERS         强制「为所有用户安装」（HKLM + %ProgramFiles%，需要管理员）
;   /NOASSOC          不注册 .msixpkg 文件关联
;   /NOPATH           不把安装目录加进 PATH
;   /NODESKTOP        不建桌面快捷方式
;
; 默认行为：已提权 → 为所有用户安装；未提权 → 只为我安装。
; /D= 的优先级最高：只要命令行里给过 /D=，安装器绝不会用「默认目录」覆盖它。
; 退出码：0 成功 / 4 安装或卸载没能真正完成（文件写不进/删不掉） /
;         5 需要管理员 / 6 前置条件不满足 / 7 目标被占用。
;
; 日志：每次运行的每一步都追加写进 setup.log（UTF-16LE，无 BOM），目录按可写性探测
;   %LOCALAPPDATA%\MsixpkgInstaller  →  %TEMP%\MsixpkgInstaller
; 静默安装没有任何界面，这个文件就是唯一的现场记录（哪个目录、哪种模式、PATH
; 有没有真的写进去、失败在哪一步）。
;
; ---------------------------------------------------------------------------
; 两个实现上的硬约束（都踩过，别改回去）
; ---------------------------------------------------------------------------
; 1) PATH 组件要求 NSIS_MAX_STRLEN ≥ 8192。官方默认构建是 1024，读长 PATH 会
;    静默截断，写回去就是**破坏用户的 PATH**。所以编译期检查它，运行时再用注册表
;    报告的真实字节数挡第二次；宁可拒绝改动 PATH（并在日志里说明），也不截断。
;    官方提供 strlen_8192 覆盖包，构建脚本会自动叠加（见 README）。
; 2) 本文件是 UTF-8 无 BOM。makensis 默认按系统 ANSI 代码页读脚本，中文会变乱码，
;    因此构建脚本一律传 /INPUTCHARSET UTF8。

Unicode true

!include "MUI2.nsh"
!include "LogicLib.nsh"
!include "FileFunc.nsh"
!include "StrFunc.nsh"
!include "WinVer.nsh"
!include "x64.nsh"

; 声明 ${StrStr}（大小写不敏感的子串查找，用来解析命令行开关）。
; 注意语法是 ${Using:StrFunc}，不是 ${StrFunc} —— 后者会把整行当成命令 "String"。
; 卸载器里用的是 ${UnStrStr}（StrFunc 会另外生成一个 un.StrStr，两者不能互借）。
${Using:StrFunc} StrStr
${Using:StrFunc} UnStrStr

; ---------------------------------------------------------------------------
; 构建期定义：由 scripts\build-nsis-installer.ps1 生成 build-defines.nsh（引号内的路径，
; 避免命令行 /D 的引号与空格问题）。文件不存在时用下面的缺省值，直接 makensis 也能编。
; ---------------------------------------------------------------------------
!if /FILEEXISTS "${__FILEDIR__}\build-defines.nsh"
  !include "${__FILEDIR__}\build-defines.nsh"
!endif

!ifndef PRODUCT_VERSION
  !define PRODUCT_VERSION "0.2.1"
!endif
!ifndef PRODUCT_VERSION_QUAD
  !define PRODUCT_VERSION_QUAD "0.2.1.0"
!endif
!ifndef APP_GUI
  !define APP_GUI "${__FILEDIR__}\..\..\src\MsixpkgInstaller.Native\build\x64\Release\msixpkg-installer.exe"
!endif
!ifndef APP_CLI
  !define APP_CLI "${__FILEDIR__}\..\..\src\MsixpkgInstaller.Native\build\x64\Release\msixpkg-native.exe"
!endif
!ifndef APP_ICON
  !define APP_ICON "${__FILEDIR__}\..\..\src\MsixpkgInstaller.Native\res\msixpkg.ico"
!endif
!ifndef OUTPUT_DIR
  !define OUTPUT_DIR "${__FILEDIR__}\..\..\artifacts\setup"
!endif
!ifndef OUTPUT_NAME
  !define OUTPUT_NAME "MsixpkgInstaller-${PRODUCT_VERSION}-setup.exe"
!endif
; ARP 的 EstimatedSize（KB）。0 = 不写该字段；构建脚本按实际产物计算后覆盖。
!ifndef ESTIMATED_SIZE_KB
  !define ESTIMATED_SIZE_KB 0
!endif

!if ${NSIS_MAX_STRLEN} < 8192
  !warning "NSIS_MAX_STRLEN=${NSIS_MAX_STRLEN} < 8192：PATH 组件会在较长的 PATH 上拒绝改动（安全，但会经常用不了）。构建脚本会用官方 strlen_8192 覆盖包。"
!endif

; ---------------------------------------------------------------------------
; 常量
; ---------------------------------------------------------------------------
!define COMPANY            "MsixpkgInstaller"
!define PRODUCT_NAME       "Msixpkg Installer"                  ; 语言中立（目录名 / 注册表值名）
!define SM_FOLDER          "Msixpkg Installer"                  ; 语言中立：卸载时按记录清理
!define CFG_KEY            "Software\MsixpkgInstaller"          ; 安装记录（InstallDir / 模式 / PATH …）
!define UNINST_KEY         "Software\Microsoft\Windows\CurrentVersion\Uninstall\MsixpkgInstaller"
!define CAPS_KEY           "Software\MsixpkgInstaller\Capabilities"
!define REGISTERED_APPS    "Software\RegisteredApplications"
!define CLASSES_KEY        "Software\Classes"
!define EXT                ".msixpkg"
!define PROGID             "Msixpkg.Package"
!define MACHINE_PATH_KEY   "SYSTEM\CurrentControlSet\Control\Session Manager\Environment"
!define USER_PATH_KEY      "Environment"
!define HKEY_CURRENT_USER  0x80000001
!define HKEY_LOCAL_MACHINE 0x80000002
!define KEY_QUERY_VALUE    0x0001
!define SETUP_LOG_DIR      "$LOCALAPPDATA\MsixpkgInstaller"

Name "$(PRODUCT_NAME_UI)"
OutFile "${OUTPUT_DIR}\${OUTPUT_NAME}"
InstallDir "$PROGRAMFILES64\${PRODUCT_NAME}"
RequestExecutionLevel user
; 提权只由用户显式选择「为所有用户安装」时通过右键「以管理员身份运行」完成，
; 或者由卸载器在发现这是机器级安装时用 runas 重新拉起自己（见 un.onInit）。
ShowInstDetails show
ShowUninstDetails show
SetCompressor /SOLID lzma
Icon "${APP_ICON}"
UninstallIcon "${APP_ICON}"
BrandingText "$(PRODUCT_NAME_UI) ${PRODUCT_VERSION}"
VIProductVersion "${PRODUCT_VERSION_QUAD}"
VIAddVersionKey "CompanyName"     "${COMPANY}"
VIAddVersionKey "ProductName"     "${PRODUCT_NAME}"
VIAddVersionKey "FileDescription" "${PRODUCT_NAME} Setup"
VIAddVersionKey "FileVersion"     "${PRODUCT_VERSION_QUAD}"
VIAddVersionKey "ProductVersion"  "${PRODUCT_VERSION_QUAD}"
VIAddVersionKey "LegalCopyright"  ""

; ---------------------------------------------------------------------------
; 变量
; ---------------------------------------------------------------------------
Var InstallMode        ; "allusers" / "currentuser"
Var IsAdmin            ; "1" / "0"
Var SilentMode         ; "1" / "0"
Var NoAssoc            ; 开关
Var NoPath
Var NoDesktop
Var ForceAllUsers
Var ForceCurrentUser
Var ExistingMachineDir ; HKLM 里的旧安装目录（空 = 没有机器级安装）
Var ExistingUserDir    ; HKCU 里的旧安装目录


; ---------------------------------------------------------------------------
; PATH 组件（扫描 / 读写 / 截断保护）在 msixpkg-path.nsh 里单独放一份，就是为了能被
; tests/nsis/path-logic.nsi 直接调用宏做单元测试 —— 这段逻辑改坏会毁掉用户的 PATH。
; ---------------------------------------------------------------------------
!include "msixpkg-path.nsh"

; 清理用
Var RunningWizard
Var StateRecordsFound
Var ExistingVersion
Var DirOverridden      ; 命令行里出现了 /D=（NSIS 已经把它写进 $INSTDIR）
Var LogDir             ; 实际可写的日志目录（首次写日志时探测）
Var LogText

; ---------------------------------------------------------------------------
; 日志
; 每行 DetailPrint（GUI 的「显示详细信息」里可见）+ 追加到日志文件。
; 目录按「可写性」探测：%LOCALAPPDATA%\MsixpkgInstaller → %TEMP%\MsixpkgInstaller。
; 之所以要有回落链：静默安装时这个文件往往是唯一的现场记录，写不进去就等于没有证据。
; （受限执行环境里 %LOCALAPPDATA% 可能写不了，见 docs/handoff.md 的环境限制。）
; ---------------------------------------------------------------------------
!macro LOG Text
  DetailPrint "${Text}"
  StrCpy $LogText "${Text}"
  Call WriteLogFile
!macroend

!macro UNLOG Text
  DetailPrint "${Text}"
  StrCpy $LogText "${Text}"
  Call un.WriteLogFile
!macroend

!macro WRITE_LOG_BODY
  ${If} $LogDir == ""
    StrCpy $LogDir "${SETUP_LOG_DIR}"
    CreateDirectory "$LogDir"
    ClearErrors
    FileOpen $9 "$LogDir\setup.log" a
    ${If} ${Errors}
      StrCpy $LogDir "$TEMP\MsixpkgInstaller"
      CreateDirectory "$LogDir"
      ClearErrors
      FileOpen $9 "$LogDir\setup.log" a
      ${If} ${Errors}
        StrCpy $LogDir ""
      ${Else}
        FileClose $9
      ${EndIf}
    ${Else}
      FileClose $9
    ${EndIf}
  ${EndIf}

  ${If} $LogDir != ""
    ClearErrors
    FileOpen $9 "$LogDir\setup.log" a
    ${IfNot} ${Errors}
      ; 必须显式 FileSeek 到末尾：NSIS 的 FileOpen "a" **不会**定位到文件末尾
      ; （实测：第二次写从 offset 0 开始，把已有内容盖掉）。少了这一行，日志只剩下
      ; 每次运行最后写的那一行 —— 排查问题时最需要的前几行全没了。
      FileSeek $9 0 END
      ; 用 UTF-16LE 而不是 ANSI：日志里有中文，ANSI 会让它在别的代码页机器上变成乱码，
      ; 而日志恰恰是给别人看现场用的。（读取：Get-Content -Encoding Unicode）
      FileWriteUTF16LE $9 "[$InstallMode] $LogText$\r$\n"
      FileClose $9
    ${EndIf}
  ${EndIf}
!macroend

Function WriteLogFile
  !insertmacro WRITE_LOG_BODY
FunctionEnd

Function un.WriteLogFile
  !insertmacro WRITE_LOG_BODY
FunctionEnd

; ---------------------------------------------------------------------------
; 宏：管理员判定（推栈返回 "1"/"0"）
; shell32!IsUserAnAdmin 反映的是**当前令牌**，正是「现在能不能写 HKLM」的语义；
; 它是弃用 API 但仍随 Windows 10/11 提供。插件调用失败时退回注册表探针。
; ---------------------------------------------------------------------------
!macro IS_ADMIN_BODY
  ClearErrors
  System::Call 'shell32::IsUserAnAdmin()i.r0'
  ${If} ${Errors}
    ClearErrors
    WriteRegStr HKLM "Software\MsixpkgInstallerProbe" "Probe" "1"
    ${If} ${Errors}
      Push "0"
    ${Else}
      DeleteRegKey HKLM "Software\MsixpkgInstallerProbe"
      Push "1"
    ${EndIf}
  ${ElseIf} $0 == 0
    Push "0"
  ${Else}
    Push "1"
  ${EndIf}
!macroend

Function IsAdminUser
  !insertmacro IS_ADMIN_BODY
FunctionEnd

Function un.IsAdminUser
  !insertmacro IS_ADMIN_BODY
FunctionEnd


; ---------------------------------------------------------------------------
; 安装：把安装目录加进 PATH（已存在则不动）
; ---------------------------------------------------------------------------
Function AddInstallDirToPath
  Call ReadPathValue

  ${If} $PathTooLong == "1"
    StrCpy $R4 "$(MSG_PATH_TOO_LONG)$\r$\n$INSTDIR"
    !insertmacro LOG "$R4"
    Return
  ${EndIf}

  StrCpy $PathEntry "$INSTDIR"
  StrCpy $PathNew "$PathEntry"
  !insertmacro PathScan
  ${If} $PathHit == "1"
    StrCpy $R4 "$(MSG_PATH_ALREADY)$\r$\n$INSTDIR"
    !insertmacro LOG "$R4"
    Return
  ${EndIf}

  ${If} $PathRebuilt != ""
    StrCpy $PathNew "$PathRebuilt;$PathEntry"
  ${EndIf}

  Call WritePathValue
  ${If} $WriteFailed == "1"
    StrCpy $R4 "$(MSG_PATH_WRITE_FAILED)$\r$\n$INSTDIR"
    !insertmacro LOG "$R4"
    Return
  ${EndIf}
  StrCpy $R4 "$(MSG_PATH_ADDED)$\r$\n$INSTDIR"
  !insertmacro LOG "$R4"
FunctionEnd

; ---------------------------------------------------------------------------
; 卸载：按条目精确移除安装目录（PATH 里只有我们一条时删掉整个值）
; ---------------------------------------------------------------------------
Function un.RemoveInstallDirFromPath
  Call un.ReadPathValue

  ${If} $PathTooLong == "1"
    StrCpy $R4 "$(MSG_PATH_TOO_LONG_UNINST)$\r$\n$INSTDIR"
    !insertmacro UNLOG "$R4"
    Return
  ${EndIf}

  ${If} $PathValue == ""
    Return
  ${EndIf}

  StrCpy $PathEntry "$INSTDIR"
  !insertmacro PathScan
  ${If} $PathHit != "1"
    Return
  ${EndIf}

  ${If} $PathRebuilt == ""
    Call un.DeletePathValue
    StrCpy $R4 "$(MSG_PATH_REMOVED_ALL)$\r$\n$INSTDIR"
    !insertmacro UNLOG "$R4"
  ${Else}
    StrCpy $PathNew "$PathRebuilt"
    Call un.WritePathValue
    ${If} $WriteFailed == "1"
      StrCpy $R4 "$(MSG_PATH_WRITE_FAILED)$\r$\n$INSTDIR"
      !insertmacro UNLOG "$R4"
    ${Else}
      StrCpy $R4 "$(MSG_PATH_REMOVED)$\r$\n$INSTDIR"
      !insertmacro UNLOG "$R4"
    ${EndIf}
  ${EndIf}
FunctionEnd

; ---------------------------------------------------------------------------
; 安装记录：同一份键名在两个配置单元里各自存在，按 $InstallMode 选根
; ---------------------------------------------------------------------------
!macro CFG_WRITE_STR Key Value
  ${If} $InstallMode == "allusers"
    WriteRegStr HKLM "${CFG_KEY}" "${Key}" "${Value}"
  ${Else}
    WriteRegStr HKCU "${CFG_KEY}" "${Key}" "${Value}"
  ${EndIf}
!macroend

!macro CFG_WRITE_DWORD Key Value
  ${If} $InstallMode == "allusers"
    WriteRegDWORD HKLM "${CFG_KEY}" "${Key}" ${Value}
  ${Else}
    WriteRegDWORD HKCU "${CFG_KEY}" "${Key}" ${Value}
  ${EndIf}
!macroend

; ---------------------------------------------------------------------------
; 文件关联：整块只写一遍，用宏参数区分配置单元
; ---------------------------------------------------------------------------
!macro ASSOC_WRITE ROOT
  WriteRegStr ${ROOT} "${CLASSES_KEY}\${EXT}" "" "${PROGID}"
  WriteRegStr ${ROOT} "${CLASSES_KEY}\${EXT}\OpenWithProgids" "${PROGID}" ""
  WriteRegStr ${ROOT} "${CLASSES_KEY}\${PROGID}" "" "$(ASSOC_TYPE_NAME)"
  WriteRegStr ${ROOT} "${CLASSES_KEY}\${PROGID}" "FriendlyTypeName" "$(ASSOC_TYPE_NAME)"
  WriteRegStr ${ROOT} "${CLASSES_KEY}\${PROGID}\DefaultIcon" "" "$INSTDIR\msixpkg-installer.exe,0"
  WriteRegStr ${ROOT} "${CLASSES_KEY}\${PROGID}\shell\open\command" "" '"$INSTDIR\msixpkg-installer.exe" "%1"'
  ; Capabilities 让「设置 → 默认应用」里能看到我们（Windows 10 起不允许程序
  ; 自行改 UserChoice，所以这里只是提供入口，最终由用户点一下设为默认）
  WriteRegStr ${ROOT} "${CAPS_KEY}" "ApplicationName" "$(PRODUCT_NAME_UI)"
  WriteRegStr ${ROOT} "${CAPS_KEY}" "ApplicationDescription" "$(CAPS_DESC)"
  WriteRegStr ${ROOT} "${CAPS_KEY}\FileAssociations" "${EXT}" "${PROGID}"
  WriteRegStr ${ROOT} "${REGISTERED_APPS}" "${PRODUCT_NAME}" "${CAPS_KEY}"
!macroend

!macro ASSOC_REMOVE ROOT
  ; 只有默认值还是我们时才清掉 .msixpkg，否则会覆盖用户后来选的其它处理程序
  ReadRegStr $0 ${ROOT} "${CLASSES_KEY}\${EXT}" ""
  ${If} $0 == "${PROGID}"
    DeleteRegValue ${ROOT} "${CLASSES_KEY}\${EXT}" ""
  ${EndIf}
  DeleteRegValue ${ROOT} "${CLASSES_KEY}\${EXT}\OpenWithProgids" "${PROGID}"
  DeleteRegKey ${ROOT} "${CLASSES_KEY}\${EXT}\OpenWithProgids"
  DeleteRegKey /ifempty ${ROOT} "${CLASSES_KEY}\${EXT}"
  DeleteRegKey ${ROOT} "${CLASSES_KEY}\${PROGID}"
  DeleteRegKey ${ROOT} "${CAPS_KEY}"
  DeleteRegValue ${ROOT} "${REGISTERED_APPS}" "${PRODUCT_NAME}"
!macroend

; ---------------------------------------------------------------------------
; 页面
; ---------------------------------------------------------------------------
!define MUI_ICON "${APP_ICON}"
!define MUI_UNICON "${APP_ICON}"
!define MUI_ABORTWARNING
!define MUI_WELCOMEPAGE_TITLE "$(WELCOME_TITLE)"
!define MUI_WELCOMEPAGE_TEXT "$(WELCOME_TEXT)"
!define MUI_DIRECTORYPAGE_TEXT_TOP "$(DIR_TEXT_TOP)"
!define MUI_FINISHPAGE_TITLE "$(FINISH_TITLE)"
!define MUI_FINISHPAGE_TEXT "$(FINISH_TEXT)"
!define MUI_FINISHPAGE_RUN "$INSTDIR\msixpkg-installer.exe"
!define MUI_FINISHPAGE_RUN_TEXT "$(FINISH_RUN_TEXT)"
!define MUI_UNCONFIRMPAGE_TEXT_TOP "$(UNCONFIRM_TEXT)"

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "SimpChinese"
!insertmacro MUI_LANGUAGE "English"

; ---------------------------------------------------------------------------
; 界面文案（安装期语言 = 系统 UI 语言；NSIS 会自动选表）
; 注意：**卸载不依赖任何文案**。卸载要删的路径/键都记在注册表里，所以安装与
; 卸载语言不同也不会留下残渣（首版设计里踩过这个坑）。
; ---------------------------------------------------------------------------
LangString PRODUCT_NAME_UI     ${LANG_SIMPCHINESE} "Msixpkg 安装器"
LangString PRODUCT_NAME_UI     ${LANG_ENGLISH}     "Msixpkg Installer"

LangString WELCOME_TITLE       ${LANG_SIMPCHINESE} "安装 Msixpkg 安装器 ${PRODUCT_VERSION}"
LangString WELCOME_TITLE       ${LANG_ENGLISH}     "Install Msixpkg Installer ${PRODUCT_VERSION}"
LangString WELCOME_TEXT        ${LANG_SIMPCHINESE} "这个安装包会装上「用来安装 .msixpkg 的安装器」：一个 Win32 向导和一个命令行工具。$\r$\n$\r$\n它不安装任何应用，也不会改动证书存储 —— 应用与发布者证书都由安装器在你的明确同意下处理。"
LangString WELCOME_TEXT        ${LANG_ENGLISH}     "This setup installs the tool that installs .msixpkg packages: a Win32 wizard and a command line frontend.$\r$\n$\r$\nIt installs no applications and never touches the certificate store; both are handled by the wizard with your explicit consent."

LangString DIR_TEXT_TOP         ${LANG_SIMPCHINESE} "安装目录。安装目录会被加入 PATH（如果勾选了那一项），因此请避免使用带空格的目录。"
LangString DIR_TEXT_TOP         ${LANG_ENGLISH}     "Destination folder. It is added to PATH when that component is selected, so avoid spaces here."

LangString SEC_CORE             ${LANG_SIMPCHINESE} "Msixpkg 安装器（必需）"
LangString SEC_CORE             ${LANG_ENGLISH}     "Msixpkg Installer (required)"
LangString SEC_CORE_DESC        ${LANG_SIMPCHINESE} "向导 msixpkg-installer.exe、命令行 msixpkg-native.exe、开始菜单快捷方式与「设置 → 应用」里的卸载入口。"
LangString SEC_CORE_DESC        ${LANG_ENGLISH}     "The wizard (msixpkg-installer.exe), the CLI (msixpkg-native.exe), Start menu shortcuts and the Apps & features uninstall entry."

LangString SEC_ASSOC            ${LANG_SIMPCHINESE} ".msixpkg 文件关联"
LangString SEC_ASSOC            ${LANG_ENGLISH}     ".msixpkg file association"
LangString SEC_ASSOC_DESC       ${LANG_SIMPCHINESE} "双击 .msixpkg 时用向导打开并进入摘要页（不会自动安装，仍需你在同意页确认）。"
LangString SEC_ASSOC_DESC       ${LANG_ENGLISH}     "Opening a .msixpkg file starts the wizard on the review page. It never installs without your consent."

LangString SEC_PATH             ${LANG_SIMPCHINESE} "把安装目录加入 PATH"
LangString SEC_PATH             ${LANG_ENGLISH}     "Add the install folder to PATH"
LangString SEC_PATH_DESC        ${LANG_SIMPCHINESE} "让你在任意 shell 里直接调用 msixpkg-native（install / list / verify / repair / clean）。卸载时会精确移除该条目。"
LangString SEC_PATH_DESC        ${LANG_ENGLISH}     "Lets you run msixpkg-native (install / list / verify / repair / clean) from any shell. The entry is removed again on uninstall."

LangString SEC_DESKTOP          ${LANG_SIMPCHINESE} "桌面快捷方式"
LangString SEC_DESKTOP          ${LANG_ENGLISH}     "Desktop shortcut"
LangString SEC_DESKTOP_DESC     ${LANG_SIMPCHINESE} "在桌面放一个「Msixpkg 安装器」快捷方式（默认不勾选）。"
LangString SEC_DESKTOP_DESC     ${LANG_ENGLISH}     "Creates a Msixpkg Installer shortcut on the desktop (not selected by default)."

LangString FINISH_TITLE         ${LANG_SIMPCHINESE} "Msixpkg 安装器已就绪"
LangString FINISH_TITLE         ${LANG_ENGLISH}     "Msixpkg Installer is ready"
LangString FINISH_TEXT          ${LANG_SIMPCHINESE} "把 .msixpkg 文件拖进向导窗口，或双击它，即可开始安装应用。$\r$\n$\r$\n命令行：msixpkg-native help"
LangString FINISH_TEXT          ${LANG_ENGLISH}     "Drop a .msixpkg file onto the wizard, or just double-click it, to install an app.$\r$\n$\r$\nCommand line: msixpkg-native help"
LangString FINISH_RUN_TEXT      ${LANG_SIMPCHINESE} "立即运行 Msixpkg 安装器"
LangString FINISH_RUN_TEXT      ${LANG_ENGLISH}     "Run Msixpkg Installer now"

LangString UNCONFIRM_TEXT       ${LANG_SIMPCHINESE} "卸载只删除这个安装器自己（程序文件、快捷方式、文件关联、PATH 条目、卸载入口）。$\r$\n$\r$\n已经装好的应用不会被卸载；%ProgramData%\Msixpkg 下的安装记录也会原样保留 —— 那是回收发布者证书的依据，删掉它们会让证书永远留在受信任存储里。"
LangString UNCONFIRM_TEXT       ${LANG_ENGLISH}     "Uninstalling removes only this tool (program files, shortcuts, file association, PATH entry, uninstall entry).$\r$\n$\r$\nInstalled applications are left alone, and the installation records under %ProgramData%\Msixpkg are kept: they are what allows the publisher certificate to be reclaimed later."

LangString MSG_NOT_X64          ${LANG_SIMPCHINESE} "这个安装包只提供 x64 版本。当前系统不是 64 位 Windows。"
LangString MSG_NOT_X64          ${LANG_ENGLISH}     "This setup is x64 only; no 64-bit Windows was detected."
LangString MSG_OLD_WINDOWS      ${LANG_SIMPCHINESE} "需要 Windows 10 或更高版本（安装器依赖 AddPackageByUriAsync，最低 Windows 10 2004 / build 19041）。"
LangString MSG_OLD_WINDOWS      ${LANG_ENGLISH}     "Windows 10 or later is required (the installer needs AddPackageByUriAsync; Windows 10 2004 / build 19041 minimum)."
LangString MSG_ALLUSERS_ADMIN   ${LANG_SIMPCHINESE} "「为所有用户安装」需要管理员权限。请右键本安装包 →「以管理员身份运行」，或去掉 /ALLUSERS 开关（那样只为你安装）。"
LangString MSG_ALLUSERS_ADMIN   ${LANG_ENGLISH}     "$\"Install for all users$\" requires administrator rights. Run this setup as administrator, or drop /ALLUSERS to install for the current user only."
; 注意：LangString 的文本是运行时从字符串表里读出来的，里面的 $变量 不会展开。
; 需要带路径/版本的消息一律「文案 + 变量分开拼」，不要写进 LangString。
LangString MSG_MACHINE_EXISTS   ${LANG_SIMPCHINESE} "检测到这台机器上已有的「为所有用户」安装，位置：$\r$\n$\r$\n更新或卸载它需要管理员权限，请右键本安装包 →「以管理员身份运行」。$\r$\n$\r$\n如果你只想要一份自己的副本，请加 /CURRENTUSER 开关。"
LangString MSG_MACHINE_EXISTS   ${LANG_ENGLISH}     "A per-machine installation already exists at:$\r$\n$\r$\nUpdating or removing it requires administrator rights; run this setup as administrator.$\r$\n$\r$\nTo install a private copy instead, pass /CURRENTUSER."
LangString MSG_CLOSE_WIZARD     ${LANG_SIMPCHINESE} "正在运行的 Msixpkg 安装器占用了要替换的文件。请先关闭它（以及正在进行的安装/卸载），再重试。"
LangString MSG_CLOSE_WIZARD     ${LANG_ENGLISH}     "A running Msixpkg Installer is holding the files to be replaced. Close it (and any install/uninstall in progress), then try again."

LangString MSG_PATH_TOO_LONG    ${LANG_SIMPCHINESE} "PATH 太长，超过本安装包能安全处理的上限，因此**没有改动 PATH**。请手工把下面这个目录加进 PATH："
LangString MSG_PATH_TOO_LONG    ${LANG_ENGLISH}     "PATH is longer than this setup can handle safely, so PATH was left untouched. Add this folder to PATH manually:"
LangString MSG_PATH_TOO_LONG_UNINST ${LANG_SIMPCHINESE} "PATH 太长，超过安全上限，因此卸载时没有改动它。如果下面这个目录还在 PATH 里，请手工删除："
LangString MSG_PATH_TOO_LONG_UNINST ${LANG_ENGLISH} "PATH is longer than the safe limit, so uninstall left it untouched. Remove this folder from PATH manually if it is still there:"
LangString MSG_PATH_ADDED       ${LANG_SIMPCHINESE} "已加入 PATH（对新开的进程生效）："
LangString MSG_PATH_ADDED       ${LANG_ENGLISH}     "Added to PATH (effective for newly started processes):"
LangString MSG_PATH_ALREADY     ${LANG_SIMPCHINESE} "PATH 里已经有这个目录，未做改动："
LangString MSG_PATH_ALREADY     ${LANG_ENGLISH}     "Already present in PATH; left unchanged:"
LangString MSG_PATH_REMOVED     ${LANG_SIMPCHINESE} "已从 PATH 移除："
LangString MSG_PATH_REMOVED     ${LANG_ENGLISH}     "Removed from PATH:"
LangString MSG_PATH_REMOVED_ALL ${LANG_SIMPCHINESE} "PATH 里只剩我们这一条，该值已删除："
LangString MSG_PATH_REMOVED_ALL ${LANG_ENGLISH}     "PATH contained only this folder; the value was removed:"
LangString MSG_PATH_WRITE_FAILED ${LANG_SIMPCHINESE} "**写 PATH 失败**（可能没有权限）。请手工把下面这个目录加进 PATH："
LangString MSG_PATH_WRITE_FAILED ${LANG_ENGLISH}    "Writing PATH failed (insufficient rights?). Add this folder to PATH manually:"
LangString MSG_CORE_FAILED      ${LANG_SIMPCHINESE} "**程序文件写入失败**，安装已中止。请确认下面的目录存在、可写，并且没有被安全软件拦截："
LangString MSG_CORE_FAILED      ${LANG_ENGLISH}     "Writing the program files failed; the installation was aborted. Check that this folder exists, is writable and is not blocked by security software:"

LangString MSG_STATE_KEPT       ${LANG_SIMPCHINESE} "注意：检测到 Msixpkg 的安装记录。卸载**不会**删除它们 —— 删掉就没法再回收发布者证书了。要清理请先用 msixpkg-native uninstall / clean。记录位置："
LangString MSG_STATE_KEPT       ${LANG_ENGLISH}     "Note: Msixpkg installation records were found. They are kept on purpose: without them the publisher certificate can never be reclaimed. Use msixpkg-native uninstall / clean first if you want them gone. Records:"
LangString MSG_UNINST_ADMIN     ${LANG_SIMPCHINESE} "这是「为所有用户」安装，卸载需要管理员权限。正在请求提权…"
LangString MSG_UNINST_ADMIN     ${LANG_ENGLISH}     "This is a per-machine installation; uninstalling requires administrator rights. Requesting elevation..."
LangString MSG_UNINST_NO_ADMIN  ${LANG_SIMPCHINESE} "提权被拒绝，卸载已取消。请右键「卸载 Msixpkg Installer」→「以管理员身份运行」。"
LangString MSG_UNINST_NO_ADMIN  ${LANG_ENGLISH}     "Elevation was declined; uninstall cancelled. Run the uninstaller as administrator."
LangString MSG_UNINST_KEEP_FILES ${LANG_SIMPCHINESE} "有文件正在被使用，无法删除。请关闭所有 Msixpkg 安装器窗口后重试，或重启机器。"
LangString MSG_UNINST_KEEP_FILES ${LANG_ENGLISH}    "Some files are in use and could not be deleted. Close all Msixpkg Installer windows and retry, or reboot."

LangString ARP_PUBLISHER        ${LANG_SIMPCHINESE} "MsixpkgInstaller"
LangString ARP_PUBLISHER        ${LANG_ENGLISH}     "MsixpkgInstaller"
LangString ARP_COMMENTS         ${LANG_SIMPCHINESE} "Msixpkg 安装器本体（.msixpkg 的安装器）。已安装的应用在「设置」里另有 <应用名>（Msixpkg）条目。"
LangString ARP_COMMENTS         ${LANG_ENGLISH}     "The Msixpkg installer itself. Installed apps show up separately as <app name> (Msixpkg) in Apps & features."

LangString ASSOC_TYPE_NAME      ${LANG_SIMPCHINESE} "Msixpkg 包"
LangString ASSOC_TYPE_NAME      ${LANG_ENGLISH}     "Msixpkg package"
LangString CAPS_DESC            ${LANG_SIMPCHINESE} "打开 .msixpkg 包并安装其中的应用"
LangString CAPS_DESC            ${LANG_ENGLISH}     "Open a .msixpkg package and install the app inside"

LangString SM_LINK_MAIN         ${LANG_SIMPCHINESE} "Msixpkg 安装器"
LangString SM_LINK_MAIN         ${LANG_ENGLISH}     "Msixpkg Installer"
LangString SM_DESC_MAIN         ${LANG_SIMPCHINESE} "打开 .msixpkg 并安装其中的应用"
LangString SM_DESC_MAIN         ${LANG_ENGLISH}     "Open a .msixpkg package and install the app inside"
LangString SM_LINK_MANAGE       ${LANG_SIMPCHINESE} "管理已安装的应用"
LangString SM_LINK_MANAGE       ${LANG_ENGLISH}     "Manage installed apps"
LangString SM_DESC_MANAGE       ${LANG_SIMPCHINESE} "查看本安装器装过什么、卸载、修复建议"
LangString SM_DESC_MANAGE       ${LANG_ENGLISH}     "Review what this installer installed, uninstall, repair advice"
LangString SM_LINK_UNINST       ${LANG_SIMPCHINESE} "卸载 Msixpkg 安装器"
LangString SM_LINK_UNINST       ${LANG_ENGLISH}     "Uninstall Msixpkg Installer"
LangString SM_DESC_UNINST       ${LANG_ENGLISH}     "Remove the Msixpkg installer from this computer"
LangString SM_DESC_UNINST       ${LANG_SIMPCHINESE} "从这台机器上移除 Msixpkg 安装器"
LangString DESKTOP_LINK         ${LANG_SIMPCHINESE} "Msixpkg 安装器"
LangString DESKTOP_LINK         ${LANG_ENGLISH}     "Msixpkg Installer"

; ---------------------------------------------------------------------------
; .onInit：平台门禁 → 开关 → 提权状态 → 安装模式 → 旧安装 → 组件默认值
; ---------------------------------------------------------------------------
Function .onInit
  SetRegView 64

  ${If} ${Silent}
    StrCpy $SilentMode "1"
  ${Else}
    StrCpy $SilentMode "0"
  ${EndIf}

  ; ---- 平台门禁：x64 + Windows 10+ ----
  ${IfNot} ${RunningX64}
    MessageBox MB_OK|MB_ICONSTOP "$(MSG_NOT_X64)" /SD IDOK
    SetErrorLevel 6
    Abort
  ${EndIf}
  ${IfNot} ${AtLeastWin10}
    MessageBox MB_OK|MB_ICONSTOP "$(MSG_OLD_WINDOWS)" /SD IDOK
    SetErrorLevel 6
    Abort
  ${EndIf}

  ; ---- 开关 ----
  ${GetParameters} $R0
  StrCpy $NoAssoc "0"
  StrCpy $NoPath "0"
  StrCpy $NoDesktop "0"
  StrCpy $ForceAllUsers "0"
  StrCpy $ForceCurrentUser "0"

  ${StrStr} $1 "$R0" "/NOASSOC"
  ${If} $1 != ""
    StrCpy $NoAssoc "1"
  ${EndIf}
  ${StrStr} $1 "$R0" "/NOPATH"
  ${If} $1 != ""
    StrCpy $NoPath "1"
  ${EndIf}
  ${StrStr} $1 "$R0" "/NODESKTOP"
  ${If} $1 != ""
    StrCpy $NoDesktop "1"
  ${EndIf}
  ${StrStr} $1 "$R0" "/ALLUSERS"
  ${If} $1 != ""
    StrCpy $ForceAllUsers "1"
  ${EndIf}
  ${StrStr} $1 "$R0" "/CURRENTUSER"
  ${If} $1 != ""
    StrCpy $ForceCurrentUser "1"
  ${EndIf}
  ; /D= 是 NSIS 的内建开关：它在进入本函数**之前**就把值写进了 $INSTDIR，而且会把自己
  ; 从 $CMDLINE 里删掉 —— 所以在 $R0 里找不到它，只能靠「$INSTDIR 是否还是 InstallDir
  ; 的默认值」来判断用户有没有显式指定目录。下面任何"默认目录"逻辑都不许覆盖它
  ; （第一版没有这一步，结果 /D= 被默认值覆盖，静默安装静默地装到了别处）。
  ${If} $INSTDIR == "$PROGRAMFILES64\${PRODUCT_NAME}"
    StrCpy $DirOverridden "0"
  ${Else}
    StrCpy $DirOverridden "1"
  ${EndIf}

  ; ---- 是否已提权 ----
  Call IsAdminUser
  Pop $IsAdmin

  ; ---- 已有安装（HKLM = 机器级 / HKCU = 用户级）----
  ReadRegStr $ExistingMachineDir HKLM "${CFG_KEY}" "InstallDir"
  ReadRegStr $ExistingUserDir HKCU "${CFG_KEY}" "InstallDir"

  ; ---- 定安装模式 ----
  ${If} $ForceCurrentUser == "1"
    StrCpy $InstallMode "currentuser"
  ${ElseIf} $ForceAllUsers == "1"
    StrCpy $InstallMode "allusers"
  ${ElseIf} $ExistingMachineDir != ""
    ; 机器上已经有一份机器级安装：默认跟着它走（更新/修复），但必须有管理员权限
    StrCpy $InstallMode "allusers"
  ${ElseIf} $ExistingUserDir != ""
    StrCpy $InstallMode "currentuser"
  ${ElseIf} $IsAdmin == "1"
    StrCpy $InstallMode "allusers"
  ${Else}
    StrCpy $InstallMode "currentuser"
  ${EndIf}

  ${If} $InstallMode == "allusers"
  ${AndIf} $IsAdmin != "1"
    ${If} $ExistingMachineDir != ""
      StrCpy $R4 "$(MSG_MACHINE_EXISTS)$\r$\n$ExistingMachineDir"
      MessageBox MB_OK|MB_ICONSTOP "$R4" /SD IDOK
    ${Else}
      MessageBox MB_OK|MB_ICONSTOP "$(MSG_ALLUSERS_ADMIN)" /SD IDOK
    ${EndIf}
    SetErrorLevel 5
    Abort
  ${EndIf}

  ${If} $InstallMode == "allusers"
    SetShellVarContext all
  ${Else}
    SetShellVarContext current
  ${EndIf}

  ; ---- 安装目录：优先级 /D= > 已有安装 > 按模式的默认值 ----
  ${If} $InstallMode == "allusers"
    ReadRegStr $ExistingVersion HKLM "${CFG_KEY}" "Version"
  ${Else}
    ReadRegStr $ExistingVersion HKCU "${CFG_KEY}" "Version"
  ${EndIf}

  ${If} $DirOverridden == "1"
    ; 用户显式指定了目录（/D=），一个字都不改
  ${ElseIf} $InstallMode == "allusers"
    ${If} $ExistingMachineDir != ""
      StrCpy $INSTDIR "$ExistingMachineDir"
    ${EndIf}
  ${Else}
    ${If} $ExistingUserDir != ""
      StrCpy $INSTDIR "$ExistingUserDir"
    ${Else}
      StrCpy $INSTDIR "$LOCALAPPDATA\Programs\${PRODUCT_NAME}"
    ${EndIf}
  ${EndIf}

  !insertmacro LOG "---- setup start: mode=$InstallMode admin=$IsAdmin dir=$INSTDIR silent=$SilentMode"

  ; ---- 被占用的目标（向导正在跑）----
  StrCpy $RunningWizard "0"
  ${If} ${FileExists} "$INSTDIR\msixpkg-installer.exe"
    ClearErrors
    FileOpen $0 "$INSTDIR\msixpkg-installer.exe" a
    ${If} ${Errors}
      StrCpy $RunningWizard "1"
    ${Else}
      FileClose $0
    ${EndIf}
  ${EndIf}
  ${If} $RunningWizard == "1"
    MessageBox MB_OK|MB_ICONSTOP "$(MSG_CLOSE_WIZARD)" /SD IDOK
    !insertmacro LOG "abort: msixpkg-installer.exe is locked (wizard running?)"
    SetErrorLevel 7
    Abort
  ${EndIf}

  ; ---- 组件默认值（静默模式下这就是最终选择）----
  ; 放在单独的函数里：NSIS 是自上而下编译的，组件标识符 ${SEC_*} 要到 Section
  ; 指令被处理时才存在，所以这里只能 Call 一个在后面定义的函数。
  Call ApplyComponentSwitches
FunctionEnd

; ---------------------------------------------------------------------------
; 卸载 .onInit：读回安装记录 → 需要时提权重入
; ---------------------------------------------------------------------------
Function un.onInit
  SetRegView 64

  ReadRegStr $InstallMode HKLM "${CFG_KEY}" "InstallMode"
  ${If} $InstallMode == ""
    ReadRegStr $InstallMode HKCU "${CFG_KEY}" "InstallMode"
  ${EndIf}
  ${If} $InstallMode == ""
    ReadRegStr $InstallMode HKLM "${CFG_KEY}" "InstallDir"
    ${If} $InstallMode != ""
      StrCpy $InstallMode "allusers"
    ${Else}
      StrCpy $InstallMode "currentuser"
    ${EndIf}
  ${EndIf}

  ${If} $InstallMode == "allusers"
    SetShellVarContext all
  ${Else}
    SetShellVarContext current
  ${EndIf}

  ; 机器级安装的卸载必须提权：本进程是 asInvoker（安装器的安全属性），
  ; 所以用 runas 重新拉起**安装目录里的**卸载器（不是自己的临时副本），
  ; 由它在提权后正常走完流程。带 /ELEVATED 防止无限重入。
  ${If} $InstallMode == "allusers"
    Call un.IsAdminUser
    Pop $0
    ${If} $0 != "1"
      ${GetParameters} $R1
      ${UnStrStr} $R2 "$R1" "/ELEVATED"
      ${If} $R2 == ""
        ${If} ${Silent}
          StrCpy $R3 "/S"
        ${Else}
          StrCpy $R3 ""
        ${EndIf}
        ClearErrors
        ExecShellWait "runas" "$INSTDIR\Uninstall.exe" "$R3 /ELEVATED"
        ${If} ${Errors}
          MessageBox MB_OK|MB_ICONSTOP "$(MSG_UNINST_NO_ADMIN)" /SD IDOK
          SetErrorLevel 5
        ${EndIf}
        Quit
      ${Else}
        ; 已经提权重入过一次还是没权限：不再兜圈
        MessageBox MB_OK|MB_ICONSTOP "$(MSG_UNINST_NO_ADMIN)" /SD IDOK
        SetErrorLevel 5
        Quit
      ${EndIf}
    ${EndIf}
  ${EndIf}
FunctionEnd

; ---------------------------------------------------------------------------
; 组件
; ---------------------------------------------------------------------------
Section "$(SEC_CORE)" SEC_CORE
  SectionIn RO

  SetOutPath "$INSTDIR"
  File "/oname=msixpkg-installer.exe" "${APP_GUI}"
  File "/oname=msixpkg-native.exe" "${APP_CLI}"
  WriteUninstaller "$INSTDIR\Uninstall.exe"

  ; 复制完之后**必须**回头确认文件真的在：SetOutPath/File 失败时 NSIS 不会让安装失败，
  ; 于是一个什么都没装的安装会以「成功」退出。宁可报错，也不要出一个装了个寂寞的 0。
  ; （本机受限执行环境里就复现过：目标目录在沙箱外 → File 静默失败、退出码仍是 0。）
  IfFileExists "$INSTDIR\msixpkg-installer.exe" 0 core_write_failed
  IfFileExists "$INSTDIR\msixpkg-native.exe" 0 core_write_failed
  IfFileExists "$INSTDIR\Uninstall.exe" 0 core_write_failed
  !insertmacro LOG "core: files installed to $INSTDIR"
  Goto core_files_ok

  core_write_failed:
    !insertmacro LOG "core: FAILED to write the program files into $INSTDIR"
    ${IfNot} ${Silent}
      StrCpy $R4 "$(MSG_CORE_FAILED)$\r$\n$INSTDIR"
      MessageBox MB_OK|MB_ICONSTOP "$R4"
    ${EndIf}
    SetErrorLevel 4
    Abort

  core_files_ok:

  ; ---- 开始菜单（目录名语言中立；.lnk 名字跟随安装期语言，路径记进注册表）----
  CreateDirectory "$SMPROGRAMS\${SM_FOLDER}"
  CreateShortcut "$SMPROGRAMS\${SM_FOLDER}\$(SM_LINK_MAIN).lnk"   "$INSTDIR\msixpkg-installer.exe" ""         "$INSTDIR\msixpkg-installer.exe" 0 SW_SHOWNORMAL "" "$(SM_DESC_MAIN)"
  CreateShortcut "$SMPROGRAMS\${SM_FOLDER}\$(SM_LINK_MANAGE).lnk" "$INSTDIR\msixpkg-installer.exe" "--manage" "$INSTDIR\msixpkg-installer.exe" 0 SW_SHOWNORMAL "" "$(SM_DESC_MANAGE)"
  CreateShortcut "$SMPROGRAMS\${SM_FOLDER}\$(SM_LINK_UNINST).lnk" "$INSTDIR\Uninstall.exe"         ""         "$INSTDIR\msixpkg-installer.exe" 0 SW_SHOWNORMAL "" "$(SM_DESC_UNINST)"

  ; ---- 安装记录（卸载按记录精确清理，与语言无关）----
  ClearErrors
  !insertmacro CFG_WRITE_STR "InstallDir" "$INSTDIR"
  !insertmacro CFG_WRITE_STR "InstallMode" "$InstallMode"
  !insertmacro CFG_WRITE_STR "Version" "${PRODUCT_VERSION}"
  !insertmacro CFG_WRITE_STR "StartMenuFolder" "$SMPROGRAMS\${SM_FOLDER}"
  !insertmacro CFG_WRITE_STR "StartMenuLinks" "$SMPROGRAMS\${SM_FOLDER}\$(SM_LINK_MAIN).lnk;$SMPROGRAMS\${SM_FOLDER}\$(SM_LINK_MANAGE).lnk;$SMPROGRAMS\${SM_FOLDER}\$(SM_LINK_UNINST).lnk"
  ${If} ${Errors}
    !insertmacro LOG "core: WARNING - install records could not be written to the registry"
  ${EndIf}

  ; ---- ARP：「设置 → 应用」里的卸载入口 ----
  ClearErrors
  ${If} $InstallMode == "allusers"
    WriteRegStr HKLM "${UNINST_KEY}" "DisplayName"     "$(PRODUCT_NAME_UI) ${PRODUCT_VERSION}"
    WriteRegStr HKLM "${UNINST_KEY}" "DisplayVersion"  "${PRODUCT_VERSION}"
    WriteRegStr HKLM "${UNINST_KEY}" "Publisher"       "$(ARP_PUBLISHER)"
    WriteRegStr HKLM "${UNINST_KEY}" "DisplayIcon"     "$INSTDIR\msixpkg-installer.exe,0"
    WriteRegStr HKLM "${UNINST_KEY}" "UninstallString" '"$INSTDIR\Uninstall.exe"'
    WriteRegStr HKLM "${UNINST_KEY}" "QuietUninstallString" '"$INSTDIR\Uninstall.exe" /S'
    WriteRegStr HKLM "${UNINST_KEY}" "InstallLocation" "$INSTDIR"
    WriteRegStr HKLM "${UNINST_KEY}" "Comments"        "$(ARP_COMMENTS)"
    WriteRegDWORD HKLM "${UNINST_KEY}" "NoModify" 1
    WriteRegDWORD HKLM "${UNINST_KEY}" "NoRepair" 1
  ${Else}
    WriteRegStr HKCU "${UNINST_KEY}" "DisplayName"     "$(PRODUCT_NAME_UI) ${PRODUCT_VERSION}"
    WriteRegStr HKCU "${UNINST_KEY}" "DisplayVersion"  "${PRODUCT_VERSION}"
    WriteRegStr HKCU "${UNINST_KEY}" "Publisher"       "$(ARP_PUBLISHER)"
    WriteRegStr HKCU "${UNINST_KEY}" "DisplayIcon"     "$INSTDIR\msixpkg-installer.exe,0"
    WriteRegStr HKCU "${UNINST_KEY}" "UninstallString" '"$INSTDIR\Uninstall.exe"'
    WriteRegStr HKCU "${UNINST_KEY}" "QuietUninstallString" '"$INSTDIR\Uninstall.exe" /S'
    WriteRegStr HKCU "${UNINST_KEY}" "InstallLocation" "$INSTDIR"
    WriteRegStr HKCU "${UNINST_KEY}" "Comments"        "$(ARP_COMMENTS)"
    WriteRegDWORD HKCU "${UNINST_KEY}" "NoModify" 1
    WriteRegDWORD HKCU "${UNINST_KEY}" "NoRepair" 1
  ${EndIf}

  ; EstimatedSize：ARP 里显示的占用（KB）。载荷大小由构建脚本按实际产物算好传进来，
  ; 安装器运行时不再遍历目录（核心 NSIS 没有 GetFileSize，用 FileFunc 的 GetSize 会
  ; 多一次全目录遍历，得不偿失）。
!if ${ESTIMATED_SIZE_KB} > 0
  !insertmacro CFG_WRITE_DWORD "EstimatedSize" ${ESTIMATED_SIZE_KB}
!endif
  ${If} ${Errors}
    !insertmacro LOG "core: WARNING - the Apps & features uninstall entry could not be written"
  ${EndIf}

  !insertmacro LOG "core: shortcuts + uninstall entry written (mode=$InstallMode)"
SectionEnd

Section "$(SEC_ASSOC)" SEC_ASSOC
  ClearErrors
  ${If} $InstallMode == "allusers"
    !insertmacro ASSOC_WRITE HKLM
  ${Else}
    !insertmacro ASSOC_WRITE HKCU
  ${EndIf}
  ${If} ${Errors}
    ; 关联写不进去不算致命（程序本身已经装好了），但必须留下痕迹，
    ; 不能让用户以为双击 .msixpkg 一定能用。
    !insertmacro LOG "assoc: FAILED to register ${EXT} -> ${PROGID}"
  ${Else}
    !insertmacro CFG_WRITE_STR "AssocRegistered" "1"

    ; 让资源管理器立刻刷新图标/关联（不广播的话要等它自己重新读）
    System::Call 'shell32::SHChangeNotify(i 0x08000000, i 0x0000, p 0, p 0)'
    !insertmacro LOG "assoc: ${EXT} -> ${PROGID}"
  ${EndIf}
SectionEnd

Section "$(SEC_PATH)" SEC_PATH
  Call AddInstallDirToPath
SectionEnd

Section /o "$(SEC_DESKTOP)" SEC_DESKTOP
  CreateShortcut "$DESKTOP\$(DESKTOP_LINK).lnk" "$INSTDIR\msixpkg-installer.exe" "" "$INSTDIR\msixpkg-installer.exe" 0 SW_SHOWNORMAL "" "$(SM_DESC_MAIN)"
  !insertmacro CFG_WRITE_STR "DesktopLink" "$DESKTOP\$(DESKTOP_LINK).lnk"
  !insertmacro LOG "desktop: shortcut created"
SectionEnd

; ---------------------------------------------------------------------------
; 组件默认值：/NOASSOC /NOPATH /NODESKTOP 关掉对应组件。
; GUI 模式下效果就是「这三个复选框预先不勾」；静默模式下这就是最终选择。
; 定义在 Section 之后，因为 ${SEC_*} 要到那时才存在（见 .onInit 里的说明）。
; ---------------------------------------------------------------------------
Function ApplyComponentSwitches
  ${If} $NoAssoc == "1"
    SectionSetFlags ${SEC_ASSOC} 0
  ${EndIf}
  ${If} $NoPath == "1"
    SectionSetFlags ${SEC_PATH} 0
  ${EndIf}
  ${If} $NoDesktop == "1"
    SectionSetFlags ${SEC_DESKTOP} 0
  ${EndIf}
FunctionEnd

; ---------------------------------------------------------------------------
; 组件说明（必须在 Section 之后：这里要用到 ${SEC_*}）
; ---------------------------------------------------------------------------
!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
  !insertmacro MUI_DESCRIPTION_TEXT ${SEC_CORE}    "$(SEC_CORE_DESC)"
  !insertmacro MUI_DESCRIPTION_TEXT ${SEC_ASSOC}   "$(SEC_ASSOC_DESC)"
  !insertmacro MUI_DESCRIPTION_TEXT ${SEC_PATH}    "$(SEC_PATH_DESC)"
  !insertmacro MUI_DESCRIPTION_TEXT ${SEC_DESKTOP} "$(SEC_DESKTOP_DESC)"
!insertmacro MUI_FUNCTION_DESCRIPTION_END

; ===========================================================================
; 卸载
; ===========================================================================
Section "Uninstall"
  SetRegView 64
  !insertmacro UNLOG "---- uninstall start: mode=$InstallMode dir=$INSTDIR"

  ; ---- 安装记录（状态库）在不在：只提示，绝不删 ----
  StrCpy $StateRecordsFound ""
  ReadEnvStr $0 MSIXPKG_DATA_ROOT
  ${If} $0 != ""
  ${AndIf} ${FileExists} "$0\state\*.json"
    StrCpy $StateRecordsFound "$0\state"
  ${EndIf}
  ${If} $StateRecordsFound == ""
  ${AndIf} ${FileExists} "$APPDATA\Msixpkg\state\*.json"
    StrCpy $StateRecordsFound "$APPDATA\Msixpkg\state"
  ${EndIf}
  ${If} $StateRecordsFound == ""
  ${AndIf} ${FileExists} "$LOCALAPPDATA\Msixpkg\state\*.json"
    StrCpy $StateRecordsFound "$LOCALAPPDATA\Msixpkg\state"
  ${EndIf}
  ${If} $StateRecordsFound != ""
    !insertmacro UNLOG "state: records kept at $StateRecordsFound"
    ${IfNot} ${Silent}
      StrCpy $R4 "$(MSG_STATE_KEPT)$\r$\n$StateRecordsFound"
      MessageBox MB_OK|MB_ICONINFORMATION "$R4"
    ${EndIf}
  ${EndIf}

  ; ---- 快捷方式（按记录删，语言无关）----
  ReadRegStr $0 HKCU "${CFG_KEY}" "DesktopLink"
  ${If} $0 == ""
    ReadRegStr $0 HKLM "${CFG_KEY}" "DesktopLink"
  ${EndIf}
  ${If} $0 != ""
    Delete "$0"
  ${EndIf}

  ReadRegStr $0 HKCU "${CFG_KEY}" "StartMenuFolder"
  ${If} $0 == ""
    ReadRegStr $0 HKLM "${CFG_KEY}" "StartMenuFolder"
  ${EndIf}
  ${If} $0 != ""
    RMDir /r "$0"
  ${Else}
    RMDir /r "$SMPROGRAMS\${SM_FOLDER}"
  ${EndIf}

  ; ---- 文件关联 ----
  ${If} $InstallMode == "allusers"
    !insertmacro ASSOC_REMOVE HKLM
  ${Else}
    !insertmacro ASSOC_REMOVE HKCU
  ${EndIf}
  ; UserChoice 指向我们时才清（否则会把用户的选择一并抹掉）
  ReadRegStr $0 HKCU "Software\Microsoft\Windows\CurrentVersion\Explorer\FileExts\${EXT}\UserChoice" "ProgId"
  ${If} $0 == "${PROGID}"
    DeleteRegKey HKCU "Software\Microsoft\Windows\CurrentVersion\Explorer\FileExts\${EXT}\UserChoice"
  ${EndIf}
  System::Call 'shell32::SHChangeNotify(i 0x08000000, i 0x0000, p 0, p 0)'

  ; ---- PATH ----
  Call un.RemoveInstallDirFromPath

  ; ---- 程序文件 ----
  ; 先把工作目录挪出 $INSTDIR：只要还有进程以它为当前目录，RMDir 就会以
  ; "目录正在被使用"失败，最后留下一个空壳目录。
  SetOutPath "$TEMP"
  Delete "$INSTDIR\msixpkg-installer.exe"
  Delete "$INSTDIR\msixpkg-native.exe"
  ; 卸载器原件：从 ARP / 开始菜单调用时，真正在跑的是它在 %TEMP% 里的副本，
  ; 所以这里能删掉原件；用 `_?=` 调用时原件正在运行，这次删除会静默失败 ——
  ; 那种情况下由调用方（安装器 / 脚本）负责收尾，NSIS 的语义本就如此。
  Delete "$INSTDIR\Uninstall.exe"

  ; ---- 注册表 ----
  ${If} $InstallMode == "allusers"
    DeleteRegKey HKLM "${CFG_KEY}"
    DeleteRegKey HKLM "${UNINST_KEY}"
  ${Else}
    DeleteRegKey HKCU "${CFG_KEY}"
    DeleteRegKey HKCU "${UNINST_KEY}"
  ${EndIf}
  ; 另一侧可能留有残余（例如先装用户级、后又装了机器级）：一并清掉我们自己的键
  DeleteRegKey HKCU "${CFG_KEY}"
  DeleteRegKey HKCU "${UNINST_KEY}"

  RMDir "$INSTDIR"

  ; 删完要确认：文件被占用时 Delete 会静默失败，不能让用户以为卸干净了
  ${If} ${FileExists} "$INSTDIR\msixpkg-installer.exe"
    !insertmacro UNLOG "uninstall: program files could not be deleted (in use?)"
    SetErrorLevel 4
    ${IfNot} ${Silent}
      MessageBox MB_OK|MB_ICONEXCLAMATION "$(MSG_UNINST_KEEP_FILES)"
    ${EndIf}
  ${EndIf}

  !insertmacro UNLOG "---- uninstall done"
SectionEnd
