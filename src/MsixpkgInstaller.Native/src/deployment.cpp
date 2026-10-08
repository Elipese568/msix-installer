#include "deployment.h"

// 这个编译单元是**唯一**允许看到 Windows / WinRT 头的部署相关单元（见 deployment.h）。

#include <Windows.h>
#include <shellapi.h>

#include <winrt/base.h>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Management.Deployment.h>

#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "crypto.h"
#include "deployment_errors.generated.h"
#include "internal.h"
#include "msixpkg/errors.h"

namespace msixpkg {
namespace {

/// 部署栈的类型名与我们自己的进度回调重名，各自取个别名避免歧义。
using WinRtDeploymentProgress = winrt::Windows::Management::Deployment::DeploymentProgress;
using WinRtDeploymentResult = winrt::Windows::Management::Deployment::DeploymentResult;
using WinRtPackage = winrt::Windows::ApplicationModel::Package;
using WinRtPackageManager = winrt::Windows::Management::Deployment::PackageManager;

std::string hresult_hex(uint32_t value) {
    char buffer[16] = {};
    std::snprintf(buffer, sizeof(buffer), "0x%08X", value);
    return buffer;
}

bool is_unreserved_uri_character(unsigned char value) {
    if (value >= 'A' && value <= 'Z') return true;
    if (value >= 'a' && value <= 'z') return true;
    if (value >= '0' && value <= '9') return true;
    return value == '-' || value == '.' || value == '_' || value == '~';
}

/// Crockford base32（去掉 i / l / o / u），与 Appx 的 PublisherId 编码一致。
std::string base32_crockford(const uint8_t* data, size_t size) {
    static constexpr char alphabet[] = "0123456789abcdefghjkmnpqrstvwxyz";
    std::string result;
    result.reserve((size * 8 + 4) / 5);

    uint32_t buffer = 0;
    int bits = 0;
    for (size_t index = 0; index < size; ++index) {
        buffer = (buffer << 8) | data[index];
        bits += 8;
        while (bits >= 5) {
            bits -= 5;
            result.push_back(alphabet[(buffer >> bits) & 0x1F]);
        }
    }

    if (bits > 0) {
        result.push_back(alphabet[(buffer << (5 - bits)) & 0x1F]);
    }

    return result;
}

std::string format_version(const winrt::Windows::ApplicationModel::PackageVersion& version) {
    return std::to_string(version.Major) + "." + std::to_string(version.Minor) + "." +
           std::to_string(version.Build) + "." + std::to_string(version.Revision);
}

std::string architecture_name(winrt::Windows::System::ProcessorArchitecture architecture) {
    using winrt::Windows::System::ProcessorArchitecture;
    switch (architecture) {
        case ProcessorArchitecture::X86: return "x86";
        case ProcessorArchitecture::X64: return "x64";
        case ProcessorArchitecture::Arm: return "arm";
        case ProcessorArchitecture::Arm64: return "arm64";
        case ProcessorArchitecture::Neutral: return "neutral";
        default: return "unknown";
    }
}

std::string status_name(const winrt::Windows::ApplicationModel::PackageStatus& status) {
    if (status.VerifyIsOK()) {
        return "Ok";
    }

    std::vector<std::string> flags;
    if (status.NotAvailable()) flags.emplace_back("NotAvailable");
    if (status.PackageOffline()) flags.emplace_back("PackageOffline");
    if (status.DataOffline()) flags.emplace_back("DataOffline");
    if (status.Disabled()) flags.emplace_back("Disabled");
    if (status.NeedsRemediation()) flags.emplace_back("NeedsRemediation");
    if (status.LicenseIssue()) flags.emplace_back("LicenseIssue");
    if (status.Modified()) flags.emplace_back("Modified");
    if (status.Tampered()) flags.emplace_back("Tampered");
    if (status.DependencyIssue()) flags.emplace_back("DependencyIssue");
    if (status.Servicing()) flags.emplace_back("Servicing");
    if (status.DeploymentInProgress()) flags.emplace_back("DeploymentInProgress");
    if (flags.empty()) {
        return "Unknown";
    }

    std::string joined = flags.front();
    for (size_t index = 1; index < flags.size(); ++index) {
        joined += ", " + flags[index];
    }

    return joined;
}

void report(const DeploymentProgress& progress, uint32_t percent, const std::wstring& message) {
    if (progress) {
        progress(percent, message);
    }
}

/// 当前线程必须在 MTA 中才能安全地 co_await / get 部署操作。
/// 重复初始化是安全的：RPC_E_CHANGED_MODE 只说明 COM 已经以别的模式初始化过
/// （例如 GUI 线程的 STA），那种情况下 WinRT 调用同样可用，不该当成失败。
void ensure_apartment() {
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
    } catch (const winrt::hresult_error& error) {
        if (error.code().value != RPC_E_CHANGED_MODE) {
            throw;
        }
    }
}

[[noreturn]] void throw_deployment_failure(uint32_t hresult, const std::string& raw) {
    const DeploymentFailure failure = describe_deployment_failure(hresult);
    std::string detail = failure.name.empty() ? std::string("HRESULT") : failure.name;
    detail += " " + hresult_hex(hresult);
    if (!raw.empty()) {
        detail += " - " + raw;
    }

    throw Error(failure.code, failure.hint, detail);
}

/// 安装一个包；失败抛 Error。percent_base/percent_span 把部署栈的 0..100 映射到整体进度。
void add_one_package(
    WinRtPackageManager& manager,
    const std::filesystem::path& package,
    const InstallRequest& request,
    const DeploymentProgress& progress,
    uint32_t percent_base,
    uint32_t percent_span) {
    winrt::Windows::Management::Deployment::AddPackageOptions options;
    options.ForceTargetAppShutdown(request.force_target_app_shutdown);
    options.ForceUpdateFromAnyVersion(request.allow_downgrade);
    // 失败时不要把半装的包留在磁盘上（回滚规则见 docs/installer-design.md §5）。
    options.RetainFilesOnFailure(false);

    const std::wstring display = package.filename().wstring();
    const winrt::Windows::Foundation::Uri uri(winrt::to_hstring(file_uri_from_path(package)));

    auto operation = manager.AddPackageByUriAsync(uri, options);

    // 按值捕获 sink：进度回调可能在 operation 完成前一直活着。
    const DeploymentProgress sink = progress;
    const std::wstring label = display;
    operation.Progress([sink, label, percent_base, percent_span](
                           auto const&, const WinRtDeploymentProgress& state) {
        if (!sink) {
            return;
        }

        const uint32_t mapped = percent_base + (state.percentage * percent_span) / 100;
        sink(mapped, L"Deploying " + label);
    });

    try {
        const WinRtDeploymentResult result = operation.get();
        const int32_t extended = result.ExtendedErrorCode().value;
        if (extended != 0) {
            throw_deployment_failure(
                static_cast<uint32_t>(extended), winrt::to_string(result.ErrorText()));
        }
    } catch (const winrt::hresult_error& error) {
        throw_deployment_failure(
            static_cast<uint32_t>(error.code().value), winrt::to_string(error.message()));
    }
}

/// 取包集合里的第一个包（包族名在当前用户下唯一）；空集合返回 false。
/// 刻意不用 range-for + break：那会让循环的递增表达式不可达，MSVC 报 C4702。
bool first_package(
    const winrt::Windows::Foundation::Collections::IIterable<WinRtPackage>& packages,
    WinRtPackage& result) {
    auto iterator = packages.First();
    if (!iterator.HasCurrent()) {
        return false;
    }

    result = iterator.Current();
    return true;
}

/// 从包仓库回读身份：装完拿到真实的 FullName / FamilyName。
bool lookup_installed_identity(const InstallRequest& request, RegisteredPackage& found) {
    for (const RegisteredPackage& candidate : find_registered_packages()) {
        if (candidate.name != request.identity_name) {
            continue;
        }

        if (!request.identity_publisher.empty() && candidate.publisher != request.identity_publisher) {
            continue;
        }

        // 版本只在显式给出时用于消歧：降级安装时仓库里可能短暂同时存在两个版本。
        if (!request.identity_version.empty() && candidate.version != request.identity_version) {
            found = candidate;
            continue;
        }

        found = candidate;
        return true;
    }

    return !found.full_name.empty();
}

}  // namespace

