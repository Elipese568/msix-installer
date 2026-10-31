# 分发安装器（setup / Uninstall.exe）的目标定义。
#
# 与主 CMakeLists 分开：安装引擎、Wren 页面与资源都在这里演进，主工程只负责 include。
# 版本号仍从 res/msixpkg.rc 的 FILEVERSION 读，保持「单一事实来源」。

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/res/msixpkg.rc" MSIXPKG_RC_SOURCE)

# 版本号是在**配置期**从 rc 解析成编译定义的，所以必须把 rc 登记成配置依赖：
# 否则改完 FILEVERSION 再增量构建，CMake 不会重跑 configure，setup/Uninstall.exe
# 的 VERSIONINFO、ARP DisplayVersion 与安装记录会继续塞着上一个版本号。
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             "${CMAKE_CURRENT_SOURCE_DIR}/res/msixpkg.rc")

string(REGEX MATCH
  "FILEVERSION[ \t]+([0-9]+)[ \t]*,[ \t]*([0-9]+)[ \t]*,[ \t]*([0-9]+)[ \t]*,[ \t]*([0-9]+)"
  MSIXPKG_VERSION_MATCH "${MSIXPKG_RC_SOURCE}")
if(NOT MSIXPKG_VERSION_MATCH)
  message(FATAL_ERROR "找不到 res/msixpkg.rc 里的 FILEVERSION x, y, z, w")
endif()

set(MSIXPKG_SETUP_VERSION
    "${CMAKE_MATCH_1}.${CMAKE_MATCH_2}.${CMAKE_MATCH_3}")
set(MSIXPKG_SETUP_VERSION_QUAD
    "${CMAKE_MATCH_1}.${CMAKE_MATCH_2}.${CMAKE_MATCH_3}.${CMAKE_MATCH_4}")
set(MSIXPKG_SETUP_VERSION_COMMA
    "${CMAKE_MATCH_1},${CMAKE_MATCH_2},${CMAKE_MATCH_3},${CMAKE_MATCH_4}")
message(STATUS "Setup version: ${MSIXPKG_SETUP_VERSION} (${MSIXPKG_SETUP_VERSION_QUAD})")

set(MSIXPKG_SETUP_SOURCES
  setup/setup_util.cpp
  setup/setup_options.cpp
  setup/setup_sha256.cpp
  setup/setup_payload.cpp
  setup/setup_log.cpp
  setup/setup_registry.cpp
  setup/setup_path.cpp
  setup/setup_strings.cpp
  setup/setup_shortcuts.cpp
  setup/setup_engine.cpp
)

add_library(msixpkg_setup_core STATIC ${MSIXPKG_SETUP_SOURCES})
target_include_directories(msixpkg_setup_core
  PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/setup")
target_compile_features(msixpkg_setup_core PUBLIC cxx_std_20)
target_compile_options(msixpkg_setup_core PRIVATE
  /utf-8 /W4 /permissive- /EHsc /Zc:__cplusplus)
target_compile_definitions(msixpkg_setup_core PUBLIC
  UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
  MSIXPKG_SETUP_VERSION=L"${MSIXPKG_SETUP_VERSION}"
  MSIXPKG_SETUP_VERSION_QUAD=L"${MSIXPKG_SETUP_VERSION_QUAD}")
target_link_libraries(msixpkg_setup_core PUBLIC bcrypt shell32 ole32 shlwapi advapi32 user32)

if(MSIXPKG_WARNINGS_AS_ERRORS)
  target_compile_options(msixpkg_setup_core PRIVATE /WX)
endif()

# ---------- Wren（与 GUI 向导共用同一个 submodule；只加一次） ----------
if(NOT TARGET Wren::wren)
  set(WREN_BUILD_DEMO OFF)
  set(WREN_BUILD_TESTS OFF)
  set(WREN_WARNINGS_AS_ERRORS ${MSIXPKG_WARNINGS_AS_ERRORS})
  add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/gui/ui" "${CMAKE_CURRENT_BINARY_DIR}/wren")
endif()

# ---------- 资源（版本信息 + 图标） ----------
set(MSIXPKG_SETUP_ICON_PATH "${CMAKE_CURRENT_SOURCE_DIR}/res/msixpkg.ico")

set(MSIXPKG_SETUP_FILE_DESCRIPTION "Msixpkg Installer Setup")
set(MSIXPKG_SETUP_INTERNAL_NAME "msixpkg-setup")
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/setup/res/setup.rc.in"
               "${CMAKE_CURRENT_BINARY_DIR}/setup.rc" @ONLY)

