#include "installer.h"

#include <Windows.h>
#include <shellapi.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "arp.h"
#include "certstore.h"
#include "container.h"
#include "crypto.h"
#include "internal.h"
#include "json.h"
#include "log.h"
#include "msixpkg/errors.h"
#include "policy.h"
#include "verify.h"

namespace msixpkg {
namespace {

/// GetVersionEx 在没有相应 manifest 声明时会撒谎；RtlGetVersion 说真话。
/// 这里自己声明结构，避免为一个字段去拉 winternl.h。
struct OsVersionInfo {
    ULONG size;
    ULONG major;
    ULONG minor;
    ULONG build;
    ULONG platform_id;
    wchar_t service_pack[128];
};

std::filesystem::path current_executable_path() {
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD written =
            ::GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (written == 0) {
            throw Error(codes::IoError, "Unable to determine the installer executable path.");
        }

        if (written < buffer.size()) {
            buffer.resize(written);
            return std::filesystem::path(buffer);
        }

        buffer.resize(buffer.size() * 2);
    }
}

std::filesystem::path file_name_of(std::string_view entry_name) {
    const size_t slash = entry_name.find_last_of('/');
    const std::string_view name = slash == std::string_view::npos ? entry_name : entry_name.substr(slash + 1);
    return widen_utf8(name);
}

void extract_entry_to_file(const Container& source, std::string_view entry_name, const std::filesystem::path& target) {
    const zip::Entry* entry = source.archive().find(entry_name);
    if (entry == nullptr) {
        throw Error(codes::FileMissing, "A declared payload entry is missing from the container.", std::string(entry_name));
    }

    // 上限取清单声明的大小 + 1：既不让解压炸弹得逞，也不因为默认 64 MB 上限拒绝大包。
    const uint64_t declared = source.manifest().main_package.path == entry_name
        ? source.manifest().main_package.size
        : [&]() {
              for (const FileRef& dependency : source.manifest().dependencies) {
                  if (dependency.path == entry_name) {
                      return dependency.size;
                  }
              }

              return entry->uncompressed_size;
          }();

    const Bytes bytes = source.read_entry(entry_name, declared + 1);
    write_file(target, bytes);
}

/// 从容器里取发布者证书的 DER 字节。
Bytes publisher_certificate_bytes(const Container& source) {
    if (!source.manifest().has_publisher) {
        return {};
    }

    return source.read_entry(source.manifest().publisher.cert_path);
}

std::string display_name_of(const Manifest& manifest) {
    if (manifest.has_display && !manifest.display.name.empty()) {
        return manifest.display.name;
    }

    return manifest.identity.name;
}

std::string first_error(const Report& report) {
    if (!report.errors.empty()) {
        const Issue& issue = report.errors.front();
        return issue.to_string();
    }

    return "unknown validation failure";
}

[[noreturn]] void throw_report(const Report& report) {
    const Issue& issue = report.errors.front();
    throw Error(issue.code, issue.message, issue.detail);
}

// ---------- 提权助手 ----------

enum class ElevationOutcome {
    Ran,
    Cancelled,
    Failed,
};

ElevationOutcome run_elevated(
    const std::wstring& arguments,
    DWORD& exit_code,
    std::string& error) {
    const std::filesystem::path executable = current_executable_path();
    // lpDirectory 必须指向一个活到 ShellExecuteExW 返回之后的对象：这里不能直接写
    // executable.parent_path().c_str()（临时对象在语句结束时就没了）。
    const std::filesystem::path directory = executable.parent_path();

    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    info.lpVerb = L"runas";
    info.lpFile = executable.c_str();
    info.lpParameters = arguments.c_str();
    info.lpDirectory = directory.c_str();
    // 提权助手一律隐藏：否则 UAC 之后还会闪一个黑框。
    info.nShow = SW_HIDE;

    if (!::ShellExecuteExW(&info)) {
        const DWORD last = ::GetLastError();
        if (last == ERROR_CANCELLED) {
            return ElevationOutcome::Cancelled;
        }

        error = "ShellExecuteEx failed: Win32 error " + std::to_string(last);
        return ElevationOutcome::Failed;
    }

    ::WaitForSingleObject(info.hProcess, INFINITE);
    DWORD code = 1;
    ::GetExitCodeProcess(info.hProcess, &code);
    ::CloseHandle(info.hProcess);
    exit_code = code;
    return ElevationOutcome::Ran;
}

struct HelperResultFile {
    bool added = false;
    bool existed_before = false;
    std::string sha1;
    std::string sha256;
    std::string subject;
};

std::string helper_result_json(const CertificateHelperResult& result) {
    json::Value root = json::Value::make_object();
    root.set("added", json::Value::make_bool(result.added));
    root.set("existedBefore", json::Value::make_bool(result.existed_before));
    root.set("sha1", json::Value::make_string(result.sha1));
    root.set("sha256", json::Value::make_string(result.sha256));
    root.set("subject", json::Value::make_string(result.subject));
    return root.to_json();
}

HelperResultFile read_helper_result(const std::filesystem::path& path) {
    const std::optional<std::string> text = read_text_file(path);
    if (!text.has_value()) {
        throw Error(
            codes::IoError,
            "The elevated helper did not report a result.",
            narrow_utf8(path.wstring()));
    }

    const json::Value root = json::parse(*text);
    HelperResultFile result;
    if (const json::Value* value = root.find("added"); value != nullptr && value->kind == json::Value::Kind::Boolean) {
        result.added = value->boolean;
    }

    if (const json::Value* value = root.find("existedBefore"); value != nullptr && value->kind == json::Value::Kind::Boolean) {
        result.existed_before = value->boolean;
    }

    if (const json::Value* value = root.find("sha1"); value != nullptr && value->kind == json::Value::Kind::String) {
        result.sha1 = value->text;
    }

    if (const json::Value* value = root.find("sha256"); value != nullptr && value->kind == json::Value::Kind::String) {
        result.sha256 = value->text;
    }

    if (const json::Value* value = root.find("subject"); value != nullptr && value->kind == json::Value::Kind::String) {
        result.subject = value->text;
    }

    return result;
}

Bytes sha1_bytes_from_hex(const std::string& hex) {
    Bytes bytes;
    if (!try_from_hex(hex, bytes) || bytes.size() != 20) {
        throw Error(
            codes::CertificateMalformed,
            "The certificate SHA-1 thumbprint is not 40 hexadecimal characters.",
            hex);
    }

    return bytes;
}

std::string dependency_label(const FileRef& dependency) {
    if (dependency.name.empty()) {
        return dependency.path;
    }

    return dependency.name + " " + dependency.version;
}

uint64_t total_payload_size(const Manifest& manifest) {
    uint64_t total = manifest.main_package.size;
    for (const FileRef& dependency : manifest.dependencies) {
        total += dependency.size;
    }

    return total;
}

/// 把提权助手的结果（raw 文件）转成内部结构。
HelperResultFile run_certificate_helper(
    bool add,
    const std::filesystem::path& container,
    const std::string& sha1,
    const std::filesystem::path& result_path,
    const std::filesystem::path& log_path,
    bool& elevation_declined,
    const Log& log) {
    std::wstring arguments = add ? L"cert-add" : L"cert-remove";
    if (add) {
        arguments += L" --container " + quote_argument(container.wstring());
    }

    arguments += L" --thumbprint-sha1 " + quote_argument(widen_utf8(sha1));
    if (add) {
        arguments += L" --result " + quote_argument(result_path.wstring());
    }

    if (!log_path.empty()) {
        arguments += L" --log " + quote_argument(log_path.wstring());
    }

    log.line("elevating: msixpkg-native " + narrow_utf8(arguments));

    DWORD exit_code = 1;
    std::string error;
    const ElevationOutcome outcome = run_elevated(arguments, exit_code, error);
    if (outcome == ElevationOutcome::Cancelled) {
        elevation_declined = true;
        throw Error(
            deployment_codes::Cancelled,
            "Elevation was declined, so the publisher certificate was not changed.");
    }

    if (outcome == ElevationOutcome::Failed) {
        throw Error(deployment_codes::AccessDenied, "The elevated helper could not be started.", error);
    }

    if (exit_code != 0) {
        throw Error(
            deployment_codes::Failed,
            add ? "The elevated certificate helper failed." : "The elevated certificate removal failed.",
            "exit code " + std::to_string(exit_code));
    }

    if (!add) {
        return {};
    }

    const HelperResultFile result = read_helper_result(result_path);
    std::error_code ignored;
    std::filesystem::remove(result_path, ignored);
    return result;
}

}  // namespace

