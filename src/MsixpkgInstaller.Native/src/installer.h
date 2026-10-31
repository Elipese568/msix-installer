#pragma once

// 安装 / 卸载编排（docs/installer-design.md §3、§5）。
//
// 对 UI 零依赖：只通过 interface.h 的三个接口与前端打交道。
// 前端（gui / console / silent）共用这一个实现。
//
// 安全语义（实现里不可妥协，见 docs/security-model.md）：
//  1. 校验在前、改动在后：任何写操作之前必须整份容器校验通过；
//  2. 未命中钉扎白名单默认拒绝；只有「同意提供方明确批准 + 记住发布者」才继续；
//  3. 卸载**先卸应用**，再按四条判定决定是否回收证书；existed_before 为真永不删；
//  4. 回滚只删本次创建的物（依赖包是共享的框架包，不回滚；证书只在「本次新增」时删）；
//  5. 提权只用于证书：主进程 asInvoker，证书步骤走 `runas` 助手并 SW_HIDE。

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "container.h"
#include "deployment.h"
#include "interface.h"
#include "state.h"

namespace msixpkg {

/// 原生安装器版本（用于容器清单的 `installerMinVersion` 门禁与 ARP 展示）。
inline constexpr char kInstallerVersion[] = "0.3.0";

struct InstallOptions {
    std::filesystem::path container;

    /// 调用方显式给的白名单（CLI 的 `--pin`）。会与下面两个来源合并。
    std::vector<std::string> allowed_publisher_thumbprints;
    bool require_pinned_publisher = false;

    /// 是否把「用户之前明确信任过」的发布者也算作已钉扎（TOFU 记忆）。
    /// 关掉它 = 每次安装都重新问一遍。
    bool use_remembered_publishers = true;

    bool allow_downgrade = false;
    bool all_users = false;
    bool dry_run = false;

    /// 是否允许为证书步骤拉起提权助手。false 时若确实需要导入证书就直接失败。
    bool allow_elevation = true;

    /// 解包后是否保留暂存目录（排查用；默认失败也清、成功也清）。
    bool keep_staging = false;
};

/// 一次安装的**只读**预演结果：给 dry-run、GUI 摘要页 / 同意页用。
///
/// 之前 GUI 为了拿到同意页需要的发布者/包信息，只能自己把 `validate()` 再跑一遍 ——
/// 那既重复又容易和编排里的判定漂移（例如钉扎结果）。现在两边共用这一个来源。
struct InstallPlan {
    bool valid = false;
    std::string failure_code;
    std::string failure_message;
    std::string failure_detail;

    std::filesystem::path container_path;
    Manifest manifest;
    bool has_manifest = false;
    bool has_publisher_certificate = false;

    std::string publisher_subject;
    std::string certificate_sha1;
    std::string certificate_sha256;
    std::string content_digest;
    std::string package_family_name;

    /// 命中「管理员白名单 / 显式 --pin / 之前记住的」中的任意一个。
    bool publisher_pinned = false;
    /// 只命中了「之前记住的」（用于同意页上区分「管理员批准」与「你自己上次同意的」）。
    bool publisher_remembered = false;
    /// 该证书是否**已经**在本机受信任人员存储里。
    bool certificate_already_trusted = false;
    /// 包仓库读不出来时为 false（未知）。
    bool already_installed = false;
    /// 合并后的白名单，编排的 TOCTOU 复检也要用同一份。
    std::vector<std::string> allowed_publisher_thumbprints;

    /// 同意页需要的全部信息；GUI 直接用它，不必自己重算。
    ConsentRequest consent;
    std::vector<PlannedAction> actions;
    /// 白名单 / 仓库读取过程中的问题，由调用方写进日志或界面。
    std::vector<std::string> warnings;
};

struct UninstallOptions {
    /// 二者给一个即可：install_id 优先（状态库里最精确），否则按 family name 找。
    std::string install_id;
    std::string family_name;
    /// 状态库丢失时的兜底：手工指定证书 SHA-1 指纹。
    std::string certificate_sha1;

    bool dry_run = false;
    bool allow_elevation = true;
    /// 是否允许回收证书（第四条判定里的「已获同意」）。默认允许，但还要满足其它三条。
    bool allow_certificate_removal = true;
};

struct InstallOutcome {
    bool installed = false;
    bool cancelled = false;
    bool elevation_declined = false;
    bool dry_run = false;

    std::string install_id;
    std::string family_name;
    std::string full_name;
    std::string version;
    std::string identity_name;
    std::string publisher_subject;
    std::string certificate_sha1;

    bool certificate_trusted = false;
    bool certificate_added_by_installer = false;
    bool certificate_existed_before = false;
    bool arp_created = false;
    std::string arp_hive;