set(MSIXPKG_SETUP_FILE_DESCRIPTION "Msixpkg Installer Uninstaller")
set(MSIXPKG_SETUP_INTERNAL_NAME "msixpkg-uninstall")
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/setup/res/setup.rc.in"
               "${CMAKE_CURRENT_BINARY_DIR}/uninstall.rc" @ONLY)

# ---------- Wren 页面 + 复用向导的 Card/StepRail/PhaseTimeline ----------
add_library(msixpkg_setup_ui STATIC
  setup/setup_ui.cpp
  setup/setup_widgets.cpp
  # 与安装向导共用同一套装饰控件，避免 setup 的卡片/步骤条各画一遍。
  gui/gui_widgets.cpp
)
target_include_directories(msixpkg_setup_ui
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/setup"
          "${CMAKE_CURRENT_SOURCE_DIR}/gui"
          "${CMAKE_CURRENT_SOURCE_DIR}/gui/ui"
          "${CMAKE_CURRENT_SOURCE_DIR}/res"
          "${CMAKE_CURRENT_BINARY_DIR}")
target_compile_options(msixpkg_setup_ui PRIVATE
  /utf-8 /W4 /permissive- /EHsc /Zc:__cplusplus)
target_link_libraries(msixpkg_setup_ui PUBLIC msixpkg_setup_core Wren::wren comctl32)

if(MSIXPKG_WARNINGS_AS_ERRORS)
  target_compile_options(msixpkg_setup_ui PRIVATE /WX)
endif()

# ---------- 两个进程：setup 与 Uninstall.exe ----------
add_executable(msixpkg_setup WIN32
  setup/setup_main.cpp
  setup/setup_entry.cpp
  "${CMAKE_CURRENT_BINARY_DIR}/setup.rc"
)
target_link_libraries(msixpkg_setup PRIVATE msixpkg_setup_ui)
target_compile_options(msixpkg_setup PRIVATE /utf-8 /W4 /permissive- /EHsc /Zc:__cplusplus)
target_link_options(msixpkg_setup PRIVATE
  "/MANIFEST:EMBED"
  "/MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/app.manifest")
set_target_properties(msixpkg_setup PROPERTIES
  OUTPUT_NAME "msixpkg-setup"
  VS_DPI_AWARE "PerMonitor")

add_executable(msixpkg_uninstall WIN32
  setup/uninstall_main.cpp
  setup/setup_entry.cpp
  "${CMAKE_CURRENT_BINARY_DIR}/uninstall.rc"
)
target_link_libraries(msixpkg_uninstall PRIVATE msixpkg_setup_ui)
target_compile_options(msixpkg_uninstall PRIVATE /utf-8 /W4 /permissive- /EHsc /Zc:__cplusplus)
target_link_options(msixpkg_uninstall PRIVATE
  "/MANIFEST:EMBED"
  "/MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/app.manifest")
set_target_properties(msixpkg_uninstall PROPERTIES
  OUTPUT_NAME "msixpkg-uninstall"
  VS_DPI_AWARE "PerMonitor")

set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/setup.rc"
                            "${CMAKE_CURRENT_BINARY_DIR}/uninstall.rc"
                            PROPERTIES COMPILE_OPTIONS "/c65001")

if(MSIXPKG_WARNINGS_AS_ERRORS)
  target_compile_options(msixpkg_setup PRIVATE /WX)
  target_compile_options(msixpkg_uninstall PRIVATE /WX)
endif()

# ---------- 单元测试并入已有的原生测试程序 ----------
if(TARGET msixpkg_native_tests)
  target_sources(msixpkg_native_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/setup_tests.cpp")
  target_link_libraries(msixpkg_native_tests PRIVATE msixpkg_setup_core)
endif()