// ---------- 提权助手的实现（由已提权进程调用） ----------

CertificateHelperResult certificate_add_from_container(
    const std::filesystem::path& container,
    const std::string& expected_sha1,
    const std::string& result_path) {
    // 自己重新校验：提权进程只认容器里真实写着的东西，不认父进程传进来的断言。
    ValidateOptions options;
    options.installer_version = kInstallerVersion;
    options.host_build = Installer::host_build_number();
    options.host_architecture = Installer::host_architecture();
    options.allowed_publisher_thumbprints = {expected_sha1};

    const Report report = validate(container, options);
    if (!report.has_publisher_certificate) {
        throw Error(
            codes::CertificateMissing,
            "The container has no embedded publisher certificate to import.");
    }

    const std::string actual_sha1 = report.publisher_certificate.sha1_thumbprint();
    if (!equals_ignore_case_ascii(actual_sha1, expected_sha1)) {
        throw Error(
            codes::SignatureThumbprintMismatch,
            "The publisher certificate in the container does not match the requested thumbprint.",
            "expected " + expected_sha1 + ", actual " + actual_sha1);
    }

    CertificateHelperResult result;
    result.sha1 = actual_sha1;
    result.sha256 = report.publisher_certificate.sha256_thumbprint();
    result.subject = report.publisher_certificate.subject_string();

    const std::filesystem::path container_path = std::filesystem::absolute(container);
    const Container source = Container::open(container_path);
    const Bytes der = publisher_certificate_bytes(source);
    const Bytes sha1 = sha1_bytes_from_hex(actual_sha1);

    CertificateStore store = CertificateStore::open_machine_trusted_people(false);
    const AddResult added = store.add(der, sha1);
    result.added = added.added;
    result.existed_before = added.existed_before;

    if (!result_path.empty()) {
        write_text_atomic(widen_utf8(result_path), helper_result_json(result));
    }

    return result;
}

