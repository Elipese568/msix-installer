#pragma once

// 向导的共享类型（五个页面、进度接收方、同意信箱、退出码）。
//
// 这里**不包含任何引擎头**：gui_worker.cpp 是唯一接触 Installer / StateStore /
// 部署栈的编译单元，其余文件只认这些纯数据结构和 Win32。这样做的收益很实际 ——
// 界面代码不会因为引擎头（certs.h 会拉进 wincrypt.h）的宏而与 Windows 头打架。

#include <Windows.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "gui_strings.h"

namespace msixpkg::gui {

/// 退出码与 CLI 逐字一致（cli/main.cpp 的 exit_codes）：CI 与 E2E 脚本依赖它。
namespace exit_code {
constexpr int kSuccess = 0;
constexpr int kUnexpected = 1;
constexpr int kContainerInvalid = 2;
constexpr int kUsage = 3;
constexpr int kIoError = 4;
constexpr int kDeploymentFailed = 5;
constexpr int kCancelled = 6;
constexpr int kPrecondition = 7;
}  // namespace exit_code

// ---------- 五个页面 ----------
enum class WizardPage {
    Summary,   // 摘要：将要装什么
    Consent,   // 同意：将要信任谁（安全关键）
    Progress,  // 进度：阶段列表 + 真实百分比 + 取消
    Result,    // 结果：启动应用 / 打开日志 / 复制详情
    Manage,    // 管理：列本安装器装过的包，卸载 / 修复 / 打开设置
};

/// 逻辑分区：决定一组控件在哪些页上可见。外壳（标题 + 导航条）在所有页共用。
enum class PageLayout {
    Shell,
    Summary,
    Consent,
    Progress,
    Result,
    Manage,
};

// ---------- 跨线程消息 ----------
// WM_APP 之后的私有消息。只用 PostMessage 携带，wParam/lParam 只放整数，
// 其它一切都在 SharedState 里带着锁读写。
inline constexpr UINT kMsgProgress = WM_APP + 1;          // wParam=percent，lParam=等待提权
inline constexpr UINT kMsgConsentAsk = WM_APP + 2;        // 编排线程请求展示同意页
inline constexpr UINT kMsgInstallDone = WM_APP + 3;       // 安装结束（成功/失败/取消）
inline constexpr UINT kMsgPreviewReady = WM_APP + 4;      // 摘要页数据就绪
inline constexpr UINT kMsgManageLoaded = WM_APP + 5;      // 管理页数据就绪
inline constexpr UINT kMsgUninstallDone = WM_APP + 6;     // 卸载结束
inline constexpr UINT kMsgRepairReady = WM_APP + 7;       // 修复建议算好了

// ---------- 单实例与窗口交接 ----------
/// 单实例互斥体名（Global\ 前缀：跨会话也要挡住，否则两个用户同时往证书存储里写）。
inline constexpr wchar_t kSingleInstanceMutex[] = L"Global\\MsixpkgInstaller.Single";
/// WM_COPYDATA 的标识，用于把新容器路径交给已有实例。
inline constexpr ULONG_PTR kCopyDataMagic = 0x4D535850ull;  // 'MSXP'
/// 已有实例不接受新容器（正在装东西）时的应答码。
inline constexpr ULONG_PTR kCopyDataBusy = 0x42555359ull;   // 'BUSY'
/// 单实例交接的等待上限。对端卡住时，第二个实例绝不能跟着一起卡死。
inline constexpr UINT kCopyDataTimeoutMs = 10000;

// ---------- 容器摘要（摘要页 + 同意页的数据） ----------
struct PackageView {
    bool ok = false;                    // 容器是否通过校验
    std::wstring error_code;            // 失败时的稳定错误码
    std::wstring error_message;         // 失败时的一句话
    /// 校验失败时进程该返回的退出码（与 CLI 一致：2 校验失败 / 3 前置条件 / 4 I/O）。
    /// 由编排线程算好带过来 —— 错误码到退出码的翻译只应该有一份实现。
    int exit_code = 0;
    std::string name;                   // Identity/@Name
    std::string display_name;           // 显示名（清单里有就用它）
    std::string version;
    std::string architecture;
    std::string publisher;              // Identity/@Publisher（DN）
    std::string description;
    uint64_t total_size = 0;
    std::vector<std::string> dependencies;
    bool already_installed = false;
    bool allow_all_users = false;

