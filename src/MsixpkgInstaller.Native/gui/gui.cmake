# Win32 原生安装向导（docs/installer-design.md §8）。
#
# 这个文件由主 CMakeLists.txt 在末尾 include（见那里的 if(EXISTS ...) 钩子），
# 所以可以直接用 CMAKE_CURRENT_SOURCE_DIR —— 那时它仍然指向
# src/MsixpkgInstaller.Native。
#
# 目标名 msixpkg_native_gui / 产物 msixpkg-installer.exe：
#   - WIN32 子系统（入口 wWinMain，不弹控制台窗口）；
#   - 资源用 res/msixpkg.rc 编进去（图标 + zh-CN / en-US 两张字符串表）；
#   - app.manifest 提供 asInvoker + PerMonitorV2 + longPathAware + UTF-8 code page
#     + comctl32 v6；它同时决定「进程不自提权」这条安全属性，所以必须随 exe 走。

set(MSIXPKG_GUI_SOURCES
  gui/gui_wizard.cpp
  gui/gui_worker.cpp
  gui/gui_ui.cpp
  gui/gui_strings.cpp
)

add_executable(msixpkg_native_gui WIN32 ${MSIXPKG_GUI_SOURCES} res/msixpkg.rc)

# 引擎头在 src/ 下（installer.h / interface.h / verify.h …），
# 资源 ID 在 res/ 下（resource.h 由 gui_strings.h 包含）。
target_include_directories(msixpkg_native_gui PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
  "${CMAKE_CURRENT_SOURCE_DIR}/gui"
  "${CMAKE_CURRENT_SOURCE_DIR}/res"
)

target_link_libraries(msixpkg_native_gui PRIVATE
  msixpkg_core     # 编排 + 部署 + 状态库 + 日志；它的 PUBLIC 依赖带上了
                   # bcrypt / crypt32 / WindowsApp / shell32
  comctl32         # ProgressBar / ListView / v6 观感
  dwmapi           # 深色模式 / Mica / 圆角（靠返回值判定支持与否）
  advapi32         # 读 AppsUseLightTheme
  ole32            # 标准控件里少数路径会用；显式列出避免将来踩隐式链接
  user32 gdi32     # 窗口与绘制（默认已链接，写出来是为了自解释）
)

set_target_properties(msixpkg_native_gui PROPERTIES
  OUTPUT_NAME "msixpkg-installer"
  VS_DPI_AWARE "PerMonitor"
)

# 编译选项与其余目标逐字一致：/utf-8 让源码里的中文注释与字符串按 UTF-8 解码，
# /W4 + /permissive- 是新代码的门槛，:__cplusplus 让 <version> 报出真实的 C++ 版本。
target_compile_options(msixpkg_native_gui PRIVATE /utf-8 /W4 /permissive- /EHsc /Zc:__cplusplus)

# 清单随 exe 走：asInvoker 决定「主进程永不自提权」，PerMonitorV2 决定 DPI 行为，
# longPathAware 决定超长路径能不能用。用链接器选项而不是额外生成 .res，
# 是为了让这份 XML 同时是人可读的源文件（审计安全属性时直接看它）。
target_link_options(msixpkg_native_gui PRIVATE
  "/MANIFEST:EMBED"
  "/MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/app.manifest"
)

if(MSIXPKG_WARNINGS_AS_ERRORS)
  target_compile_options(msixpkg_native_gui PRIVATE /WX)
endif()

# 资源编译器也要知道代码页：res/*.rc 里的中文是 UTF-8 无 BOM 保存的，
# 不显式指定 rc.exe 会按系统 ANSI 代码页解码（中文变乱码）。
set_source_files_properties(res/msixpkg.rc PROPERTIES COMPILE_OPTIONS "/c65001")