bool certificate_remove_by_sha1(const std::string& sha1_hex) {
    const Bytes sha1 = sha1_bytes_from_hex(sha1_hex);
    CertificateStore store = CertificateStore::open_machine_trusted_people(false);
    return store.remove(sha1);
}

// ---------- 主机信息 ----------

int64_t Installer::host_build_number() {
    const HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
    if (ntdll == nullptr) {
        return 0;
    }

    using RtlGetVersionFn = LONG(WINAPI*)(OsVersionInfo*);
    const auto function = reinterpret_cast<RtlGetVersionFn>(::GetProcAddress(ntdll, "RtlGetVersion"));
    if (function == nullptr) {
        return 0;
    }

    OsVersionInfo info{};
    info.size = sizeof(info);
    if (function(&info) != 0) {
        return 0;
    }

    return static_cast<int64_t>(info.build);
}

std::string Installer::host_architecture() {
#if defined(_M_ARM64)
    return "arm64";
#elif defined(_M_AMD64)
    return "x64";
#elif defined(_M_IX86)
    return "x86";
#else
    return "unknown";
#endif
}

// ---------- 证书回收判定 ----------

CertificateReclaimDecision Installer::evaluate_certificate_reclaim(
    const StateStore& store,
    const InstallationRecord& record,
    bool policy_allows,
    bool consent_granted) {
    CertificateReclaimDecision decision;
    decision.added_by_installer = record.certificate_added_by_installer;
    decision.existed_before = record.certificate_existed_before;
    decision.policy_allows = policy_allows;
    decision.consent_granted = consent_granted;

    if (record.certificate_sha1.empty()) {
        decision.reason = "no publisher certificate is recorded for this installation";
        return decision;
    }

    // 红线：安装前用户自己就信任过的证书，安装器永远无权删除。
    if (decision.existed_before) {
        decision.reason = "the certificate already existed before this installer ran, so it is never removed";
        return decision;
    }

    if (!decision.added_by_installer) {
        decision.reason = "this installer did not add the certificate";
        return decision;
    }

    // 先做**便宜且确定**的门禁：策略与同意不需要碰系统。
    // 顺序上放在枚举包仓库之前，既省一次 AppXSvc 往返，也让「策略不允许」这类
    // 明确答案不会被「仓库读不出来」的保守分支掩盖掉。
    if (!policy_allows) {
        decision.reason = "the package does not ask for the certificate to be reclaimed on uninstall";
        return decision;
    }

    if (!consent_granted) {
        decision.reason = "certificate reclamation was not approved";
        return decision;
    }

    // 状态库里是否还有别的安装引用同一个指纹（本地文件，也很便宜）。
    for (const InstallationRecord& other : store.find_by_certificate(record.certificate_sha1)) {
        if (other.install_id != record.install_id) {
            decision.other_installations_reference = true;
            break;
        }
    }

    if (decision.other_installations_reference) {
        decision.reason = "another installation recorded by this installer references the same certificate";
        return decision;
    }

    // 最后才问系统：还有别的已注册包用这个发布者签名吗？
    //
    // 仓库读不出来时**保守处理**：判定函数必须是「全函数」——它被 uninstall、verify、repair、
    // GUI 复用，抛异常会让「卸载」这条最需要确定性的路径半途而废。
    // 不知道还有谁在用这张证书时，留着它永远比删掉它安全。
    try {
        for (const RegisteredPackage& package : find_registered_packages()) {
            if (equals_ignore_case_ascii(package.publisher, record.publisher_subject)) {
                decision.other_packages_use_publisher = true;
                break;
            }
        }
    } catch (const Error&) {
        decision.repository_unavailable = true;
    }

    if (decision.repository_unavailable) {
        decision.reason =
            "the package repository could not be enumerated, so it is not safe to remove the certificate";
        return decision;
    }

    if (decision.other_packages_use_publisher) {
        decision.reason = "another registered package is signed by the same publisher";
        return decision;
    }

    decision.remove = true;
    decision.reason = "the certificate was added by this installer and nothing else needs it";
    return decision;
}

// ---------- 只读预演 ----------