// ---------- 纯函数层 ----------

std::string publisher_id_from_dn(std::string_view publisher_dn) {
    const std::wstring wide = widen_utf8(publisher_dn);
    const auto* bytes = reinterpret_cast<const uint8_t*>(wide.data());
    const Bytes digest = sha256(bytes, wide.size() * sizeof(wchar_t));
    // 只取前 8 字节 → 13 个 base32 字符（64 bit → ceil(64/5)）。
    return base32_crockford(digest.data(), 8);
}

std::string package_family_name(std::string_view name, std::string_view publisher_dn) {
    return std::string(name) + "_" + publisher_id_from_dn(publisher_dn);
}

std::string file_uri_from_path(const std::filesystem::path& path) {
    const std::filesystem::path absolute = std::filesystem::absolute(path);
    const std::u8string utf8 = absolute.u8string();
    std::string text(reinterpret_cast<const char*>(utf8.data()), utf8.size());
    std::replace(text.begin(), text.end(), '\\', '/');

    // Windows 路径 C:/x 要变成 /C:/x，才能拼出 file:///C:/x。
    if (text.empty() || text.front() != '/') {
        text.insert(text.begin(), '/');
    }

    static constexpr char digits[] = "0123456789ABCDEF";
    std::string result = "file://";
    result.reserve(text.size() + 8);
    for (const char raw : text) {
        const auto value = static_cast<unsigned char>(raw);
        if (is_unreserved_uri_character(value) || value == '/' || value == ':') {
            result.push_back(raw);
            continue;
        }

        result.push_back('%');
        result.push_back(digits[(value >> 4) & 0x0F]);
        result.push_back(digits[value & 0x0F]);
    }

    return result;
}

