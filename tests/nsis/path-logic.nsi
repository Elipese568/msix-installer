; ============================================================================
; PATH 组件的单元测试（NSIS 层面，不需要注册表、不需要管理员）
; ============================================================================
;
; 直接调用 installer/nsis/msixpkg-path.nsh 里的 PathScan 宏（也就是安装器真正用的那份
; 代码，不是复制品），对本文件末尾列出的每个用例断言两件事：
;   1. 扫描结果：$PathHit（是否已有我们这一条）与 $PathRebuilt（去掉我们之后的 PATH）；
;   2. 往返结果：模拟「安装时追加 → 卸载时移除」之后 PATH 的最终值。
;
; 为什么要这么测：这段逻辑在受限执行环境里没法通过「真的装一次」验证（子进程写不了
; HKCU），而它一旦改坏就会毁掉用户整条 PATH。用例覆盖空串、空段（首/中/尾）、重复条目、
; 前缀陷阱（C:\a 不能匹配 C:\ab）、大小写、带引号的条目。
;
; 编译（必须带 /INPUTCHARSET UTF8）：
;   makensis /INPUTCHARSET UTF8 tests\nsis\path-logic.nsi
; 运行（静默安装器会自己跑完；报告写在 exe 旁边）：
;   path-logic.exe /S        → 退出码 = 失败用例数
;
; 这个文件**不按 /WX 编译**：它刻意只用到 msixpkg-path.nsh 的一部分，NSIS 会对没用到的
; 变量与 un.* 包装函数报 6001/6010 —— 那是预期的。产品脚本 MsixpkgInstaller.nsi 全程 /WX。
;
; 注意：本文件是纯 ASCII，避免 PowerShell / makensis 的编码歧义。

Unicode true
Name "msixpkg path logic test"
OutFile "path-logic.exe"
RequestExecutionLevel user
SilentInstall silent
ShowInstDetails nevershow

!include "LogicLib.nsh"

; msixpkg-path.nsh 的读写宏按 $InstallMode 选配置单元，所以调用方要先声明它
;（安装器脚本里本来就有；这里为了单独编译测试再声明一份）。
Var InstallMode

!include "${__FILEDIR__}\..\..\installer\nsis\msixpkg-path.nsh"

Var ReportHandle
Var CaseName
Var CaseExpectedHit
Var CaseExpectedRebuilt
Var CaseExpectedFinal
Var SimInstalled
Var SimFinal
Var Failures
Var Reason

!macro Report Text
  FileWrite $ReportHandle "${Text}$\r$\n"
!macroend

; 单个用例：$PathValue / $PathEntry 由参数给出，断言扫描与往返结果。
!macro CASE Name Value Entry ExpectedHit ExpectedRebuilt ExpectedFinal
  StrCpy $CaseName "${Name}"
  StrCpy $PathValue "${Value}"
  StrCpy $PathEntry "${Entry}"
  StrCpy $CaseExpectedHit "${ExpectedHit}"
  StrCpy $CaseExpectedRebuilt "${ExpectedRebuilt}"
  StrCpy $CaseExpectedFinal "${ExpectedFinal}"
  StrCpy $Reason ""

  ; ---- 1) 扫描 ----
  !insertmacro PathScan
  ${If} $PathHit != $CaseExpectedHit
    StrCpy $Reason "hit: expected=[$CaseExpectedHit] actual=[$PathHit]"
  ${ElseIf} $PathRebuilt != $CaseExpectedRebuilt
    StrCpy $Reason "rebuilt: expected=[$CaseExpectedRebuilt] actual=[$PathRebuilt]"
  ${EndIf}

  ; ---- 2) 安装时的追加规则（与 AddInstallDirToPath 一致）----
  ${If} $PathHit == "1"
    StrCpy $SimInstalled "$PathValue"          ; 已经在 PATH 里 -> 一个字都不改
  ${ElseIf} $PathRebuilt == ""
    StrCpy $SimInstalled "$PathEntry"
  ${Else}
    StrCpy $SimInstalled "$PathRebuilt;$PathEntry"
  ${EndIf}

  ; ---- 3) 卸载时的移除规则（与 un.RemoveInstallDirFromPath 一致）----
  StrCpy $PathValue "$SimInstalled"
  !insertmacro PathScan
  ${If} $PathHit != "1"
    StrCpy $SimFinal "$SimInstalled"
  ${Else}
    StrCpy $SimFinal "$PathRebuilt"
  ${EndIf}

  ${If} $Reason == ""
  ${AndIf} $SimFinal != $CaseExpectedFinal
    StrCpy $Reason "round trip: expected=[$CaseExpectedFinal] actual=[$SimFinal]"
  ${EndIf}

  ${If} $Reason == ""
    !insertmacro Report "PASS ${Name}"
  ${Else}
    IntOp $Failures $Failures + 1
    !insertmacro Report "FAIL ${Name}: $Reason (value=[${Value}] entry=[${Entry}])"
  ${EndIf}
!macroend

Section
  StrCpy $Failures "0"
  StrCpy $InstallMode "currentuser"
  FileOpen $ReportHandle "$EXEDIR\path-logic-report.txt" w

  !insertmacro Report "msixpkg PATH logic test ($EXEDIR)"
  !insertmacro Report ""

  ; 名字             PATH 原值              条目        命中 去重后的值            往返后的最终值
  !insertmacro CASE "empty"            ""                     "C:\e"      "0"  ""                            ""
  !insertmacro CASE "only-ours"        "C:\e"                 "C:\e"      "1"  ""                            ""
  !insertmacro CASE "ours-last"        "C:\a;C:\e"            "C:\e"      "1"  "C:\a"                        "C:\a"
  !insertmacro CASE "ours-first"       "C:\e;C:\a"            "C:\e"      "1"  "C:\a"                        "C:\a"
  !insertmacro CASE "ours-middle"      "C:\a;C:\e;C:\b"       "C:\e"      "1"  "C:\a;C:\b"                   "C:\a;C:\b"
  !insertmacro CASE "absent"           "C:\a;C:\b"            "C:\e"      "0"  "C:\a;C:\b"                   "C:\a;C:\b"
  !insertmacro CASE "leading-empty"    ";C:\a"                "C:\e"      "0"  ";C:\a"                       ";C:\a"
  !insertmacro CASE "middle-empty"     "C:\a;;C:\b"           "C:\e"      "0"  "C:\a;;C:\b"                  "C:\a;;C:\b"
  !insertmacro CASE "trailing-empty"   "C:\a;"                "C:\e"      "0"  "C:\a;"                       "C:\a;"
  !insertmacro CASE "prefix-trap"      "C:\ab;C:\a"           "C:\a"      "1"  "C:\ab"                       "C:\ab"
  !insertmacro CASE "case-insensitive" "c:\E"                 "C:\e"      "1"  ""                            ""
  !insertmacro CASE "duplicate-ours"   "C:\a;C:\e;C:\e"       "C:\e"      "1"  "C:\a"                        "C:\a"
  !insertmacro CASE "quoted-entry"     "$\"C:\e$\""           "C:\e"      "0"  "$\"C:\e$\""                  "$\"C:\e$\""

  !insertmacro Report ""
  ${If} $Failures == "0"
    !insertmacro Report "cases 13, failures 0"
  ${Else}
    !insertmacro Report "cases 13, failures $Failures"
  ${EndIf}
  FileClose $ReportHandle

  SetErrorLevel $Failures
SectionEnd
