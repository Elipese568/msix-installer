#pragma once

// setup / Uninstall.exe 的编排层：只做文件、注册表、快捷方式、PATH，
// 不依赖 Wren、也不依赖 MSIX 部署栈（与向导的 Installer 完全分开）。
//
// 线程模型：run_* 是阻塞的，GUI 把它放进工作线程；silent 直接在入口线程跑。
// 进度通过回调抛出结构化的 phase 索引 + 英文机器可读 detail，界面负责本地化。

#include <Windows.h>

#include <filesystem>
#include <functional>
#include <string>

#include "setup_log.h"
#include "setup_options.h"
#include "setup_strings.h"
#include "setup_util.h"

namespace msixpkg::setup {

enum class ProgressKind {
    Phase,
    Percent,
    Detail,
    WaitingForElevation,
    Finished,
};

struct ProgressUpdate {
    ProgressKind kind = ProgressKind::Percent;
    int percent = 0;
    int phase = -1; // 0..4，-1 表示不更新时间线
    bool failed = false;
    std::wstring detail;
    int exit_code = 0;
};

using ProgressCallback = std::function<void(const ProgressUpdate&)>;

struct Plan {
    bool ok = false;
    int exit_code = kExitPrecondition;
    std::wstring error;

    InstallMode mode = InstallMode::CurrentUser;
    std::filesystem::path install_dir;
    bool admin = false;
    bool existing_machine = false;
    bool existing_user = false;
    bool needs_elevation = false;
    bool target_busy = false;
    bool desktop = false;
};

struct InstallResult {
    int exit_code = kExitSuccess;
    bool warnings = false;
    std::wstring error;
    std::filesystem::path install_dir;
    bool elevated_child = false;
};

/// 只读预演：读已有安装记录、判断模式与目录、检查目标是否被占用。零副作用。
Plan plan_install(const Options& options, Log& log);
Plan plan_uninstall(const Options& options, Log& log);

InstallResult run_install(const Options& options, const Strings& strings, Log& log,
                          const ProgressCallback& progress);
InstallResult run_uninstall(const Options& options, const Strings& strings, Log& log,
                            const ProgressCallback& progress);

/// 卸载器退出前调用：把自己的 exe 改名并让一个 detached cmd 在进程退出后清掉它和空目录。
bool schedule_self_delete(const std::filesystem::path& self, const std::filesystem::path& install_dir,
                          Log& log);

/// 返回存在的状态库目录（空 = 没有）。只读，不创建任何目录。
std::filesystem::path find_state_records();

/// 校验内嵌载荷；供 --verify-payload 与构建脚本使用。
bool verify_embedded_payload(Log& log, std::wstring& error);

}  // namespace msixpkg::setup