std::vector<PlannedAction> plan_install(const InstallRequest& request) {
    std::vector<PlannedAction> actions;
    actions.push_back({"validate", "Verify the container signature, publisher pinning and payload hashes"});
    actions.push_back({"stage", "Extract the payload into a protected staging directory under %ProgramData%\\Msixpkg\\staging"});

    if (!request.identity_publisher.empty()) {
        actions.push_back(
            {"certificate",
             "Import the publisher certificate into LocalMachine\\TrustedPeople (elevated helper; skipped if already present)"});
    }

    for (const std::filesystem::path& dependency : request.dependencies) {
        actions.push_back({"dependency", "Install dependency package " + narrow_utf8(dependency.filename().wstring())});
    }

    std::string main = request.identity_name.empty() ? narrow_utf8(request.main_package.filename().wstring())
                                                    : request.identity_name;
    if (!request.identity_version.empty()) {
        main += " " + request.identity_version;
    }

    if (!request.identity_architecture.empty()) {
        main += " (" + request.identity_architecture + ")";
    }

    if (request.allow_downgrade) {
        main += " [allow downgrade]";
    }

    actions.push_back({"main", "Install " + main + " for the current user"});

    if (request.all_users) {
        actions.push_back({
            "provision",
            "Provision for all users (stage to the system volume, then provision): " +
                package_family_name(request.identity_name, request.identity_publisher)});
    }

    actions.push_back({"arp", "Create the \"Apps & features\" uninstall entry under HKLM\\...\\Uninstall"});
    actions.push_back({"journal", "Record the installation state under %ProgramData%\\Msixpkg\\state"});
    return actions;
}

std::vector<PlannedAction> plan_uninstall(const UninstallRequest& request) {
    std::vector<PlannedAction> actions;
    const std::string target = !request.full_name.empty()
        ? request.full_name
        : (request.family_name.empty() ? std::string("(unknown package)") : request.family_name);
    actions.push_back({"locate", "Resolve the registered package for " + target});
    actions.push_back({"shutdown", "Stop the application if it is running"});
    actions.push_back({"remove", "Remove the package: " + target});
    actions.push_back(
        {"certificate",
         "Reclaim the publisher certificate only if the installer added it AND no other installation references it"});
    actions.push_back({"arp", "Remove the \"Apps & features\" uninstall entry"});
    actions.push_back({"journal", "Delete the installation state record"});
    return actions;
}

