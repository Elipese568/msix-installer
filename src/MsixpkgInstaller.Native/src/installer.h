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

#include "deployment.h"
#include "interface.h"
#include "state.h"

namespace msixpkg {

/// 原生安装器版本（用于容器清单的 `installerMinVersion` 门禁与 ARP 展示）。
inline constexpr char kInstallerVersion[] = "0.2.0";

struct InstallOptions {
    std::filesystem::path container;

    /// 允许的发布者 SHA-256 指纹白名单（小写）。空且 require_pinned_publisher=false 时，
    /// 未命中钉扎会交给同意提供方决定（TOFU）。
    std::vector<std::string> allowed_publisher_thumbprints;
    bool require_pinned_publisher = false;

    bool allow_downgrade = false;
    bool all_users = false;
    bool dry_run = false;

    /// 是否允许为证书步骤拉起提权助手。false 时若确实需要导入证书就直接失败。
    bool allow_elevation = true;

    /// 解包后是否保留暂存目录（排查用；默认失败也清、成功也清）。
    bool keep_staging = false;
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

}  // namespace msixpkg
