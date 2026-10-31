#pragma once

// 版本号的单一事实来源仍是 src/MsixpkgInstaller.Native/res/msixpkg.rc 的 FILEVERSION；
// CMake 在配置期把它解析成编译定义，setup 的 exe 属性、ARP 与日志都用同一份。

#ifndef MSIXPKG_SETUP_VERSION
#define MSIXPKG_SETUP_VERSION L"0.0.0"
#endif

#ifndef MSIXPKG_SETUP_VERSION_QUAD
#define MSIXPKG_SETUP_VERSION_QUAD L"0.0.0.0"
#endif