namespace {

/// 规则表：按**顺序**匹配，第一条命中的规则胜出。
/// fragment 是对 Windows 符号名（本身就是大写）做子串匹配。
/// 顺序很关键：先判「签名本身有问题」，再判「信任问题」—— 否则
/// TRUST_E_NOSIGNATURE / TRUST_E_BAD_DIGEST 会被笼统地归到「证书不受信」。
struct FailureRule {
    const char* fragment;
    const char* code;
    const char* hint;
};

constexpr FailureRule kFailureRules[] = {
    {"NOT_SIGNED", deployment_codes::SignatureInvalid,
     "The package is not signed, or its signature was removed. Only packages signed by a trusted publisher can be installed."},
    {"NOSIGNATURE", deployment_codes::SignatureInvalid,
     "The package has no signature. Only packages signed by a trusted publisher can be installed."},
    {"BAD_DIGEST", deployment_codes::SignatureInvalid,
     "The package signature does not match its contents. The payload was modified or corrupted in transit."},
    {"SIGNATURE", deployment_codes::SignatureInvalid,
     "The package signature could not be validated. Re-obtain the package from the publisher."},
    {"TRUST", deployment_codes::PublisherNotTrusted,
     "The publisher certificate is not trusted on this machine. Import the publisher certificate into "
     "LocalMachine\\TrustedPeople and retry (msixpkg-native install does this for you when allowed)."},
    {"PREREQUISITE", deployment_codes::DependencyMissing,
     "A dependency (framework) package is missing. Ship the matching framework package and retry."},
    {"RESOLVE_DEPENDENCY", deployment_codes::DependencyMissing,
     "A dependency declared by the package could not be resolved. Ship the matching framework package and retry."},
    {"HOSTRUNTIME", deployment_codes::DependencyMissing,
     "A host runtime dependency is missing. Ship the matching runtime package and retry."},
    {"MISSING_REQUIRED_FILE", deployment_codes::PackageInvalid,
     "The package is missing a file its manifest requires. The package was built incorrectly or is truncated."},
    {"DOWNGRADE", deployment_codes::HigherVersionInstalled,
     "A higher version is already installed. Uninstall it first, or re-run with --allow-downgrade."},
    {"PACKAGE_UPDATING", deployment_codes::PackageInUse,
     "Another deployment operation for this package is already in progress. Wait for it to finish and retry."},
    {"STAGING_ONHOLD", deployment_codes::PackageInUse,
     "The package is staged but still on hold (usually because the app is running). Close the app and retry."},
    {"IN_USE", deployment_codes::PackageInUse,
     "The package or one of its files is in use. Close the app (and any shell window showing its folder) and retry."},
    {"REMOVEDEFAULTPACKAGES", deployment_codes::BlockedByPolicy,
     "Removing the default packages for this user is blocked by policy."},
    {"BLOCKED_BY_POLICY", deployment_codes::BlockedByPolicy,
     "Installation is blocked by machine policy. Ask an administrator to allow sideloading, or enable Developer Mode."},
    {"POLICY_FAILURE", deployment_codes::BlockedByPolicy,
     "Installation is blocked by machine policy. Ask an administrator to allow sideloading, or enable Developer Mode."},
    {"SIDELOAD", deployment_codes::BlockedByPolicy,
     "Sideloading of non-Store packages is disabled. Enable Developer Mode or the sideloading policy."},
    {"ABORT", deployment_codes::Cancelled,
     "The operation was aborted (usually because elevation was declined). No changes were kept."},
    {"CANCEL", deployment_codes::Cancelled,
     "The operation was cancelled (elevation was declined, or cancellation was requested). No changes were kept."},
    {"USEREXIT", deployment_codes::Cancelled,
     "The installation was cancelled by the user."},
    {"ACCESS_DENIED", deployment_codes::AccessDenied,
     "Access denied. The certificate step needs administrator rights; retry and accept the elevation prompt."},
    {"ELEVATION", deployment_codes::AccessDenied,
     "The operation requires administrator rights that were not granted."},
    {"NOT_REGISTERED_FOR_USER", deployment_codes::PackageNotFound,
     "The package is not registered for this user. It may already be uninstalled."},
    {"NOT_FOUND", deployment_codes::PackageNotFound,
     "The package was not found in the package repository. It may already be uninstalled."},
    {"OPEN_PACKAGE_FAILED", deployment_codes::PackageInvalid,
     "The package file could not be opened. It may be locked by another process or deleted."},
    {"INVALID_PACKAGE", deployment_codes::PackageInvalid,
     "The package is not a valid MSIX. Verify the payload was not modified after verification."},
    {"CORRUPT", deployment_codes::PackageInvalid,
     "The package repository entry or the package content is corrupted. Re-download the package and retry."},
    {"TAMPERED", deployment_codes::PackageInvalid,
     "The installed package files were modified after installation. Uninstall, then install again from a verified package."},
    {"WRONG_PROCESSOR_ARCHITECTURE", deployment_codes::ArchitectureMismatch,
     "The package architecture does not match this machine. Use an x64 package on x64 Windows."},
    {"ARCHITECTURE", deployment_codes::ArchitectureMismatch,
     "The package architecture does not match this machine."},
    {"DISK_FULL", deployment_codes::OutOfDiskSpace,
     "Not enough disk space to install the package. Free some space and retry."},
    {"OUT_OF_DISK_SPACE", deployment_codes::OutOfDiskSpace,
     "Not enough disk space to install the package. Free some space and retry."},
    {"ALREADY_EXISTS", deployment_codes::Failed,
     "A package with the same identity and version is already installed. Uninstall it first, or allow an update."},
    {"REGISTRATION_FAILURE", deployment_codes::Failed,
     "The package files were placed but registration failed. Run `msixpkg-native repair` to see what is inconsistent."},
    {"DEREGISTRATION_FAILURE", deployment_codes::Failed,
     "Unregistration failed. Close the app and retry; if it persists, run `msixpkg-native repair`."},
    {"INSTALL_FAILED", deployment_codes::Failed,
     "The deployment stack refused the package. The log contains the extended error and the failing stage."},
    {"FAILURE", deployment_codes::Failed,
     "The deployment stack reported a failure. See the log for the extended error."},
};

bool contains_fragment(const std::string& name, const char* fragment) {
    return name.find(fragment) != std::string::npos;
}

}  // namespace

