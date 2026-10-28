# 自绘 UI 框架的独立构建（docs/self-drawn-ui-framework.md §2.2）。
#
# 这里刻意做成可独立配置的 CMake 工程：框架 + demo 不依赖 msixpkg_core，
# 方便在没有引擎/没有 WinUI 的机器上单独跑起来验证渲染、材质与 MSAA。
# 后续接入安装器时，把 msixpkg_selfdrawn_ui 的源码加进 gui.cmake 即可，
# 或者从上层 add_subdirectory 本目录。

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  cmake_minimum_required(VERSION 3.25)
  project(MsixpkgSelfDrawnUi LANGUAGES CXX)
  set(CMAKE_CXX_STANDARD 20)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  set(CMAKE_CXX_EXTENSIONS OFF)
endif()

option(MSIXPKG_UI_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

set(MSIXPKG_UI_SOURCES
  canvas.cpp
  text_layout.cpp
  theme.cpp
  widget.cpp
  controls.cpp
  gdi_text.cpp
  presenter.cpp
  material.cpp
  host.cpp
  automation.cpp
)

add_library(msixpkg_selfdrawn_ui STATIC ${MSIXPKG_UI_SOURCES})

target_include_directories(msixpkg_selfdrawn_ui PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(msixpkg_selfdrawn_ui PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(msixpkg_selfdrawn_ui PRIVATE /utf-8 /W4 /permissive- /EHsc /Zc:__cplusplus)

# gdiplus：帧与绘制；dwmapi：材质/深色/圆角；oleacc：LresultFromObject；
# oleaut32：SysAllocString；advapi32：主题/强调色注册表。
target_link_libraries(msixpkg_selfdrawn_ui PUBLIC
  gdiplus
  dwmapi
  oleacc
  oleaut32
  ole32
  user32
  gdi32
  advapi32
)

if(MSIXPKG_UI_WARNINGS_AS_ERRORS)
  target_compile_options(msixpkg_selfdrawn_ui PRIVATE /WX)
endif()

# ---- demo ----
add_executable(msixpkg_ui_demo WIN32 demo/self_drawn_demo.cpp)
target_link_libraries(msixpkg_ui_demo PRIVATE msixpkg_selfdrawn_ui)
target_compile_options(msixpkg_ui_demo PRIVATE /utf-8 /W4 /permissive- /EHsc /Zc:__cplusplus)
set_target_properties(msixpkg_ui_demo PROPERTIES OUTPUT_NAME "msixpkg-ui-demo")

if(MSIXPKG_UI_WARNINGS_AS_ERRORS)
  target_compile_options(msixpkg_ui_demo PRIVATE /WX)
endif()

# ---- 纯逻辑测试（不需要窗口） ----
add_executable(msixpkg_ui_tests tests/ui_tests.cpp)
target_link_libraries(msixpkg_ui_tests PRIVATE msixpkg_selfdrawn_ui)
target_compile_options(msixpkg_ui_tests PRIVATE /utf-8 /W4 /permissive- /EHsc /Zc:__cplusplus)

if(MSIXPKG_UI_WARNINGS_AS_ERRORS)
  target_compile_options(msixpkg_ui_tests PRIVATE /WX)
endif()

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  enable_testing()
  add_test(NAME ui COMMAND msixpkg_ui_tests)
endif()
