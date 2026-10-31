#pragma once

// setup / Uninstall.exe 的命令行。
//
// 兼容旧 NSIS 分发安装包的静默开关（/S /D= /CURRENTUSER /ALLUSERS /NOASSOC /NOPATH
// /NODESKTOP），这样既有 CI、部署脚本与文档不用改写；新增的长开关只用于诊断与自动化。
//
// 解析是纯函数：不碰注册表、不建目录、不写日志，因此可以直接单测。

#include <Windows.h>

#include <filesystem>
#include <string>
#include <vector>

namespace msixpkg::setup {

enum class InstallMode {
    Auto,        // 未指定：按已有安装与是否提权决定
    AllUsers,    // HKLM + %ProgramFiles%
    CurrentUser, // HKCU + %LOCALAPPDATA%\Programs
};

enum class UiPage {
    None,
    Welcome,
    Options,
    Progress,
    Finish,
    Confirm,
};

struct Options {
    bool silent = false;
    bool help = false;
    bool verify_payload = false;

    // 内部开关：提权重入的标记，防止无限递归。
    bool elevated = false;
    // 测试/受管环境：明确禁止自己弹 UAC（此时 /ALLUSERS 未提权会退 5）。
    bool no_elevate = false;

    InstallMode mode = InstallMode::Auto;
    std::filesystem::path install_dir;
    bool install_dir_explicit = false;

    bool no_assoc = false;
    bool no_path = false;
    // 旧包默认不建桌面快捷方式（NSIS 的 desktop section 是 /o）；/DESKTOP 显式打开。
    bool desktop = false;

    // 诊断 / 截图回归（只影响这一次运行，绝不改变安装语义）。
    UiPage ui_page = UiPage::None;
    std::wstring ui_result;   // success / failure / cancelled
    int ui_percent = -1;      // 0..100
    std::wstring ui_theme;    // auto / light / dark / hc
    std::wstring ui_material; // auto / solid / mica / acrylic
    std::wstring report_path;
    int exit_after_ms = 0;
    // 诊断/自动化：停在某页后自动执行该页的主动作（GUI E2E 用，绝不改变安装语义）。
    bool auto_mode = false;

    std::wstring log_dir; // 空 = 按可写性探测（%LOCALAPPDATA% → %TEMP%）
};

struct ParseResult {
    bool ok = false;
    Options options;
    std::wstring error;
};

/// 解析参数（argv[0] 是 exe 自身；可以是 nullptr）。
ParseResult parse_arguments(int argc, const wchar_t* const* argv);

/// 供提权重入使用：把选项序列化成命令行（不含 exe 路径）。
std::vector<std::wstring> elevation_arguments(const Options& options);

/// 供日志/界面使用的一句摘要。
std::wstring describe_options(const Options& options);

}  // namespace msixpkg::setup
