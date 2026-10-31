#pragma once

#include <Windows.h>

namespace msixpkg::setup {

/// setup_main / uninstall_main 共用的入口：解析参数、开日志、初始化 COM、选择 UI 或静默。
int run_setup_process(HINSTANCE instance, bool uninstall_mode);

}  // namespace msixpkg::setup
