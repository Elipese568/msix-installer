; ============================================================================
; PATH 组件的实现（被 MsixpkgInstaller.nsi 与 tests/nsis/path-logic.nsi 共用）
; ============================================================================
;
; 为什么单独一个文件：PATH 是用户的资产，改错一次就可能让别人的命令行全线失效，
; 而这套逻辑在受限环境里没法通过「真的装一次」来验证（子进程写不了 HKCU）。
; 因此把它抽出来，配一个直接调用宏的 NSIS 单元测试（tests/nsis/path-logic.nsi），
; 把各种边界（空段、重复、首/中/尾位置、只有我们一条、值不存在）都钉住。
;
; 这里的两个不变量：
;   1. 只按 ';' 分段做**整段相等**比较，绝不做子串替换 —— 否则 C:\a 会误伤 C:\ab；
;   2. 写回时保留原来的 REG_SZ / REG_EXPAND_SZ 类型；长度越界就拒绝改动（见 READ_PATH_BODY）。
;
; 变量（installer 与 uninstaller 共用同一份声明，各自有独立的运行时存储）。
; 调用方还必须自己声明 `Var InstallMode`（"allusers" / "currentuser"）：
; 读写宏靠它决定动 HKLM 还是 HKCU。

Var PathValue          ; 读到的原值
Var PathType           ; 注册表类型（"1" = REG_SZ / "2" = REG_EXPAND_SZ）
Var PathExists         ; 值是否存在（"1"/"0"）
Var PathTooLong        ; "1" = 注册表报告的长度超过 NSIS 字符串上限，拒绝改动
Var PathEntry          ; 要匹配的条目（安装时是本安装目录）
Var PathNew            ; 待写入的新值
Var PathHit            ; PathScan 输出：命中
Var PathRebuilt        ; PathScan 输出：去掉命中段后的新值
Var PathFirst
Var PathRest
Var PathChunk
Var PathLen
Var PathPos
Var PathIndex
Var PathChar
Var PathNext
Var PathLast
Var WriteFailed        ; "1" = 最近一次写注册表失败

!ifndef HKEY_CURRENT_USER
  !define HKEY_CURRENT_USER  0x80000001
!endif
!ifndef HKEY_LOCAL_MACHINE
  !define HKEY_LOCAL_MACHINE 0x80000002
!endif
!ifndef KEY_QUERY_VALUE
  !define KEY_QUERY_VALUE    0x0001
!endif
!ifndef MACHINE_PATH_KEY
  !define MACHINE_PATH_KEY   "SYSTEM\CurrentControlSet\Control\Session Manager\Environment"
!endif
!ifndef USER_PATH_KEY
  !define USER_PATH_KEY      "Environment"
!endif

; ---------------------------------------------------------------------------
; 逐段扫描：输入 $PathValue / $PathEntry，输出 $PathHit（"1"/"0"）与 $PathRebuilt
; 逐字符找 ';' 而不是用字符串函数：NSIS 核心没有「按分隔符切分」。
; ---------------------------------------------------------------------------
!macro PathScan
  StrCpy $PathHit "0"
  StrCpy $PathRebuilt ""
  StrCpy $PathFirst "1"
  StrCpy $PathRest "$PathValue"

  ${Do}
    StrLen $PathLen "$PathRest"

    ; 找第一个 ';'（没有就认为整段都是最后一段）
    StrCpy $PathPos "$PathLen"
    StrCpy $PathIndex "0"
    ${Do}
      ${If} $PathIndex >= $PathLen
        ${Break}
      ${EndIf}
      StrCpy $PathChar "$PathRest" 1 "$PathIndex"
      ${If} $PathChar == ";"
        StrCpy $PathPos "$PathIndex"
        ${Break}
      ${EndIf}
      IntOp $PathIndex $PathIndex + 1
    ${Loop}

    ; StrCpy 的 maxlen 就是段长，$PathPos 为 0 时给空串（**实测**：
    ; `StrCpy $x "abcdef" 0` → ""；"0 = 整串复制"的说法只对 -1 那类写法成立）。
    ; 空段（首字符就是 ';'，或 C:\a;;C:\b 里中间那一段）因此天然正确 ——
    ; tests/nsis/path-logic.nsi 的 leading-empty / middle-empty 用例盯着这一点。
    StrCpy $PathChunk "$PathRest" "$PathPos"

    IntOp $PathNext $PathPos + 1
    ${If} $PathPos >= $PathLen
      StrCpy $PathRest ""
      StrCpy $PathLast "1"
    ${Else}
      StrCpy $PathRest "$PathRest" "" "$PathNext"
      StrCpy $PathLast "0"
    ${EndIf}

    ${If} $PathChunk == "$PathEntry"
      StrCpy $PathHit "1"
    ${ElseIf} $PathFirst == "1"
      StrCpy $PathRebuilt "$PathChunk"
      StrCpy $PathFirst "0"
    ${Else}
      StrCpy $PathRebuilt "$PathRebuilt;$PathChunk"
    ${EndIf}

    ${If} $PathLast == "1"
      ${Break}
    ${EndIf}
  ${Loop}