InstallPlan Installer::plan(const InstallOptions& options) {
    InstallPlan plan;

    if (options.container.empty()) {
        plan.failure_code = codes::IoError;
        plan.failure_message = "No container was supplied for installation.";
        return plan;
    }

    plan.container_path = std::filesystem::absolute(options.container);

    // ---- 钉扎白名单：管理员部署的 + 用户之前记住的 + 调用方显式给的 ----
    //
    // 只读预演不能建目录，所以这里用 peek_data_roots()（绝不创建任何东西）。
    // peek 拿到的位置如果还不存在，读白名单自然什么也读不到，这是正确行为。
    const DataRoots roots = peek_data_roots();
    const PublisherPolicy policy = load_publisher_policy(roots);
    plan.warnings = policy.warnings;

    std::vector<std::string> pins = options.allowed_publisher_thumbprints;
    for (const std::string& thumbprint : policy.machine_pinned) {
        pins.push_back(thumbprint);
    }

    if (options.use_remembered_publishers) {
        for (const std::string& thumbprint : policy.remembered) {
            pins.push_back(thumbprint);
        }
    }

    plan.allowed_publisher_thumbprints = pins;

    // ---- 容器校验（只读） ----
    ValidateOptions validate_options;
    validate_options.installer_version = kInstallerVersion;
    validate_options.host_build = host_build_number();
    validate_options.host_architecture = host_architecture();
    validate_options.allowed_publisher_thumbprints = pins;
    validate_options.require_pinned_publisher = options.require_pinned_publisher;
    validate_options.require_signature = true;
    validate_options.validate_inner_appx = true;

    const Report report = validate(plan.container_path, validate_options);
    if (!report.valid()) {
        const Issue& issue = report.errors.front();
        plan.failure_code = issue.code;
        plan.failure_message = issue.message;
        plan.failure_detail = issue.detail;
        return plan;
    }

    if (!report.has_manifest) {
        plan.failure_code = codes::ManifestMissing;
        plan.failure_message = "The container has no manifest.";
        return plan;
    }

    plan.has_manifest = true;
    plan.manifest = report.manifest;
    plan.has_publisher_certificate = report.has_publisher_certificate;
    plan.content_digest = report.content_digest_hex;

    const Manifest& manifest = plan.manifest;
    plan.package_family_name = package_family_name(manifest.identity.name, manifest.identity.publisher);

    if (report.has_publisher_certificate) {
        plan.publisher_subject = report.publisher_certificate.subject_string();
        plan.certificate_sha1 = report.publisher_certificate.sha1_thumbprint();
        plan.certificate_sha256 = report.publisher_certificate.sha256_thumbprint();
    }

    // ---- 钉扎判定 ----
    plan.publisher_pinned = report.publisher_was_pinned;
    if (plan.publisher_pinned && options.use_remembered_publishers) {
        // 区分「管理员/显式批准」与「你自己上次同意过」：同意页的措辞不一样。
        const std::string normalized = normalize_thumbprint(plan.certificate_sha256);
        const bool machine_or_explicit = [&] {
            for (const std::string& thumbprint : options.allowed_publisher_thumbprints) {
                if (normalize_thumbprint(thumbprint) == normalized) {
                    return true;
                }
            }

            for (const std::string& thumbprint : policy.machine_pinned) {
                if (thumbprint == normalized) {
                    return true;
                }
            }

            return false;
        }();

        plan.publisher_remembered = !machine_or_explicit;
    }

    // 证书是否已经在受信任人员存储里（只读打开，不写）。
    if (plan.has_publisher_certificate) {
        try {
            plan.certificate_already_trusted = is_publisher_trusted_hex(plan.certificate_sha1);
        } catch (const Error& error) {
            plan.warnings.push_back("the trust store could not be read: " + error.to_string());
        }
    }

    // 已经装过没有？仓库读不出来时按「未知」处理，不让预演失败。
    try {
        plan.already_installed = is_package_registered(plan.package_family_name);
    } catch (const Error& error) {
        plan.warnings.push_back("the package repository could not be read: " + error.to_string());
    }

    if (options.all_users && !manifest.allow_all_users) {
        plan.failure_code = deployment_codes::AllUsersNotPermitted;
        plan.failure_message = "The package does not allow installing for all users.";
        plan.failure_detail = manifest.identity.name;
        return plan;
    }

    // ---- 同意页需要的全部信息 ----
    ConsentRequest& consent = plan.consent;
    consent.publisher.subject = plan.publisher_subject;
    consent.publisher.thumbprint_sha1 = plan.certificate_sha1;
    consent.publisher.thumbprint_sha256 = plan.certificate_sha256;
    if (report.has_publisher_certificate) {
        consent.publisher.not_before_utc = manifest.publisher.not_before_utc;
        consent.publisher.not_after_utc = manifest.publisher.not_after_utc;
        consent.publisher.self_signed = manifest.publisher.self_signed;
    }

    consent.publisher.already_trusted = plan.certificate_already_trusted;
    consent.publisher.pinned = plan.publisher_pinned;
    consent.package.name = manifest.identity.name;
    consent.package.publisher = manifest.identity.publisher;
    consent.package.version = manifest.identity.version;
    consent.package.architecture = manifest.identity.architecture;
    consent.package.total_size = total_payload_size(manifest);
    for (const FileRef& dependency : manifest.dependencies) {
        consent.package.dependencies.push_back(dependency_label(dependency));
    }

    if (manifest.has_display) {
        consent.package.description = manifest.display.description;
    }

    consent.already_installed = plan.already_installed;
    consent.requires_elevation = !is_process_elevated();
    consent.allow_all_users = manifest.allow_all_users;
    consent.unpinned_publisher = plan.has_publisher_certificate && !plan.publisher_pinned;

    // ---- 动作清单 ----
    InstallRequest deploy_request;
    deploy_request.identity_name = manifest.identity.name;
    deploy_request.identity_publisher = manifest.identity.publisher;
    deploy_request.identity_version = manifest.identity.version;
    deploy_request.identity_architecture = manifest.identity.architecture;
    deploy_request.allow_downgrade = options.allow_downgrade;
    deploy_request.all_users = options.all_users;
    plan.actions = plan_install(deploy_request);

    plan.valid = true;
    return plan;
}