    // 证书与钉扎（同意页的三个「是/否」与指纹）
    std::string subject;
    std::string thumbprint_sha1;
    std::string thumbprint_sha256;
    std::string not_before_utc;
    std::string not_after_utc;
    bool has_certificate = false;
    bool already_trusted = false;
    bool pinned = false;
    bool unpinned_publisher = false;

    std::vector<std::wstring> plan;     // 计划步骤的一行描述（来自 InstallPlan.actions）
    // 摘要阶段没有日志文件：plan() 是零副作用的，日志要等真正开始安装才建。
    std::wstring family_name;
    std::string application_id;         // 内层 AppxManifest 的 Application/@Id（启动应用用）
};

// ---------- 进度 ----------
/// Phase 的镜像。界面层刻意不包含 interface.h，所以阶段在跨界处被翻译成这个枚举；
/// 两张表一一对应（gui_worker.cpp 里有一处 switch 做这个翻译）。
enum class PhaseView {
    Idle,
    Validate,
    Stage,
    Consent,
    Certificate,
    Dependencies,
    MainPackage,
    Provision,
    Reclaim,
    Rollback,
    Done,
    Failed,
};

// ---------- 安装结果 ----------
enum class InstallStatus { Success, Failure, Cancelled };

struct InstallView {
    /// 是否已有结果。默认 false：默认构造出的 InstallView 是「还没有结果」，
    /// 而不是「装失败了」——结果页靠它决定是渲染结果还是继续等。
    bool present = false;
    InstallStatus status = InstallStatus::Failure;
    int exit_code = exit_code::kUnexpected;
    std::wstring error_code;
    std::wstring error_message;
    std::wstring error_detail;
    /// 可操作建议在字符串表里的编号（0 = 没有建议）。这里只带编号不带文案：
    /// 文案表属于 UI 线程，编排线程不该去碰资源。
    int hint_id = 0;
    std::wstring log_path;
    std::wstring family_name;
    std::string application_id;         // AUMID = family_name + "!" + application_id
    bool elevation_declined = false;
    bool installed = false;
    bool certificate_added = false;
    bool certificate_existed_before = false;
};

// ---------- 管理页 ----------
struct ManageEntry {
    std::string install_id;
    std::wstring name;
    std::wstring version;
    std::wstring certificate_sha1;
    std::wstring status;                // present / missing / partial / failed
    std::wstring detail;                // 详情框里的一整段说明
    bool package_present = false;
    bool certificate_present = false;
};

struct ManageView {
    bool loaded = false;
    std::wstring data_root;
    std::vector<ManageEntry> entries;
    std::wstring notice;                // 空 = 没有提示
    std::wstring repair_text;           // 修复建议全文（只读；空 = 还没算过）
};

// ---------- 同意问答（编排线程 ←→ UI 线程） ----------
/// 编排线程通过 GuiConsent::Ask 把一个待决问题挂进 SharedState 并唤醒 UI；
/// UI 在同意页上把用户的选择填回 decision，再让编排线程继续。
struct PendingConsent {
    std::atomic<bool> active{false};
    std::atomic<bool> answered{false};
    bool approved = false;
    bool remember_publisher = false;
    bool all_users = false;
};

/// 用户在同意页上按下「下一步」时给出的决定。
///
/// 安装线程真正走到 consent.Ask 时，如果这里已经有一份决定就直接消费它，
/// 不再把同意页推上来 —— 否则用户要点两次「下一步」（第一次只是启动了安装线程，
/// 线程紧接着又发了一次同意请求）。
struct ConsentAnswers {
    bool approved = false;
    bool remember_publisher = false;
    bool all_users = false;
};

// ---------- 共享状态 ----------
/// 只有「两条线程都碰」的东西才放这里。每个字段都受 mutex 保护；
/// 例外是几个 atomic 的「握手」标志，它们本身就是同步原语。
struct SharedState {
    std::mutex mutex;
    std::atomic<HWND> target{nullptr};         // 窗口句柄：窗口可能先消失，必须原子读
    std::atomic<bool> cancel{false};           // 取消请求（安装页的取消按钮 / 关窗）
    std::wstring container_path;               // 当前容器（也用于 WM_COPYDATA 换入）
    PackageView preview;
    InstallView install;
    ManageView manage;
    uint32_t percent = 0;
    PhaseView phase = PhaseView::Idle;
    bool waiting_for_elevation = false;
    std::vector<std::wstring> phase_log;
    std::shared_ptr<PendingConsent> consent;   // 本次「请求-应答」；一次性
    /// 同意页已经给出的决定，只供紧接着的那次 Ask 消费（一次性）。
    std::optional<ConsentAnswers> preapproved_consent;
};

// ---------- 编排线程的上下文 ----------
//
// 刻意用 shared_ptr 持有共享状态：任务线程活多久它就活多久，所以窗口先关也
// 不会让线程摸到悬垂对象。窗口句柄用原子量而不是值 —— 它随时可能被销毁。
struct WorkerContext {
    std::atomic<HWND>* target = nullptr;
    std::shared_ptr<SharedState> shared;
};

// ---------- 命令行 ----------
struct CommandLine {
    std::filesystem::path container;    // 可选的位置参数
    bool manage = false;
    bool help = false;