DeploymentFailure describe_deployment_failure(uint32_t hresult) {
    DeploymentFailure failure;
    failure.hresult = hresult;
    failure.code = deployment_codes::Failed;

    for (const DeploymentErrorName& entry : kDeploymentErrorNames) {
        if (entry.code == hresult) {
            failure.name = entry.name;
            break;
        }
    }

    // 表里没有名字也不影响分类：先按数值猜几个最常见的 Win32 码。
    // 这里必须用**系统本来的符号名**（ERROR_ACCESS_DENIED 之类），
    // 因为下面的规则表就是按这些名字的子串匹配的。
    if (failure.name.empty()) {
        switch (hresult) {
            case 0x80070005u: failure.name = "ERROR_ACCESS_DENIED"; break;
            case 0x80070002u: failure.name = "ERROR_FILE_NOT_FOUND"; break;
            case 0x80070070u: failure.name = "ERROR_DISK_FULL"; break;
            case static_cast<uint32_t>(E_ABORT): failure.name = "E_ABORT"; break;
            default: break;
        }
    }

    const std::string upper = failure.name;
    for (const FailureRule& rule : kFailureRules) {
        if (!upper.empty() && contains_fragment(upper, rule.fragment)) {
            failure.code = rule.code;
            failure.hint = rule.hint;
            return failure;
        }
    }

    failure.hint = failure.name.empty()
        ? ("The deployment stack reported failure " + hresult_hex(hresult) + ". See the log for details.")
        : ("Deployment failed with " + failure.name + " (" + hresult_hex(hresult) + "). See the log for details.");
    return failure;
}