// ---------- 安装 ----------

InstallOutcome Installer::install(const InstallOptions& options) {
    const InstallPlan plan = this->plan(options);

    InstallOutcome outcome;
    if (!plan.valid) {
        // 校验失败是**唯一**在动任何东西之前就要建日志的场景：否则用户拿不到
        // 「为什么被拒」的可查记录。dry-run 成功路径一个文件都不建。
        const DataRoots roots = resolve_data_roots();
        Log log = Log::open(roots);
        log.line("validation failed: " + plan.failure_code + ": " + plan.failure_message);
        throw Error(plan.failure_code, plan.failure_message, plan.failure_detail);
    }

    outcome.plan = plan.actions;
    outcome.identity_name = plan.manifest.identity.name;
    outcome.family_name = plan.package_family_name;
    outcome.version = plan.manifest.identity.version;
    outcome.publisher_subject = plan.publisher_subject;
    outcome.certificate_sha1 = plan.certificate_sha1;

    if (options.dry_run) {
        // **零副作用**：到这里为止只读过容器与注册表，没有建目录、没有写日志、
        // 没有碰证书存储 —— 摘要页与 `--dry-run` 都靠这条性质。
        outcome.dry_run = true;
        return outcome;
    }

    const std::filesystem::path container_path = plan.container_path;
    DataRoots roots = resolve_data_roots();
    StateStore store(roots);
    store.ensure_directories();

    Log log = Log::open(roots);
    if (!roots.warning.empty()) {
        log.line("warning: " + roots.warning);
    }

    for (const std::string& warning : plan.warnings) {
        log.line("warning: " + warning);
    }

    // 机器级数据目录尽量收紧为 Administrators + SYSTEM；拿不到就记下来继续 ——
    // 「装给当前用户」本来就不该要求管理员。
    if (roots.machine_wide) {
        std::string acl_error;
        if (restrict_directory_to_administrators(roots.root, acl_error)) {
            log.line("tightened the ACL of " + narrow_utf8(roots.root.wstring()) + " to Administrators + SYSTEM");
        } else {
            log.line("warning: could not tighten the data directory ACL: " + acl_error);
        }
    }

    InstallationRecord record;
    record.install_id = new_install_id();
    record.container_path = narrow_utf8(container_path.wstring());
    record.started_utc = utc_now();
    record.log_path = log.path_utf8();

    outcome.install_id = record.install_id;
    outcome.log_path = log.path_utf8();
    outcome.data_root = narrow_utf8(roots.root.wstring());
    outcome.warning = roots.warning;

    const bool has_certificate = plan.has_publisher_certificate;
    const bool pinned = plan.publisher_pinned;
    const bool unpinned = has_certificate && !pinned;
    const bool already_trusted = plan.certificate_already_trusted;

    std::filesystem::path staging;
    bool registered = false;

    try {
        // ---- 校验结果已经在 plan() 里拿到，这里只补记账与日志 ----
        progress_.OnPhase(Phase::Validate, L"Validating the container");
        log.step("validate", record.container_path);

        const Manifest& manifest = plan.manifest;
        record.content_digest = plan.content_digest;
        record.identity_name = manifest.identity.name;
        record.identity_publisher = manifest.identity.publisher;
        record.identity_version = manifest.identity.version;
        record.identity_architecture = manifest.identity.architecture;
        record.package_family_name = plan.package_family_name;
        record.uninstall_remove_certificate = manifest.uninstall_remove_certificate;
        record.dependencies.reserve(manifest.dependencies.size());
        record.payload_sha256.push_back(manifest.main_package.sha256);
        for (const FileRef& dependency : manifest.dependencies) {
            record.dependencies.push_back(dependency_label(dependency));
            record.payload_sha256.push_back(dependency.sha256);
        }

        if (has_certificate) {
            record.publisher_subject = plan.publisher_subject;
            record.certificate_sha1 = plan.certificate_sha1;
            record.certificate_sha256 = plan.certificate_sha256;
            log.fingerprint("publisher", record.publisher_subject);
            log.fingerprint("certificate SHA-1", record.certificate_sha1);
            log.fingerprint("certificate SHA-256", record.certificate_sha256);
            log.line(std::string("pinning: ") + (pinned ? (plan.publisher_remembered ? "allowed (remembered by the user on an earlier run)" : "allowed (whitelisted)")
                                                       : "NOT pinned -> explicit consent required"));
        }

        log.fingerprint("content digest", record.content_digest);

        InstallRequest deploy_request;
        deploy_request.identity_name = record.identity_name;
        deploy_request.identity_publisher = record.identity_publisher;
        deploy_request.identity_version = record.identity_version;
        deploy_request.identity_architecture = record.identity_architecture;
        deploy_request.allow_downgrade = options.allow_downgrade;
        deploy_request.all_users = options.all_users;

        progress_.OnPhase(Phase::Consent, L"Waiting for user consent");
        const ConsentDecision decision = consent_.Ask(plan.consent);
        if (!decision.approved) {
            log.line("the user declined the installation");
            outcome.cancelled = true;
            return outcome;
        }

        if (unpinned && !decision.remember_publisher) {
            // 红线 2：未命中钉扎白名单时，只有「明确勾选信任此发布者」才继续。
            log.line("refused: the publisher is not pinned and was not explicitly trusted");
            throw Error(
                codes::NotPinned,
                "The publisher is not in the pinned list and was not explicitly trusted.",
                record.certificate_sha256);
        }

        if (unpinned) {
            // 审计 + **真的记住**。以前这里只写日志，「将记住」是一句空话：
            // 用户下次安装会被再问一遍，而复选框却在暗示不用再问。
            log.line("audit: the user chose to trust the unpinned publisher " + record.certificate_sha256);
            const std::filesystem::path remembered = remembered_publishers_path(roots);
            if (remember_publisher(roots, record.certificate_sha256, record.publisher_subject)) {
                log.line("audit: remembered the publisher in " + narrow_utf8(remembered.wstring()));
            } else {
                log.line("warning: the publisher could not be remembered; the user will be asked again");
            }
        }

        const bool all_users = decision.all_users && manifest.allow_all_users;
        deploy_request.all_users = all_users;

        if (cancel_.IsCancelled()) {
            outcome.cancelled = true;
            log.line("cancelled before staging");
            return outcome;
        }

        // ---- 解包到受保护暂存目录 ----
        progress_.OnPhase(Phase::Stage, L"Extracting the payload");
        staging = create_staging_directory(roots);
        log.step("stage", narrow_utf8(staging.wstring()));

        const Container source = Container::open(container_path);
        const std::filesystem::path main_package = staging / file_name_of(manifest.main_package.path);
        extract_entry_to_file(source, manifest.main_package.path, main_package);

        std::vector<std::filesystem::path> dependencies;
        dependencies.reserve(manifest.dependencies.size());
        for (const FileRef& dependency : manifest.dependencies) {
            const std::filesystem::path path = staging / file_name_of(dependency.path);
            extract_entry_to_file(source, dependency.path, path);
            dependencies.push_back(path);
        }

        // TOCTOU 复检：部署前用磁盘上的那份主包再校验一次大小与 SHA-256。
        // 白名单必须与 plan() 用的那份完全一致（否则这里会因为「未钉扎」而假报警）。
        ValidateOptions recheck_options;
        recheck_options.installer_version = kInstallerVersion;
        recheck_options.host_build = host_build_number();
        recheck_options.host_architecture = host_architecture();
        recheck_options.allowed_publisher_thumbprints = plan.allowed_publisher_thumbprints;
        recheck_options.require_pinned_publisher = options.require_pinned_publisher;
        recheck_options.require_signature = true;
        recheck_options.validate_inner_appx = true;
        recheck_options.extracted_main_package_path = narrow_utf8(main_package.wstring());

        const Report rechecked = validate(container_path, recheck_options);
        if (!rechecked.valid()) {
            log.line("post-extraction recheck failed: " + first_error(rechecked));
            throw_report(rechecked);
        }

        // ---- 证书 ----
        bool certificate_trusted = false;
        if (!has_certificate) {
            // 容器里没有证书：完全不碰证书存储（红线 1）。
            log.line("the container has no publisher certificate; the trust store is left untouched");
        } else if (already_trusted) {
            record.certificate_existed_before = true;
            record.certificate_added_by_installer = false;
            certificate_trusted = true;
            log.line("the publisher certificate is already trusted; it will never be removed by us");
        } else {
            progress_.OnPhase(Phase::Certificate, L"Importing the publisher certificate");
            const Bytes der = publisher_certificate_bytes(source);
            const Bytes sha1 = sha1_bytes_from_hex(record.certificate_sha1);

            if (is_process_elevated()) {
                CertificateStore certificate_store = CertificateStore::open_machine_trusted_people(false);
                const AddResult added = certificate_store.add(der, sha1);
                record.certificate_added_by_installer = added.added;
                record.certificate_existed_before = added.existed_before;
                log.step("certificate", added.added ? "imported (in-process, already elevated)" : "already present");
            } else if (options.allow_elevation) {
                progress_.OnWaitingForElevation();
                const std::filesystem::path result_path = staging / "cert-result.json";
                bool declined = false;
                const HelperResultFile helper = run_certificate_helper(
                    true,
                    container_path,
                    record.certificate_sha1,
                    result_path,
                    log.path(),
                    declined,
                    log);
                if (declined) {
                    outcome.elevation_declined = true;
                }

                record.certificate_added_by_installer = helper.added;
                record.certificate_existed_before = helper.existed_before;
                log.step("certificate", helper.added ? "imported (elevated helper)" : "already present");
            } else {
                throw Error(
                    deployment_codes::AccessDenied,
                    "The publisher certificate must be imported, but elevation is not allowed.",
                    record.certificate_sha1);
            }

            certificate_trusted = true;
        }

        if (cancel_.IsCancelled()) {
            throw Error(deployment_codes::Cancelled, "The installation was cancelled.");
        }

        // ---- 依赖包与主包 ----
        progress_.OnPhase(Phase::Dependencies, L"Installing dependencies");
        deploy_request.main_package = main_package;
        deploy_request.dependencies = dependencies;
        deploy_request.force_target_app_shutdown = true;

        const DeploymentProgress sink = [this](uint32_t percent, const std::wstring& message) {
            progress_.OnProgress(percent);
            (void)message;
        };

        const InstallResult installed = install_packages(deploy_request, sink);
        registered = true;

        record.package_full_name = installed.full_name;
        if (!installed.family_name.empty()) {
            record.package_family_name = installed.family_name;
        }

        record.all_users = all_users;
        record.provisioned = installed.provisioned;
        if (installed.identity_lookup_failed) {
            log.line("warning: the installed package identity could not be read back from the repository");
        }

        outcome.full_name = record.package_full_name;
        outcome.family_name = record.package_family_name;
        outcome.certificate_trusted = certificate_trusted;
        outcome.certificate_added_by_installer = record.certificate_added_by_installer;
        outcome.certificate_existed_before = record.certificate_existed_before;

        // ---- ARP 卸载入口（设置里能看到我们的卸载器） ----
        progress_.OnPhase(Phase::Done, L"Finalizing");
        try {
            const ArpEntry entry = make_arp_entry(
                record.identity_name,
                display_name_of(manifest),
                record.identity_version,
                record.identity_publisher,
                narrow_utf8(current_executable_path().wstring()),
                record.package_family_name,
                narrow_utf8(staging.wstring()));
            record.arp_hive = write_arp_entry(entry);
            record.arp_entry_created = true;
            outcome.arp_created = true;
            outcome.arp_hive = record.arp_hive;
            log.step("arp", "uninstall entry created in " + record.arp_hive);
        } catch (const Error& error) {
            // 没有 ARP 条目只是「用户得用命令行卸载」，不该让已经装好的包回滚。
            log.line("warning: the uninstall entry could not be created: " + error.to_string());
        }

        record.result = "installed";
        record.finished_utc = utc_now();
        store.save(record);

        if (!options.keep_staging) {
            remove_tree_if_present(staging);
        }

        outcome.installed = true;
        progress_.OnProgress(100);
        progress_.OnPhase(Phase::Done, L"Installed");
        log.step("install", "completed");
        return outcome;
    } catch (const Error& error) {
        log.failure(error);
        progress_.OnPhase(Phase::Rollback, L"Rolling back");
        record.failure_code = error.code();
        record.failure_detail = error.detail();
        record.finished_utc = utc_now();

        if (!registered) {
            // 包还没注册：本次创建的东西要删干净。
            if (record.certificate_added_by_installer && !record.certificate_sha1.empty()) {
                try {
                    if (is_process_elevated()) {
                        CertificateStore certificate_store = CertificateStore::open_machine_trusted_people(false);
                        certificate_store.remove(sha1_bytes_from_hex(record.certificate_sha1));
                    } else if (options.allow_elevation) {
                        bool declined = false;
                        run_certificate_helper(
                            false, {}, record.certificate_sha1, {}, log.path(), declined, log);
                    }

                    record.certificate_removed = true;
                    log.step("rollback", "removed the certificate that this run had added");
                } catch (const Error& rollback_error) {
                    log.line("warning: rollback could not remove the certificate: " + rollback_error.to_string());
                }
            }

            if (!staging.empty()) {
                remove_tree_if_present(staging);
            }
        } else {
            // 包已经注册成功：绝不能回滚证书（那会让刚装好的应用失去信任），
            // 只把「半成功」的事实记下来，交给 repair 收敛。
            record.result = "partial";
            try {
                store.save(record);
            } catch (const Error&) {
                log.line("warning: the partial installation record could not be saved");
            }
        }

        progress_.OnPhase(Phase::Failed, widen_utf8(error.code()));
        throw;
    }
}