    /// `uninstall --family-name <PFN>` / `uninstall --install-id <id>`。
    /// ARP 的 UninstallString 就是这个形状（见 arp.cpp 的 make_arp_entry），
    /// 所以「设置 → 应用 → 卸载」必须由向导认下来 —— 否则 `uninstall` 会被
    /// 当成容器路径，用户点卸载只会看到一个 E_IO。
    bool uninstall = false;
    std::wstring uninstall_family_name;
    std::wstring uninstall_install_id;

    /// GUI **不实现**的 CLI 子命令（cert-add / cert-remove / list / verify / repair / clean）。
    /// 拿到它必须立刻失败退出：把它当容器路径会走进单实例交接，
    /// 而那正是「提权助手被换成本程序」时死锁的入口（见 gui_wizard.cpp 的 run()）。
    std::wstring unsupported_subcommand;

    // ---------- 诊断参数（只影响窗口这一次运行，不改变安装语义） ----------
    /// `--ui-page <summary|consent|progress|result|manage>`：强制打开某一页，
    /// 用于截图/无障碍回归；缺数据时页面按空态渲染。
    std::wstring ui_page;
    /// `--ui-result <success|failure|cancelled|declined>`：往 Result 页塞一份合成结果，
    /// 只用于截图与无障碍回归（不启动编排线程、不碰系统）。
    std::wstring ui_result;
    /// `--ui-theme <auto|light|dark|hc>` / `--ui-material <auto|solid|mica|acrylic>`：
    /// 强制外观与材质，用于浅色/深色/高对比度截图回归。
    std::wstring ui_theme;
    std::wstring ui_material;
    /// `--ui-progress <0..100>`：塞一份合成进度（阶段 + 日志 + 百分比），用于进度页回归。
    int ui_percent = -1;
    /// `--ui-report <path>`：退出前把主题、帧统计、MSAA 子对象写到文件（wren-ui 的验收方式）。
    std::wstring report_path;
    /// `--exit-after <ms>`：到时间自动关窗，配合 --ui-report 做无人值守烟测。
    int exit_after_ms = 0;
};

/// 解析 `[install] [<container.msixpkg>] [--manage] [--help]`
/// 以及 `uninstall [--family-name <PFN>|--install-id <id>]`。
CommandLine parse_command_line(const std::wstring& command_line);

// ---------- 控件表 ----------
/// 一个控件在一页上的逻辑（dp）位置。
struct ControlSpec {
    int id;
    const wchar_t* klass;
    PageLayout group;
    DWORD style;
    DWORD extended;
    int x;
    int y;
    int width;
    int height;
};

/// 五页的全部控件（含控件类与逻辑坐标）。创建与布局都从这一张表走，
/// 所以「加一个控件」只需要改一个地方，也不会出现创建了却没参与布局的孤儿控件。
const std::vector<ControlSpec>& control_layout();

/// 页 → 该页独有的布局分组。
PageLayout layout_group(WizardPage page);

}  // namespace msixpkg::gui