// ---------- 系统访问层 ----------

std::vector<RegisteredPackage> find_registered_packages(std::string_view family_name) {
    ensure_apartment();
    WinRtPackageManager manager;

    auto found = family_name.empty()
        ? manager.FindPackagesForUser(winrt::hstring{})
        : manager.FindPackagesForUser(winrt::hstring{}, winrt::to_hstring(family_name));

    std::vector<RegisteredPackage> packages;
    for (const WinRtPackage& package : found) {
        try {
            RegisteredPackage entry;
            const auto id = package.Id();
            entry.name = winrt::to_string(id.Name());
            entry.full_name = winrt::to_string(id.FullName());
            entry.family_name = winrt::to_string(id.FamilyName());
            entry.version = format_version(id.Version());
            entry.architecture = architecture_name(id.Architecture());
            entry.publisher = winrt::to_string(id.Publisher());
            entry.publisher_id = winrt::to_string(id.PublisherId());
            entry.is_framework = package.IsFramework();
            entry.development_mode = package.IsDevelopmentMode();
            entry.status = status_name(package.Status());
            try {
                entry.installed_path = winrt::to_string(package.InstalledPath());
            } catch (const winrt::hresult_error&) {
                // IPackage8 在旧系统上不存在；安装路径只是展示信息，缺了不影响判定。
            }

            packages.push_back(std::move(entry));
        } catch (const winrt::hresult_error&) {
            // 仓库里可能残留读不出属性的坏包；跳过它，不让 list / verify / repair 整体失败。
        }
    }

    return packages;
}

bool is_package_registered(std::string_view family_name) {
    ensure_apartment();
    WinRtPackageManager manager;
    WinRtPackage package{nullptr};
    return first_package(
        manager.FindPackagesForUser(winrt::hstring{}, winrt::to_hstring(family_name)), package);
}

InstallResult install_packages(const InstallRequest& request, const DeploymentProgress& progress) {
    if (request.main_package.empty()) {
        throw Error(codes::IoError, "No main package was supplied for installation.");
    }

    if (!std::filesystem::is_regular_file(request.main_package)) {
        throw Error(
            codes::FileMissing,
            "The main package to install does not exist.",
            narrow_utf8(request.main_package.wstring()));
    }

    for (const std::filesystem::path& dependency : request.dependencies) {
        if (!std::filesystem::is_regular_file(dependency)) {
            throw Error(
                codes::FileMissing,
                "A dependency package does not exist.",
                narrow_utf8(dependency.wstring()));
        }
    }

    ensure_apartment();
    WinRtPackageManager manager;

    // 依赖先装：框架包是共享的，装失败就让整个安装失败，但**不回滚已装的依赖**
    // （回滚共享的框架包会破坏别的应用）。
    const uint32_t dependency_span =
        request.dependencies.empty() ? 0u : static_cast<uint32_t>(40 / request.dependencies.size());
    for (size_t index = 0; index < request.dependencies.size(); ++index) {
        const uint32_t base = 10 + static_cast<uint32_t>(index) * dependency_span;
        report(progress, base,
               L"Installing dependency package " + request.dependencies[index].filename().wstring());
        add_one_package(manager, request.dependencies[index], request, progress, base, dependency_span);
    }

    report(progress, 55, L"Installing the main package");
    add_one_package(manager, request.main_package, request, progress, 55, 40);

    InstallResult result;
    RegisteredPackage installed;
    if (lookup_installed_identity(request, installed)) {
        result.full_name = installed.full_name;
        result.family_name = installed.family_name;
        result.version = installed.version;
        result.architecture = installed.architecture;
    } else if (!request.identity_name.empty()) {
        // 装是装上了，只是没在仓库里认出它。用清单里的身份推一个包族名，
        // 调用方至少还能用它做卸载与引用计数（并把这个事实写进日志）。
        result.identity_lookup_failed = true;
        result.family_name = package_family_name(request.identity_name, request.identity_publisher);
        result.version = request.identity_version;
        result.architecture = request.identity_architecture;
    }

    if (request.all_users) {
        if (result.family_name.empty()) {
            throw Error(
                deployment_codes::AllUsersNotPermitted,
                "The package cannot be provisioned for all users without a package family name.");
        }

        provision_package_for_all_users(result.family_name, progress);
        result.provisioned = true;
    }

    report(progress, 100, L"Installed");
    return result;
}