// ---------- 卸载 ----------

UninstallOutcome Installer::uninstall(const UninstallOptions& options) {
    DataRoots roots = resolve_data_roots();
    StateStore store(roots);
    store.ensure_directories();

    Log log = Log::open(roots);
    if (!roots.warning.empty()) {
        log.line("warning: " + roots.warning);
    }

    UninstallOutcome outcome;
    outcome.log_path = log.path_utf8();
    outcome.data_root = narrow_utf8(roots.root.wstring());
    outcome.warning = roots.warning;

    // 1) 读状态库
    std::vector<InstallationRecord> records;
    if (!options.install_id.empty()) {
        const std::optional<InstallationRecord> loaded = store.load(options.install_id);
        if (loaded.has_value()) {
            records.push_back(*loaded);
        }
    }

    if (records.empty() && !options.family_name.empty()) {
        records = store.find_by_family_name(options.family_name);
    }

    InstallationRecord record;
    bool have_record = false;
    if (!records.empty()) {
        record = records.front();
        have_record = true;
    } else {
        // 兜底：状态库丢了（用户手工删了 / 换了机器）也要能卸载。
        record.install_id.clear();
        record.package_family_name = options.family_name;
        record.certificate_sha1 = options.certificate_sha1;
        if (!options.certificate_sha1.empty()) {
            // 显式给出指纹是操作员的明确请求：把它当作「本次新增」的断言，
            // 但仍然要过「没有别的包在用这个发布者」等判定。
            record.certificate_added_by_installer = true;
            record.uninstall_remove_certificate = true;
            log.line("warning: no state record was found; trusting the thumbprint supplied on the command line");
        }
    }

    outcome.family_name = record.package_family_name.empty() ? options.family_name : record.package_family_name;
    outcome.full_name = record.package_full_name;

    UninstallRequest plan_request;
    plan_request.family_name = outcome.family_name;
    plan_request.full_name = record.package_full_name;
    outcome.plan = plan_uninstall(plan_request);

    if (options.dry_run) {
        outcome.dry_run = true;
        return outcome;
    }

    // 2)/3) 先卸应用（顺序是安全语义的一半）
    log.step("uninstall", "family=" + outcome.family_name + " full=" + outcome.full_name);

    if (!outcome.family_name.empty() || !outcome.full_name.empty()) {
        const UninstallResult removed = uninstall_package(plan_request, {});
        outcome.removed = removed.removed;
        if (!removed.full_name.empty()) {
            outcome.full_name = removed.full_name;
            record.package_full_name = removed.full_name;
        }

        log.step("remove", removed.removed ? "removed " + outcome.full_name : "nothing to remove");
    }

    // 4) 证书回收判定
    progress_.OnPhase(Phase::Reclaim, L"Deciding whether to reclaim the certificate");
    const bool policy_allows = record.uninstall_remove_certificate;
    outcome.decision = evaluate_certificate_reclaim(store, record, policy_allows, options.allow_certificate_removal);
    log.step("certificate", outcome.decision.reason);

    if (outcome.decision.remove && !record.certificate_sha1.empty()) {
        try {
            bool removed_now = false;
            if (is_process_elevated()) {
                CertificateStore certificate_store = CertificateStore::open_machine_trusted_people(false);
                removed_now = certificate_store.remove(sha1_bytes_from_hex(record.certificate_sha1));
            } else if (options.allow_elevation) {
                progress_.OnWaitingForElevation();
                bool declined = false;
                run_certificate_helper(
                    false, {}, record.certificate_sha1, {}, log.path(), declined, log);
                removed_now = true;
            } else {
                log.line("warning: the certificate should be reclaimed but elevation is not allowed");
            }

            outcome.certificate_removed = removed_now;
            record.certificate_removed = removed_now;
            log.step("certificate", removed_now ? "reclaimed" : "not present");
        } catch (const Error& error) {
            // 证书删不掉不该阻止用户卸载应用：只记录，交给 verify / repair。
            log.line("warning: the certificate could not be reclaimed: " + error.to_string());
        }
    }

    // 5) 清理 ARP、状态记录、暂存
    if (record.arp_entry_created || !record.identity_name.empty()) {
        const std::string key = arp_key_name(record.identity_name);
        if (remove_arp_entry(key)) {
            log.step("arp", "uninstall entry removed");
        }
    }

    if (have_record && !record.install_id.empty()) {
        store.remove(record.install_id);
    }

    if (!record.install_id.empty()) {
        remove_tree_if_present(roots.staging / widen_utf8(record.install_id));
    }

    progress_.OnProgress(100);
    progress_.OnPhase(Phase::Done, L"Uninstalled");
    log.step("uninstall", "completed");
    return outcome;
}

}  // namespace msixpkg
