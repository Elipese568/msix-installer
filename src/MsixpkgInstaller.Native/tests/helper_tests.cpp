// 提权助手位置的测试：GUI 必须把**同目录的 CLI**（msixpkg-native.exe）当助手，
// 绝不能把自己（msixpkg-installer.exe）再拉起一次。
//
// 这条规则来自一次真实故障：GUI 用 current_executable_path() 当助手，于是
//   GUI ──runas──▶ GUI（不认识 cert-add，被当成容器路径）
//                 └─▶ 单实例交接：SendMessage(WM_COPYDATA) 给父窗口
//   父窗口 UI 线程：在 WM_COPYDATA 处理里等编排线程 ──▶ 编排线程在等子进程退出
//   子进程：在等父窗口返回 —— 三方互等，安装永久停在「正在等待管理员确认…」，
//   证书永远导不进去（日志最后一行永远是 `elevating: …`）。
//
// 所以这里盯住三件事：GUI 拿同目录的 CLI；CLI 拿自己；没有 CLI 时**明确失败**
// （而不是默默退回「用自己」）。

#include <filesystem>
#include <fstream>
#include <string>

#include "installer.h"
#include "test_harness.h"

namespace {

std::filesystem::path scratch_directory(const char* name) {
    const std::filesystem::path path =
        std::filesystem::path(MSIXPKG_REPO_ROOT) / "artifacts" / "native-tests" / name;
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
    std::filesystem::create_directories(path);
    return path;
}

void touch(const std::filesystem::path& path) {
    std::ofstream file(path, std::ios::binary);
    file << "placeholder (contents are irrelevant for path resolution)";
}

}  // namespace

MSIXPKG_TEST(helper_executable_is_the_cli_next_to_the_gui) {
    const std::filesystem::path directory = scratch_directory("helper-sibling");
    const std::filesystem::path self = directory / "msixpkg-installer.exe";
    const std::filesystem::path cli = directory / "msixpkg-native.exe";
    touch(self);
    touch(cli);

    std::string error = "unset";
    const std::filesystem::path helper = msixpkg::helper_executable_path(self, error);

    CHECK_MSG(helper == cli, "the GUI must elevate the CLI, not itself: got " + helper.string());
    CHECK_MSG(error.empty(), "no error expected, got " + error);
}

MSIXPKG_TEST(helper_executable_is_itself_for_the_cli) {
    // CLI 自己跑 install 时，助手就是它自己（cert-add 是它自己的子命令）。
    const std::filesystem::path directory = scratch_directory("helper-self");
    const std::filesystem::path self = directory / "msixpkg-native.exe";
    touch(self);

    std::string error = "unset";
    const std::filesystem::path helper = msixpkg::helper_executable_path(self, error);

    CHECK_MSG(helper == self, "the CLI is its own helper");
    CHECK_MSG(error.empty(), "no error expected, got " + error);
}

MSIXPKG_TEST(helper_executable_reports_a_missing_cli) {
    // 只有 GUI、没有 CLI：必须给出空路径 + 原因。默默退回「用自己」
    // 正是死锁的来源，所以这一条是回归护栏。
    const std::filesystem::path directory = scratch_directory("helper-missing");
    const std::filesystem::path self = directory / "msixpkg-installer.exe";
    touch(self);

    std::string error;
    const std::filesystem::path helper = msixpkg::helper_executable_path(self, error);

    CHECK(helper.empty());
    CHECK_MSG(!error.empty(), "a missing helper must be reported");
    CHECK_MSG(error.find("msixpkg-native.exe") != std::string::npos,
              "the error should name the expected helper, got: " + error);
}

MSIXPKG_TEST(elevation_policy_is_readable_and_explains_a_missing_uac_prompt) {
    // 「看不到 UAC」先看这一条：`ConsentPromptBehaviorAdmin = 0` 是「不提示直接提升」
    // （[MS-GPSB] 2.2.11.2），此时 runas 根本不会弹窗口 —— 助手是静默提权跑起来的。
    // 这里把本机生效值打出来，排查时不必再猜；同时守住「什么时候才该提示」这条不变式。
    const msixpkg::ElevationPolicy policy = msixpkg::read_elevation_policy();

    std::cout << "       (this machine) " << policy.describe() << "\n";

    CHECK(policy.consent_prompt_admin <= 5);
    CHECK(policy.consent_prompt_user <= 3);
    CHECK_MSG(policy.describe().find("ConsentPromptBehaviorAdmin=") != std::string::npos,
              "the log line must name the policy value that suppresses the prompt");

    // 只有 UAC 开着、且策略不是「不提示直接提升」时，管理员才会看到确认框。
    CHECK_EQ(policy.prompts_administrators(), policy.enable_lua && policy.consent_prompt_admin != 0);
}
