cmake_minimum_required(VERSION 3.25)

project(MsixpkgInstallerNative LANGUAGES CXX)

# 仓库根目录：用于定位 tests/vectors 黄金样本（在 src/MsixpkgInstaller.Native 之下）
get_filename_component(MSIXPKG_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(MSIXPKG_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

# ---------- C++/WinRT 投影头（部署层专用；Windows SDK 自带，**不需要 NuGet**） ----------
# 投影头在 <SDK>\Include\<ver>\cppwinrt\ 下（winerror.h 则在 <SDK>\Include\<ver>\shared\ 下）。
# 这个目录不在编译器的默认包含路径里，必须显式加进来。
#
# 注意：不能写成 "$ENV{ProgramFiles(x86)}" —— CMake 的 $ENV{} 变量名不允许括号，
# 那会直接报 "Syntax error in cmake code"。所以改从注册表取 SDK 根目录。
set(MSIXPKG_CPPWINRT_INCLUDE "" CACHE PATH "Windows SDK 的 C++/WinRT 投影头目录（留空则自动探测）")

if(NOT MSIXPKG_CPPWINRT_INCLUDE)
  cmake_host_system_information(
    RESULT MSIXPKG_KITS_ROOT
    QUERY WINDOWS_REGISTRY "HKLM\\SOFTWARE\\Microsoft\\Windows Kits\\Installed Roots"
    VALUE "KitsRoot10" VIEW 64)

  if(MSIXPKG_KITS_ROOT)
    file(TO_CMAKE_PATH "${MSIXPKG_KITS_ROOT}" MSIXPKG_KITS_ROOT)
    if(NOT MSIXPKG_KITS_ROOT MATCHES "/$")
      set(MSIXPKG_KITS_ROOT "${MSIXPKG_KITS_ROOT}/")
    endif()

    if(CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION AND
       EXISTS "${MSIXPKG_KITS_ROOT}Include/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/cppwinrt")
      set(MSIXPKG_CPPWINRT_INCLUDE
          "${MSIXPKG_KITS_ROOT}Include/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/cppwinrt")
    else()
      file(GLOB MSIXPKG_CPPWINRT_CANDIDATES "${MSIXPKG_KITS_ROOT}Include/*/cppwinrt")
      if(MSIXPKG_CPPWINRT_CANDIDATES)
        list(SORT MSIXPKG_CPPWINRT_CANDIDATES)
        list(GET MSIXPKG_CPPWINRT_CANDIDATES -1 MSIXPKG_CPPWINRT_INCLUDE)
      endif()
    endif()
  endif()
endif()

if(NOT MSIXPKG_CPPWINRT_INCLUDE)
  message(FATAL_ERROR
    "找不到 Windows SDK 的 C++/WinRT 投影头（<SDK>/Include/<ver>/cppwinrt）。"
    "请安装 Windows SDK，或用 -DMSIXPKG_CPPWINRT_INCLUDE=<dir> 显式指定。")
endif()
message(STATUS "C++/WinRT projection headers: ${MSIXPKG_CPPWINRT_INCLUDE}")

set(MSIXPKG_CORE_SOURCES
  src/internal.cpp
  src/json.cpp
  src/inflate.cpp
  src/zip.cpp
  src/crypto.cpp
  src/certs.cpp
  src/container.cpp
  src/appx.cpp
  src/verify.cpp
  src/certstore.cpp
  src/deployment.cpp
  src/state.cpp
  src/log.cpp
  src/arp.cpp
  src/installer.cpp
  src/ops.cpp
  src/policy.cpp
)

add_library(msixpkg_core STATIC ${MSIXPKG_CORE_SOURCES})
target_include_directories(msixpkg_core
  PUBLIC  "${CMAKE_CURRENT_SOURCE_DIR}/include"
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
          # 只有 deployment.cpp 会包含 WinRT 投影头，所以这里是 PRIVATE：
          # 其它消费者（CLI / GUI / 测试）不会被拖进 C++/WinRT。
          "${MSIXPKG_CPPWINRT_INCLUDE}"
)
target_compile_features(msixpkg_core PUBLIC cxx_std_20)
target_compile_options(msixpkg_core PRIVATE /utf-8 /W4 /permissive- /EHsc /Zc:__cplusplus)
target_compile_definitions(msixpkg_core PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
# bcrypt：SHA-256 / RSA / ECDSA；crypt32：证书解析与证书存储；WindowsApp：C++/WinRT 部署栈；
# shell32：ShellExecuteW（用 shell:AppsFolder 拉起已安装的应用）。
target_link_libraries(msixpkg_core PUBLIC bcrypt crypt32 WindowsApp shell32)

if(MSIXPKG_WARNINGS_AS_ERRORS)
  target_compile_options(msixpkg_core PRIVATE /WX)
endif()

add_executable(msixpkg_native_tests
  tests/main.cpp
  tests/vectors_tests.cpp
  tests/certstore_tests.cpp
  tests/deployment_tests.cpp
  tests/deployment_exception_tests.cpp
  tests/state_tests.cpp
  tests/reclaim_tests.cpp
  tests/policy_tests.cpp
)
target_link_libraries(msixpkg_native_tests PRIVATE msixpkg_core)
# 白盒测试：允许直接包含内部头（json/inflate/zip 都是两端共享的格式契约，值得单独覆盖）。
target_include_directories(msixpkg_native_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_options(msixpkg_native_tests PRIVATE /utf-8 /W4 /permissive- /EHsc /Zc:__cplusplus)
if(MSIXPKG_WARNINGS_AS_ERRORS)
  # 测试代码同样按 /WX 要求：「Release 零警告」不能只在被测库上成立。
  target_compile_options(msixpkg_native_tests PRIVATE /WX)
endif()
target_compile_definitions(msixpkg_native_tests PRIVATE
  MSIXPKG_VECTORS_DIR="${MSIXPKG_REPO_ROOT}/tests/vectors"
  MSIXPKG_REPO_ROOT="${MSIXPKG_REPO_ROOT}"
)

# ---------- 命令行前端 ----------
add_executable(msixpkg_native cli/main.cpp)
target_link_libraries(msixpkg_native PRIVATE msixpkg_core)
target_include_directories(msixpkg_native PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_options(msixpkg_native PRIVATE /utf-8 /W4 /permissive- /EHsc /Zc:__cplusplus)
if(MSIXPKG_WARNINGS_AS_ERRORS)
  target_compile_options(msixpkg_native PRIVATE /WX)
endif()
# 控制台子系统：入口是 wmain（argv 直接拿到宽字符，避免代码页把中文路径拆坏）。
set_target_properties(msixpkg_native PROPERTIES OUTPUT_NAME "msixpkg-native")

# ---------- GUI 向导 ----------
# 目标定义放在 gui/gui.cmake 里单独维护（界面代码与核心库分开演进）；
# 文件不存在时构建照常可用（CI / 无 GUI 的场合）。
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/gui/gui.cmake")
  include("${CMAKE_CURRENT_SOURCE_DIR}/gui/gui.cmake")
endif()

# 用 VS 的调试器跑测试时能直接命中源码
set_property(DIRECTORY PROPERTY VS_STARTUP_PROJECT msixpkg_native_tests)
enable_testing()
add_test(NAME vectors COMMAND msixpkg_native_tests)
