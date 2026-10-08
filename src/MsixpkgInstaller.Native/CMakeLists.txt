cmake_minimum_required(VERSION 3.25)

project(MsixpkgInstallerNative LANGUAGES CXX)

# 仓库根目录：用于定位 tests/vectors 黄金样本（在 src/MsixpkgInstaller.Native 之下）
get_filename_component(MSIXPKG_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(MSIXPKG_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

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
)

add_library(msixpkg_core STATIC ${MSIXPKG_CORE_SOURCES})
target_include_directories(msixpkg_core
  PUBLIC  "${CMAKE_CURRENT_SOURCE_DIR}/include"
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
target_compile_features(msixpkg_core PUBLIC cxx_std_20)
target_compile_options(msixpkg_core PRIVATE /utf-8 /W4 /permissive- /EHsc /Zc:__cplusplus)
target_compile_definitions(msixpkg_core PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
# bcrypt：SHA-256 / RSA / ECDSA；crypt32：证书解析（M3 只用前两者，但接口一次给全）
target_link_libraries(msixpkg_core PUBLIC bcrypt crypt32)

if(MSIXPKG_WARNINGS_AS_ERRORS)
  target_compile_options(msixpkg_core PRIVATE /WX)
endif()

add_executable(msixpkg_native_tests
  tests/main.cpp
  tests/vectors_tests.cpp
)
target_link_libraries(msixpkg_native_tests PRIVATE msixpkg_core)
# 白盒测试：允许直接包含内部头（json/inflate/zip 都是两端共享的格式契约，值得单独覆盖）。
target_include_directories(msixpkg_native_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_options(msixpkg_native_tests PRIVATE /utf-8 /W4 /permissive- /EHsc /Zc:__cplusplus)
target_compile_definitions(msixpkg_native_tests PRIVATE
  MSIXPKG_VECTORS_DIR="${MSIXPKG_REPO_ROOT}/tests/vectors"
  MSIXPKG_REPO_ROOT="${MSIXPKG_REPO_ROOT}"
)

# 用 VS 的调试器跑测试时能直接命中源码
set_property(DIRECTORY PROPERTY VS_STARTUP_PROJECT msixpkg_native_tests)

enable_testing()
add_test(NAME vectors COMMAND msixpkg_native_tests)