UninstallResult uninstall_package(const UninstallRequest& request, const DeploymentProgress& progress) {
    ensure_apartment();
    WinRtPackageManager manager;

    UninstallResult result;
    result.family_name = request.family_name;
    result.full_name = request.full_name;

    if (result.full_name.empty()) {
        if (result.family_name.empty()) {
            throw Error(codes::IoError, "Either a package family name or a full name is required to uninstall.");
        }

        auto found = manager.FindPackagesForUser(winrt::hstring{}, winrt::to_hstring(result.family_name));
        WinRtPackage package{nullptr};
        if (first_package(found, package)) {
            result.full_name = winrt::to_string(package.Id().FullName());
        }

        if (result.full_name.empty()) {
            // 找不到就是已经卸载了；这不是错误（卸载要幂等）。
            report(progress, 100, L"Nothing to remove");
            return result;
        }
    }

    report(progress, 10, L"Removing " + widen_utf8(result.full_name));

    auto operation = manager.RemovePackageAsync(
        winrt::to_hstring(result.full_name),
        winrt::Windows::Management::Deployment::RemovalOptions::None);

    const DeploymentProgress sink = progress;
    operation.Progress([sink](auto const&, const WinRtDeploymentProgress& state) {
        if (sink) {
            sink(10 + (state.percentage * 80) / 100, L"Removing the package");
        }
    });

    try {
        const WinRtDeploymentResult deployed = operation.get();
        const int32_t extended = deployed.ExtendedErrorCode().value;
        if (extended != 0) {
            throw_deployment_failure(
                static_cast<uint32_t>(extended), winrt::to_string(deployed.ErrorText()));
        }
    } catch (const winrt::hresult_error& error) {
        throw_deployment_failure(
            static_cast<uint32_t>(error.code().value), winrt::to_string(error.message()));
    }

    result.removed = true;
    report(progress, 100, L"Removed");
    return result;
}

void provision_package_for_all_users(std::string_view family_name, const DeploymentProgress& progress) {
    if (family_name.empty()) {
        throw Error(deployment_codes::AllUsersNotPermitted, "A package family name is required for provisioning.");
    }

    ensure_apartment();
    WinRtPackageManager manager;
    report(progress, 90, L"Provisioning for all users");
    const WinRtDeploymentResult result = manager.ProvisionPackageForAllUsersAsync(
        winrt::to_hstring(family_name)).get();
    const int32_t extended = result.ExtendedErrorCode().value;
    if (extended != 0) {
        throw_deployment_failure(static_cast<uint32_t>(extended), winrt::to_string(result.ErrorText()));
    }
}

void deprovision_package_for_all_users(std::string_view family_name) {
    if (family_name.empty()) {
        throw Error(deployment_codes::AllUsersNotPermitted, "A package family name is required for deprovisioning.");
    }

    ensure_apartment();
    WinRtPackageManager manager;
    manager.DeprovisionPackageForAllUsersAsync(winrt::to_hstring(family_name)).get();
}

void activate_application(std::string_view aumid) {
    if (aumid.empty()) {
        throw Error(deployment_codes::Failed, "An application user model ID is required to start an application.");
    }

    // 走 shell 的 AppsFolder 命名空间：不需要额外的 COM 接口，
    // 且应用会在用户自己的 shell 上下文里启动。
    const std::wstring target = L"shell:AppsFolder\\" + widen_utf8(aumid);
    const HINSTANCE status = ::ShellExecuteW(nullptr, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    const auto code = reinterpret_cast<INT_PTR>(status);
    if (code <= 32) {
        throw Error(
            deployment_codes::Failed,
            "The application could not be started.",
            "ShellExecuteW returned " + std::to_string(code));
    }
}

}  // namespace msixpkg
