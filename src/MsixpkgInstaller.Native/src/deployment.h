#pragma once

// 部署封装：用 C++/WinRT 调用 Windows 部署栈（PackageManager），把 MSIX 装 / 卸给
// **当前交互用户**（不是管理员账户 —— 见 docs/installer-design.md §3 的语义修正）。
//
// 设计要点：
//  - 这个头**不包含任何 Windows / WinRT 头**：CLI、GUI、测试都能安全地包含它，
//    只有 deployment.cpp 一个编译单元知道 WinRT 的存在。
//  - 「纯函数层」（publisher_id_from_dn / package_family_name / file_uri_from_path /
//    plan_install / plan_uninstall / describe_deployment_failure）不碰系统，
//    全部可单测；dry-run 与 GUI 摘要页共用同一份计划渲染。
//  - 失败一律抛 msixpkg::Error，code 用 deployment_codes::* —— 刻意与容器格式错误码
//    （docs/msixpkg-format-v1.md §8 / C# 端 MsixpkgErrors.cs 的 42 个 E_* 码）分开：
//    那一组必须逐字一致，这一组只描述本机部署栈的失败分类。

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace msixpkg {

/// 安装期错误码（**不是**容器格式错误码，见文件头注释）。
namespace deployment_codes {

inline constexpr char Failed[] = "E_DEPLOY_FAILED";
inline constexpr char Cancelled[] = "E_DEPLOY_CANCELLED";
inline constexpr char AccessDenied[] = "E_DEPLOY_ACCESS_DENIED";
inline constexpr char PublisherNotTrusted[] = "E_DEPLOY_PUBLISHER_NOT_TRUSTED";
inline constexpr char SignatureInvalid[] = "E_DEPLOY_SIGNATURE_INVALID";
inline constexpr char DependencyMissing[] = "E_DEPLOY_DEPENDENCY_MISSING";
inline constexpr char HigherVersionInstalled[] = "E_DEPLOY_HIGHER_VERSION_INSTALLED";
inline constexpr char PackageInUse[] = "E_DEPLOY_PACKAGE_IN_USE";
inline constexpr char PackageNotFound[] = "E_DEPLOY_PACKAGE_NOT_FOUND";
inline constexpr char PackageInvalid[] = "E_DEPLOY_PACKAGE_INVALID";
inline constexpr char BlockedByPolicy[] = "E_DEPLOY_BLOCKED_BY_POLICY";
inline constexpr char OutOfDiskSpace[] = "E_DEPLOY_OUT_OF_DISK_SPACE";
inline constexpr char ArchitectureMismatch[] = "E_DEPLOY_ARCHITECTURE_MISMATCH";
inline constexpr char AllUsersNotPermitted[] = "E_DEPLOY_ALL_USERS_NOT_PERMITTED";

}  // namespace deployment_codes

// ---------- 纯函数层 ----------

/**
 * Appx 的 PublisherId：对发布者 DN 的 **UTF-16LE** 字节做 SHA-256，取前 8 字节，
 * 用 Crockford base32 字母表 `0123456789abcdefghjkmnpqrstvwxyz`（去掉 i / l / o / u）
 * 编码成 13 个字符。它与 `PackageId::PublisherId` 完全一致。
 *
 * 自己能算出来是有意义的：容器清单里只有 `Publisher`（DN），而卸载、ARP 条目、
 * 引用计数判定都需要包族名 `<Name>_<PublisherId>`。
 */
std::string publisher_id_from_dn(std::string_view publisher_dn);

/// 包族名 = `<Name>_<PublisherId>`，与 `PackageId::FamilyName` 一致。
std::string package_family_name(std::string_view name, std::string_view publisher_dn);

/// 本机路径 → `file:///…` URI（对 UTF-8 字节按 RFC 3986 做百分号编码）。
/// 部署栈只接受 URI，而路径里完全可能带空格或中文。
std::string file_uri_from_path(const std::filesystem::path& path);

/// 计划里的一个动作：dry-run、GUI 摘要页、日志共用。
struct PlannedAction {
    std::string step;    // 稳定标识（英文小写），供脚本与 GUI 分支
    std::string detail;  // 一行人类可读明细
};

struct InstallRequest {
    std::filesystem::path main_package;               // 已解包到暂存目录的主包 .msix
    std::vector<std::filesystem::path> dependencies;  // 依赖（框架）包，先装
    /// 主包的预期身份（来自容器清单），装完用它回读真实的 FullName / FamilyName。
    std::string identity_name;
    std::string identity_publisher;
    std::string identity_version;
    std::string identity_architecture;
    bool allow_downgrade = false;
    bool force_target_app_shutdown = true;
    /// 让所有用户都可用：先由提权阶段 stage 到系统卷，再 provisioning。
    bool all_users = false;
};

struct InstallResult {
    std::string full_name;
    std::string family_name;
    std::string version;
    std::string architecture;
    bool provisioned = false;
    /// 装完在包仓库里没找到预期身份时为 true（安装本身成功了，只是回读失败）。
    bool identity_lookup_failed = false;
};

struct UninstallRequest {
    std::string family_name;
    /// 空 = 用 family_name 在当前用户已注册的包里解析；非空 = 直接按 FullName 卸载。
    std::string full_name;
};

struct UninstallResult {
    bool removed = false;
    std::string full_name;
    std::string family_name;
};

/// 已注册包的摘要（list / verify / repair / 卸载共用）。
struct RegisteredPackage {
    std::string name;
    std::string full_name;
    std::string family_name;
    std::string version;
    std::string architecture;
    std::string publisher;      // DN
    std::string publisher_id;
    std::string installed_path;
    std::string status;         // "Ok" / "NeedsRemediation" / …
    bool development_mode = false;
    bool is_framework = false;
};

std::vector<PlannedAction> plan_install(const InstallRequest& request);
std::vector<PlannedAction> plan_uninstall(const UninstallRequest& request);

/// 部署栈失败的翻译结果。
struct DeploymentFailure {
    std::string code;      // deployment_codes::*
    uint32_t hresult = 0;  // 扩展错误码
    std::string name;      // Windows 符号名（从 SDK 表查得，可能为空）
    std::string hint;      // 一句可操作提示（英文；GUI 再按 code 本地化）
};

/// 把 HRESULT 映射成「稳定码 + 系统名 + 可操作提示」。纯函数。
DeploymentFailure describe_deployment_failure(uint32_t hresult);

// ---------- 系统访问层 ----------

/// 进度回调：0..100 的百分比与一行阶段描述。可能为 nullptr。
using DeploymentProgress = std::function<void(uint32_t percent, const std::wstring& message)>;

/// 依次安装依赖包与主包（当前用户），返回装完回读到的身份。失败抛 Error。
InstallResult install_packages(const InstallRequest& request, const DeploymentProgress& progress = {});

/// 卸载：先按 FullName / FamilyName 定位，再 RemovePackageAsync。找不到视为已卸载。
UninstallResult uninstall_package(const UninstallRequest& request, const DeploymentProgress& progress = {});

/// 当前用户已注册的包；family_name 为空表示全部。单个包读属性失败会被跳过（不整体失败）。
std::vector<RegisteredPackage> find_registered_packages(std::string_view family_name = {});

bool is_package_registered(std::string_view family_name);

/// 面向所有用户的 provisioning（需管理员 + 系统卷）。默认关闭。
void provision_package_for_all_users(std::string_view family_name, const DeploymentProgress& progress = {});
void deprovision_package_for_all_users(std::string_view family_name);

/// 用 `shell:AppsFolder\<AUMID>` 拉起应用；AUMID = `<PackageFamilyName>!<ApplicationId>`。
void activate_application(std::string_view aumid);

}  // namespace msixpkg
