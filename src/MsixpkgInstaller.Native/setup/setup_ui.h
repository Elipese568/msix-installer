#pragma once

// Wren 自绘的 setup / 卸载器前端。入口在 setup_main.cpp / uninstall_main.cpp；
// 安装引擎在 setup_engine.cpp，两者通过 ProgressUpdate 通信。

#include <Windows.h>

#include "setup_options.h"

namespace msixpkg::setup {

class Strings;
class Log;

/// 安装模式的向导；返回进程退出码（0/4/5/6/7）。
int run_setup_ui(HINSTANCE instance, const Options& options, const Strings& strings, Log& log);

/// 卸载模式的确认 + 进度 + 完成页；返回进程退出码。
int run_uninstall_ui(HINSTANCE instance, const Options& options, const Strings& strings, Log& log);

}  // namespace msixpkg::setup