!macroend

; ---------------------------------------------------------------------------
; 读 PATH（含截断保护）
; ---------------------------------------------------------------------------
!macro READ_PATH_BODY
  StrCpy $PathTooLong "0"
  StrCpy $PathExists "0"
  StrCpy $PathType "2"          ; 值不存在时按 REG_EXPAND_SZ 新建（惯例）
  StrCpy $PathValue ""
  StrCpy $0 "1"

  ${If} $InstallMode == "allusers"
    System::Call 'advapi32::RegOpenKeyExW(p ${HKEY_LOCAL_MACHINE}, w "${MACHINE_PATH_KEY}", i 0, i ${KEY_QUERY_VALUE}, *p .r1)i.r0'
  ${Else}
    System::Call 'advapi32::RegOpenKeyExW(p ${HKEY_CURRENT_USER}, w "${USER_PATH_KEY}", i 0, i ${KEY_QUERY_VALUE}, *p .r1)i.r0'
  ${EndIf}

  ${If} $0 == 0
    System::Call 'advapi32::RegQueryValueExW(p r1, w "Path", p 0, *i .r2, p 0, *i .r3)i.r0'
    System::Call 'advapi32::RegCloseKey(p r1)'
    ${If} $0 == 0
      StrCpy $PathExists "1"
      StrCpy $PathType "$2"
      ; 截断保护：NSIS 字符串上限是 ${NSIS_MAX_STRLEN} 个字符；注册表报告的 $3 是
      ; 字节数（含结尾 NUL）。一旦接近上限，ReadRegStr 会静默截断 —— 那就干脆不动它。
      IntOp $4 ${NSIS_MAX_STRLEN} * 2
      IntOp $4 $4 - 128
      ${If} $3 > $4
        StrCpy $PathTooLong "1"
        Return
      ${EndIf}

      ${If} $InstallMode == "allusers"
        ReadRegStr $PathValue HKLM "${MACHINE_PATH_KEY}" "Path"
      ${Else}
        ReadRegStr $PathValue HKCU "${USER_PATH_KEY}" "Path"
      ${EndIf}
    ${EndIf}
  ${EndIf}
!macroend

; ---------------------------------------------------------------------------
; 写 PATH（保留原来的 REG_SZ / REG_EXPAND_SZ 类型）+ 通知已运行的进程
; ---------------------------------------------------------------------------
!macro WRITE_PATH_BODY
  StrCpy $WriteFailed "0"
  ClearErrors
  ${If} $InstallMode == "allusers"
    ${If} $PathType == "2"
      WriteRegExpandStr HKLM "${MACHINE_PATH_KEY}" "Path" "$PathNew"
    ${Else}
      WriteRegStr HKLM "${MACHINE_PATH_KEY}" "Path" "$PathNew"
    ${EndIf}
  ${Else}
    ${If} $PathType == "2"
      WriteRegExpandStr HKCU "${USER_PATH_KEY}" "Path" "$PathNew"
    ${Else}
      WriteRegStr HKCU "${USER_PATH_KEY}" "Path" "$PathNew"
    ${EndIf}
  ${EndIf}
  ; 写失败必须记下来：不要向上层报告"已加入 PATH"而实际什么都没发生
  ${If} ${Errors}
    StrCpy $WriteFailed "1"
  ${EndIf}

  ; WM_SETTINGCHANGE：让已经打开的「新进程」也拿到新 PATH（本进程不受影响）
  System::Call 'user32::SendMessageTimeoutW(p 0xffff, i 0x001A, p 0, w "Environment", i 0x0002, i 2000, *i .r0)'
!macroend

; ---------------------------------------------------------------------------
; 删除 PATH 值（原值只剩我们一条时才会走到这里）
; ---------------------------------------------------------------------------
!macro DELETE_PATH_BODY
  ${If} $InstallMode == "allusers"
    DeleteRegValue HKLM "${MACHINE_PATH_KEY}" "Path"
  ${Else}
    DeleteRegValue HKCU "${USER_PATH_KEY}" "Path"
  ${EndIf}
  System::Call 'user32::SendMessageTimeoutW(p 0xffff, i 0x001A, p 0, w "Environment", i 0x0002, i 2000, *i .r0)'
!macroend

Function ReadPathValue
  !insertmacro READ_PATH_BODY
FunctionEnd

Function un.ReadPathValue
  !insertmacro READ_PATH_BODY
FunctionEnd

Function WritePathValue
  !insertmacro WRITE_PATH_BODY
FunctionEnd

Function un.WritePathValue
  !insertmacro WRITE_PATH_BODY
FunctionEnd

Function un.DeletePathValue
  !insertmacro DELETE_PATH_BODY
FunctionEnd