    std::vector<PlannedAction> plan;
    std::string log_path;
    std::string data_root;
    std::string warning;
};

/// 证书回收的四条判定（T4 验收 1–3 的核心，`verify` / `repair` 也要用它）。
struct CertificateReclaimDecision {
    bool remove = false;
    /// a) 证书是安装器加进去的（existed_before == false）。
    bool added_by_installer = false;
    /// b) 应用**安装前就存在**这张证书 → 永久无权删除。
    bool existed_before = false;
    /// c) 状态库里还有别的 installId 引用同一个指纹。
    bool other_installations_reference = false;
    /// d) 系统里还有别的已注册包的 Publisher 等于该证书 Subject。
    bool other_packages_use_publisher = false;
    /// e) 容器策略允许在卸载时回收证书。
    bool policy_allows = false;
    /// f) 调用方已同意回收。
    bool consent_granted = false;
    /// 包仓库根本读不出来（受限进程 / AppXSvc 不可用）。
    /// 这种情况下**必须保守**：宁可留着证书，也不要在「不知道还有谁在用」时删掉它。
    bool repository_unavailable = false;
    /// 一句话说明「为什么删 / 为什么不删」。
    std::string reason;
};

struct UninstallOutcome {
    bool removed = false;
    bool certificate_removed = false;
    bool dry_run = false;
    std::string full_name;
    std::string family_name;
    CertificateReclaimDecision decision;
    std::vector<PlannedAction> plan;
    std::string log_path;
    std::string data_root;
    std::string warning;
};

class Installer {
public:
    Installer(IProgressSink& progress, IConsentProvider& consent, ICancelSource& cancel)
        : progress_(progress), consent_(consent), cancel_(cancel) {}

    InstallOutcome install(const InstallOptions& options);
    UninstallOutcome uninstall(const UninstallOptions& options);

    /// 只读预演：校验容器、合并钉扎白名单、算出同意页需要的一切与动作清单。
    /// **不创建任何文件、不写日志、不碰证书存储**（只读查询除外）。
    InstallPlan plan(const InstallOptions& options);

    /// 证书回收判定：只查询，不改动系统。
    static CertificateReclaimDecision evaluate_certificate_reclaim(
        const StateStore& store,
        const InstallationRecord& record,
        bool policy_allows,
        bool consent_granted);

    /// 本机 Windows 构建号（RtlGetVersion：GetVersionEx 在没有对应 manifest 声明时会撒谎）。
    static int64_t host_build_number();
    /// "x64" / "x86" / "arm64"。
    static std::string host_architecture();

private:
    IProgressSink& progress_;
    IConsentProvider& consent_;
    ICancelSource& cancel_;
};

/// 提权助手 `cert-add` 的结果。
struct CertificateHelperResult {
    bool added = false;
    bool existed_before = false;
    std::string sha1;
    std::string sha256;
    std::string subject;
};

/**
 * `cert-add` 子命令的实现（必须由**已提权**的进程调用）。
 *
 * 它会自己重新打开容器、重算指纹并与 `expected_sha1` 复核 —— 绝不信任父进程的断言：
 * 提权进程是攻击面最大的那一环，只认「容器里真的写了什么」。
 * 结果同时写进 `result_path`（JSON），让父进程拿到 added / existed_before。
 */
CertificateHelperResult certificate_add_from_container(
    const std::filesystem::path& container,
    const std::string& expected_sha1,
    const std::string& result_path);

/// `cert-remove` 子命令的实现（必须由**已提权**的进程调用）。
bool certificate_remove_by_sha1(const std::string& sha1_hex);

/// 提权助手（`cert-add` / `cert-remove`）的可执行文件路径。
///
/// **必须是与安装器同目录的 CLI `msixpkg-native.exe`**：只有它认识这两个子命令。
/// 曾经这里直接用了 `current_executable_path()`，于是 GUI（`msixpkg-installer.exe`）
/// 会把**自己**当助手再拉起一次：它把 `cert-add` 当成容器路径，转而去给已存在的窗口
/// 发 WM_COPYDATA（单实例交接），而那个窗口的 UI 线程正卡在等这次提权返回 ——
/// 三方互等，安装永久停在「正在等待管理员确认…」，GUI 变成「未响应」，
/// 证书也永远不会被导入（日志最后一行永远是 `elevating: …`）。
///
/// `self` 是调用方自己的可执行文件路径（通常就是 `current_executable_path()`）。
/// CLI 自己运行时（命令行前端 `install`）直接返回它自己。
/// 找不到助手时返回空路径并把原因写进 `error` —— **永不抛**。
std::filesystem::path helper_executable_path(const std::filesystem::path& self, std::string& error);

/// 本机的 UAC 提权策略（`HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Policies\System`）。
///
/// 为什么要读它：`ShellExecuteEx(runas)` 会不会弹确认框**完全由这几项决定**。
/// `ConsentPromptBehaviorAdmin = 0`（「不提示直接提升」，见 [MS-GPSB] 2.2.11.2）
/// 时根本不会有 UAC 窗口：助手是静默提权跑起来的，用户只会看到
/// 「正在等待管理员确认…」却永远等不到窗口，于是以为提权卡住了
/// （0.2.0 那次「看不到 UAC」的现场就是这样：策略是 0，而真正的故障是 GUI 把自己当助手）。
///
/// 把生效策略写进日志，下一次「为什么没有 UAC / 为什么提权失败」就不用再猜。
struct ElevationPolicy {
    bool enable_lua = true;
    uint32_t consent_prompt_admin = 5;
    uint32_t consent_prompt_user = 3;
    uint32_t prompt_on_secure_desktop = 1;
    uint32_t validate_admin_code_signatures = 0;

    /// 管理员请求提权时本机会不会弹确认框（`EnableLUA=0` 或策略 0 = 不会）。
    [[nodiscard]] bool prompts_administrators() const noexcept;
    /// 一行诊断，写进日志。
    [[nodiscard]] std::string describe() const;
};

/// 读取上面的策略。读不到注册表时返回默认值（与 Windows 出厂设置一致），**永不抛**。
ElevationPolicy read_elevation_policy();

}  // namespace msixpkg
