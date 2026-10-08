// msixpkg-native：原生安装器的命令行前端（也是一种前端实现，见 interface.h）。
//
// 子命令：install / uninstall / list / verify / repair / clean
//         cert-add / cert-remove（提权助手，只由主进程用 runas 拉起）
//
// 退出码（CI 与 E2E 脚本依赖，不要随意改）：
//   0 成功 / 1 未预期错误 / 2 容器格式或校验失败 / 3 用法错误 / 4 I/O 错误
//   5 部署失败 / 6 被取消（含用户拒绝提权）/ 7 前置条件不满足

#include <Windows.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include "arp.h"
#include "certstore.h"
#include "deployment.h"
#include "installer.h"
#include "interface.h"
#include "internal.h"
#include "json.h"
#include "log.h"
#include "ops.h"
#include "state.h"
#include "msixpkg/errors.h"

namespace {

namespace exit_codes {
constexpr int kSuccess = 0;
constexpr int kUnexpected = 1;
constexpr int kContainerInvalid = 2;
constexpr int kUsage = 3;
constexpr int kIoError = 4;
constexpr int kDeploymentFailed = 5;
constexpr int kCancelled = 6;
constexpr int kPrecondition = 7;
}  // namespace exit_codes

struct UsageError final : std::runtime_error {
    explicit UsageError(const std::string& message) : std::runtime_error(message) {}
};

int exit_code_for(const std::string& code) {
    if (code == msixpkg::codes::IoError) {
        return exit_codes::kIoError;
    }

    if (code == msixpkg::deployment_codes::Cancelled) {
        return exit_codes::kCancelled;
    }

    if (code == msixpkg::deployment_codes::AllUsersNotPermitted) {
        return exit_codes::kPrecondition;
    }

    if (code.rfind("E_DEPLOY_", 0) == 0) {
        return exit_codes::kDeploymentFailed;
    }

    return exit_codes::kContainerInvalid;
}

// ---------- 参数解析 ----------

class Arguments {
public:
    Arguments(std::string command, std::vector<std::string> rest) : command_(std::move(command)) {
        for (size_t index = 0; index < rest.size(); ++index) {
            const std::string& token = rest[index];
            if (token.size() >= 2 && token[0] == '-' && token[1] == '-') {
                const std::string name = token.substr(2);
                const size_t equals = name.find('=');
                if (equals != std::string::npos) {
                    values_.emplace_back(name.substr(0, equals), name.substr(equals + 1));
                    continue;
                }

                // 值型参数：--key value；下一个 token 以 -- 开头则视为布尔开关。
                if (index + 1 < rest.size() && rest[index + 1].rfind("--", 0) != 0) {
                    values_.emplace_back(name, rest[index + 1]);
                    ++index;
                    continue;
                }

                flags_.push_back(name);
                continue;
            }

            positional_.push_back(token);
        }
    }

    [[nodiscard]] const std::string& command() const { return command_; }
    [[nodiscard]] const std::vector<std::string>& positional() const { return positional_; }

    [[nodiscard]] bool has(const std::string& name) const {
        if (std::find(flags_.begin(), flags_.end(), name) != flags_.end()) {
            return true;
        }

        return std::any_of(values_.begin(), values_.end(), [&name](const auto& pair) {
            return pair.first == name;
        });
    }

    [[nodiscard]] std::string get(const std::string& name, const std::string& fallback = {}) const {
        for (const auto& pair : values_) {
            if (pair.first == name) {
                return pair.second;
            }
        }

        return fallback;
    }

    [[nodiscard]] std::vector<std::string> all(const std::string& name) const {
        std::vector<std::string> result;
        for (const auto& pair : values_) {
            if (pair.first == name) {
                result.push_back(pair.second);
            }
        }

        return result;
    }

    [[nodiscard]] std::string require(const std::string& name) const {
        const std::string value = get(name);
        if (value.empty()) {
            throw UsageError("missing required option --" + name);
        }

        return value;
    }

private:
    std::string command_;
    std::vector<std::pair<std::string, std::string>> values_;
    std::vector<std::string> flags_;
    std::vector<std::string> positional_;
};

// 这两个名字会同时出现（winrt 的类型 vs 我们的回调），显式限定避免歧义。
std::vector<std::string> collect_thumbprints(const Arguments& arguments) {
    std::vector<std::string> thumbprints;
    for (std::string value : arguments.all("pin")) {
        value.erase(std::remove(value.begin(), value.end(), ' '), value.end());
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        thumbprints.push_back(std::move(value));
    }

    return thumbprints;
}

// ---------- 前端实现 ----------

class ConsoleProgressSink final : public msixpkg::IProgressSink {
public:
    explicit ConsoleProgressSink(bool quiet, bool json) : quiet_(quiet), json_(json) {}

    void OnPhase(msixpkg::Phase phase, std::wstring_view detail) override {
        if (quiet_) {
            return;
        }

        std::cout << "phase   " << msixpkg::phase_id(phase);
        if (!detail.empty()) {
            std::cout << "  " << msixpkg::narrow_utf8(detail);
        }

        std::cout << std::endl;
    }

    void OnProgress(uint32_t percent) override {
        if (quiet_ || json_) {
            return;
        }

        std::cout << "progress " << percent << "%" << std::endl;
    }

    void OnWaitingForElevation() override {
        if (quiet_) {
            return;
        }

        std::cout << "elevate  waiting for administrator confirmation…" << std::endl;
    }

private:
    bool quiet_ = false;
    bool json_ = false;
};

/// 控制台同意：把「将要信任谁」讲清楚，并要求未命中钉扎时输入一个更强的确认词。
class ConsoleConsent final : public msixpkg::IConsentProvider {
public:
    explicit ConsoleConsent(bool assume_yes) : assume_yes_(assume_yes) {}

    msixpkg::ConsentDecision Ask(const msixpkg::ConsentRequest& request) override {
        msixpkg::ConsentDecision decision;

        if (assume_yes_) {
            // --yes：未命中钉扎一律拒绝（无人值守时不能替用户扩大信任面）。
            decision.approved = !request.unpinned_publisher;
            decision.all_users = false;
            if (!decision.approved) {
                std::cerr << "refused  the publisher is not pinned; --yes can never trust a new publisher" << std::endl;
            }

            return decision;
        }

        std::cout << std::endl;
        std::cout << "  application  " << request.package.name << " " << request.package.version << " ("
                  << request.package.architecture << ")" << std::endl;
        std::cout << "  publisher    " << request.publisher.subject << std::endl;
        std::cout << "  sha-256      " << request.publisher.thumbprint_sha256 << std::endl;
        std::cout << "  sha-1        " << request.publisher.thumbprint_sha1 << std::endl;
        if (!request.publisher.not_before_utc.empty()) {
            std::cout << "  valid        " << request.publisher.not_before_utc << " .. "
                      << request.publisher.not_after_utc << std::endl;
        }

        std::cout << "  trusted now  " << (request.publisher.already_trusted ? "yes" : "no") << std::endl;
        std::cout << "  pinned       " << (request.publisher.pinned ? "yes" : "no") << std::endl;
        if (request.already_installed) {
            std::cout << "  note         this package is already installed and will be updated" << std::endl;
        }

        std::cout << std::endl;
        std::cout << "  Installing this package will put the publisher certificate into" << std::endl;
        std::cout << "  LocalMachine\\TrustedPeople, which lets ANY application signed with it install." << std::endl;

        if (request.unpinned_publisher) {
            std::cout << std::endl;
            std::cout << "  WARNING: this publisher is NOT in your pinned list." << std::endl;
            std::cout << "  Type 'trust' (exactly) to trust this publisher, or anything else to abort: ";
            std::string answer;
            if (!std::getline(std::cin, answer) || answer != "trust") {
                decision.approved = false;
                return decision;
            }

            decision.remember_publisher = true;
        }

        std::cout << std::endl;
        std::cout << "  Install? [y/N]: ";
        std::string answer;
        if (!std::getline(std::cin, answer) || answer.empty() ||
            (answer[0] != 'y' && answer[0] != 'Y')) {
            decision.approved = false;
            return decision;
        }

        decision.approved = true;

        if (request.allow_all_users) {
            std::cout << "  Also make it available to ALL users (needs administrator)? [y/N]: ";
            std::string all_users;
            if (std::getline(std::cin, all_users) && !all_users.empty() &&
                (all_users[0] == 'y' || all_users[0] == 'Y')) {
                decision.all_users = true;
            }
        }

        return decision;
    }

private:
    bool assume_yes_ = false;
};

// ---------- 输出工具 ----------

void print_json_error(const msixpkg::Error& error) {
    msixpkg::json::Value root = msixpkg::json::Value::make_object();
    root.set("ok", msixpkg::json::Value::make_bool(false));
    root.set("code", msixpkg::json::Value::make_string(error.code()));
    root.set("message", msixpkg::json::Value::make_string(error.message()));
    root.set("detail", msixpkg::json::Value::make_string(error.detail()));
    std::cout << root.to_json();
}

msixpkg::json::Value string_array(const std::vector<std::string>& values) {
    msixpkg::json::Value array = msixpkg::json::Value::make_array();
    for (const std::string& value : values) {
        array.push(msixpkg::json::Value::make_string(value));
    }

    return array;
}

void print_plan(const std::vector<msixpkg::PlannedAction>& plan) {
    std::cout << "plan" << std::endl;
    for (size_t index = 0; index < plan.size(); ++index) {
        std::cout << "  " << (index + 1) << ". " << plan[index].step << " — " << plan[index].detail << std::endl;
    }
}

// ---------- 子命令 ----------

int command_install(const Arguments& arguments, bool quiet, bool json_output) {
    msixpkg::InstallOptions options;
    options.allow_downgrade = arguments.has("allow-downgrade");
    options.all_users = arguments.has("all-users");
    options.dry_run = arguments.has("dry-run");
    options.allow_elevation = !arguments.has("no-elevate");
    options.keep_staging = arguments.has("keep-staging");
    options.allowed_publisher_thumbprints = collect_thumbprints(arguments);
    options.require_pinned_publisher = arguments.has("require-pin");

    // `--msix <已解包的 .msix>`：跳过容器层，只走部署栈（排查 / 手工安装用）。
    const std::string msix = arguments.get("msix");
    if (!msix.empty()) {
        msixpkg::InstallRequest request;
        request.main_package = std::filesystem::path(msixpkg::widen_utf8(msix));
        for (const std::string& dependency : arguments.all("dependency")) {
            request.dependencies.push_back(std::filesystem::path(msixpkg::widen_utf8(dependency)));
        }

        request.identity_name = arguments.get("name");
        request.identity_publisher = arguments.get("publisher");
        request.identity_version = arguments.get("version");
        request.identity_architecture = arguments.get("architecture");
        request.allow_downgrade = options.allow_downgrade;
        request.all_users = options.all_users;

        if (options.dry_run) {
            print_plan(msixpkg::plan_install(request));
            return exit_codes::kSuccess;
        }

        const msixpkg::InstallResult installed = msixpkg::install_packages(request);
        if (!quiet) {
            std::cout << "installed " << installed.full_name << std::endl;
            std::cout << "identity  " << installed.family_name << " " << installed.version << std::endl;
        }

        return exit_codes::kSuccess;
    }

    if (arguments.positional().empty()) {
        throw UsageError("install requires a positional <container.msixpkg> or --msix <App.msix>");
    }

    options.container = std::filesystem::path(msixpkg::widen_utf8(arguments.positional().front()));

    ConsoleProgressSink sink(quiet, json_output);
    msixpkg::AutoApproveConsent auto_consent(options.all_users);
    ConsoleConsent interactive_consent(false);
    const bool assume_yes = arguments.has("yes");
    msixpkg::IConsentProvider& consent = assume_yes
        ? static_cast<msixpkg::IConsentProvider&>(auto_consent)
        : static_cast<msixpkg::IConsentProvider&>(interactive_consent);
    msixpkg::NeverCancel cancel;

    msixpkg::Installer installer(sink, consent, cancel);
    const msixpkg::InstallOutcome outcome = installer.install(options);

    if (json_output) {
        msixpkg::json::Value root = msixpkg::json::Value::make_object();
        root.set("ok", msixpkg::json::Value::make_bool(true));
        root.set("installed", msixpkg::json::Value::make_bool(outcome.installed));
        root.set("cancelled", msixpkg::json::Value::make_bool(outcome.cancelled));
        root.set("dryRun", msixpkg::json::Value::make_bool(outcome.dry_run));
        root.set("installId", msixpkg::json::Value::make_string(outcome.install_id));
        root.set("identityName", msixpkg::json::Value::make_string(outcome.identity_name));
        root.set("familyName", msixpkg::json::Value::make_string(outcome.family_name));
        root.set("fullName", msixpkg::json::Value::make_string(outcome.full_name));
        root.set("version", msixpkg::json::Value::make_string(outcome.version));
        root.set("publisherSubject", msixpkg::json::Value::make_string(outcome.publisher_subject));
        root.set("certificateSha1", msixpkg::json::Value::make_string(outcome.certificate_sha1));
        root.set("certificateAddedByInstaller", msixpkg::json::Value::make_bool(outcome.certificate_added_by_installer));
        root.set("certificateExistedBefore", msixpkg::json::Value::make_bool(outcome.certificate_existed_before));
        root.set("arpHive", msixpkg::json::Value::make_string(outcome.arp_hive));
        root.set("logPath", msixpkg::json::Value::make_string(outcome.log_path));
        root.set("dataRoot", msixpkg::json::Value::make_string(outcome.data_root));
        root.set("warning", msixpkg::json::Value::make_string(outcome.warning));
        root.set("steps", string_array([&outcome] {
                       std::vector<std::string> steps;
                       for (const msixpkg::PlannedAction& action : outcome.plan) {
                           steps.push_back(action.step);
                       }

                       return steps;
                   }()));
        std::cout << root.to_json();
    } else if (!quiet) {
        if (outcome.dry_run) {
            print_plan(outcome.plan);
            std::cout << "result    dry-run (nothing was changed)" << std::endl;
        } else if (outcome.cancelled) {
            std::cout << "result    cancelled" << std::endl;
        } else {
            std::cout << "installed " << outcome.identity_name << " " << outcome.version << std::endl;
            std::cout << "identity  " << outcome.family_name << std::endl;
            std::cout << "fullname  " << outcome.full_name << std::endl;
            std::cout << "cert      " << outcome.certificate_sha1
                      << (outcome.certificate_added_by_installer ? " (added by this installer)" : " (pre-existing, never removed)")
                      << std::endl;
            if (!outcome.arp_hive.empty()) {
                std::cout << "arp       " << outcome.arp_hive << std::endl;
            }

            std::cout << "log       " << outcome.log_path << std::endl;
            if (!outcome.warning.empty()) {
                std::cout << "warning   " << outcome.warning << std::endl;
            }

            std::cout << "result    installed" << std::endl;
        }
    }

    if (outcome.cancelled) {
        return exit_codes::kCancelled;
    }

    return exit_codes::kSuccess;
}

int command_uninstall(const Arguments& arguments, bool quiet, bool json_output) {
    msixpkg::UninstallOptions options;
    options.install_id = arguments.get("install-id");
    options.family_name = arguments.get("family-name");
    options.certificate_sha1 = arguments.get("cert-thumbprint");
    options.dry_run = arguments.has("dry-run");
    options.allow_elevation = !arguments.has("no-elevate");
    options.allow_certificate_removal = !arguments.has("keep-certificate");

    if (options.install_id.empty() && options.family_name.empty() && options.certificate_sha1.empty()) {
        throw UsageError("uninstall requires --family-name <PFN>, --install-id <id> or --cert-thumbprint <sha1>");
    }

    ConsoleProgressSink sink(quiet, json_output);
    msixpkg::AutoApproveConsent consent(false);
    msixpkg::NeverCancel cancel;
    msixpkg::Installer installer(sink, consent, cancel);

    const msixpkg::UninstallOutcome outcome = installer.uninstall(options);

    if (json_output) {
        msixpkg::json::Value root = msixpkg::json::Value::make_object();
        root.set("ok", msixpkg::json::Value::make_bool(true));
        root.set("removed", msixpkg::json::Value::make_bool(outcome.removed));
        root.set("dryRun", msixpkg::json::Value::make_bool(outcome.dry_run));
        root.set("familyName", msixpkg::json::Value::make_string(outcome.family_name));
        root.set("fullName", msixpkg::json::Value::make_string(outcome.full_name));
        root.set("certificateRemoved", msixpkg::json::Value::make_bool(outcome.certificate_removed));
        root.set("logPath", msixpkg::json::Value::make_string(outcome.log_path));
        root.set("dataRoot", msixpkg::json::Value::make_string(outcome.data_root));

        msixpkg::json::Value decision = msixpkg::json::Value::make_object();
        decision.set("remove", msixpkg::json::Value::make_bool(outcome.decision.remove));
        decision.set("addedByInstaller", msixpkg::json::Value::make_bool(outcome.decision.added_by_installer));
        decision.set("existedBefore", msixpkg::json::Value::make_bool(outcome.decision.existed_before));
        decision.set("otherInstallationsReference", msixpkg::json::Value::make_bool(outcome.decision.other_installations_reference));
        decision.set("otherPackagesUsePublisher", msixpkg::json::Value::make_bool(outcome.decision.other_packages_use_publisher));
        decision.set("policyAllows", msixpkg::json::Value::make_bool(outcome.decision.policy_allows));
        decision.set("consentGranted", msixpkg::json::Value::make_bool(outcome.decision.consent_granted));
        decision.set("reason", msixpkg::json::Value::make_string(outcome.decision.reason));
        root.set("certificate", std::move(decision));
        std::cout << root.to_json();
    } else if (!quiet) {
        if (outcome.dry_run) {
            print_plan(outcome.plan);
            std::cout << "result    dry-run (nothing was changed)" << std::endl;
        } else {
            std::cout << "removed   " << (outcome.removed ? outcome.full_name : "(nothing to remove)") << std::endl;
            std::cout << "cert      " << (outcome.certificate_removed ? "reclaimed" : "kept") << " — "
                      << outcome.decision.reason << std::endl;
            std::cout << "log       " << outcome.log_path << std::endl;
            std::cout << "result    uninstalled" << std::endl;
        }
    }

    return exit_codes::kSuccess;
}

int command_list(const Arguments& arguments, bool quiet, bool json_output) {
    (void)arguments;
    (void)quiet;
    msixpkg::StateStore store(msixpkg::resolve_data_roots());
    const std::vector<msixpkg::InstallationRecord> records = store.load_all();

    if (json_output) {
        msixpkg::json::Value root = msixpkg::json::Value::make_object();
        root.set("dataRoot", msixpkg::json::Value::make_string(msixpkg::narrow_utf8(store.roots().root.wstring())));
        msixpkg::json::Value items = msixpkg::json::Value::make_array();
        for (const msixpkg::InstallationRecord& record : records) {
            msixpkg::json::Value item = msixpkg::json::Value::make_object();
            item.set("installId", msixpkg::json::Value::make_string(record.install_id));
            item.set("name", msixpkg::json::Value::make_string(record.identity_name));
            item.set("version", msixpkg::json::Value::make_string(record.identity_version));
            item.set("architecture", msixpkg::json::Value::make_string(record.identity_architecture));
            item.set("familyName", msixpkg::json::Value::make_string(record.package_family_name));
            item.set("fullName", msixpkg::json::Value::make_string(record.package_full_name));
            item.set("publisher", msixpkg::json::Value::make_string(record.publisher_subject));
            item.set("certificateSha1", msixpkg::json::Value::make_string(record.certificate_sha1));
            item.set("certificateAddedByInstaller", msixpkg::json::Value::make_bool(record.certificate_added_by_installer));
            item.set("certificateExistedBefore", msixpkg::json::Value::make_bool(record.certificate_existed_before));
            item.set("result", msixpkg::json::Value::make_string(record.result));
            item.set("installedUtc", msixpkg::json::Value::make_string(record.started_utc));
            items.push(std::move(item));
        }

        root.set("installations", std::move(items));
        std::cout << root.to_json();
        return exit_codes::kSuccess;
    }

    if (records.empty()) {
        std::cout << "no installations were recorded by this installer" << std::endl;
        return exit_codes::kSuccess;
    }

    for (const msixpkg::InstallationRecord& record : records) {
        std::cout << record.install_id << "  " << record.identity_name << " " << record.identity_version << " ("
                  << record.identity_architecture << ")" << std::endl;
        std::cout << "  family    " << record.package_family_name << std::endl;
        std::cout << "  fullname  " << record.package_full_name << std::endl;
        std::cout << "  cert      " << record.certificate_sha1
                  << (record.certificate_added_by_installer ? " (added by installer)" : " (pre-existing)")
                  << (record.certificate_existed_before ? " [existed before: never removed]" : "") << std::endl;
        std::cout << "  arp       " << (record.arp_entry_created ? record.arp_hive : "-") << std::endl;
        std::cout << "  result    " << record.result << std::endl;
    }

    return exit_codes::kSuccess;
}

int command_verify(const Arguments& arguments, bool quiet, bool json_output) {
    (void)arguments;
    (void)quiet;
    msixpkg::StateStore store(msixpkg::resolve_data_roots());
    const msixpkg::VerifyReport report = msixpkg::verify_state(store, msixpkg::Log{});

    if (json_output) {
        msixpkg::json::Value root = msixpkg::json::Value::make_object();
        root.set("ok", msixpkg::json::Value::make_bool(report.ok()));
        msixpkg::json::Value items = msixpkg::json::Value::make_array();
        for (const msixpkg::VerifyIssue& issue : report.issues) {
            msixpkg::json::Value item = msixpkg::json::Value::make_object();
            item.set("severity", msixpkg::json::Value::make_string(issue.severity));
            item.set("code", msixpkg::json::Value::make_string(issue.code));
            item.set("detail", msixpkg::json::Value::make_string(issue.detail));
            item.set("suggestion", msixpkg::json::Value::make_string(issue.suggestion));
            items.push(std::move(item));
        }

        root.set("issues", std::move(items));
        root.set("installations", msixpkg::json::Value::make_integer(static_cast<int64_t>(report.installations.size())));
        std::cout << root.to_json();
    } else {
        for (const msixpkg::VerifyIssue& issue : report.issues) {
            std::cout << issue.severity << "  " << issue.code << ": " << issue.detail << std::endl;
            if (!issue.suggestion.empty()) {
                std::cout << "        → " << issue.suggestion << std::endl;
            }
        }

        std::cout << "records   " << report.installations.size() << std::endl;
        std::cout << "result    " << (report.ok() ? "consistent" : "INCONSISTENT") << std::endl;
    }

    return report.ok() ? exit_codes::kSuccess : exit_codes::kContainerInvalid;
}

int command_repair(const Arguments& arguments, bool quiet, bool json_output) {
    (void)arguments;
    (void)quiet;
    msixpkg::StateStore store(msixpkg::resolve_data_roots());
    const msixpkg::VerifyReport report = msixpkg::repair_plan(store, msixpkg::Log{});

    if (json_output) {
        msixpkg::json::Value root = msixpkg::json::Value::make_object();
        root.set("appliedAnything", msixpkg::json::Value::make_bool(false));
        msixpkg::json::Value items = msixpkg::json::Value::make_array();
        for (const msixpkg::VerifyIssue& issue : report.issues) {
            msixpkg::json::Value item = msixpkg::json::Value::make_object();
            item.set("severity", msixpkg::json::Value::make_string(issue.severity));
            item.set("code", msixpkg::json::Value::make_string(issue.code));
            item.set("detail", msixpkg::json::Value::make_string(issue.detail));
            item.set("suggestion", msixpkg::json::Value::make_string(issue.suggestion));
            items.push(std::move(item));
        }

        root.set("suggestions", std::move(items));
        std::cout << root.to_json();
    } else {
        std::cout << "repair only reports; nothing was changed" << std::endl;
        for (const msixpkg::VerifyIssue& issue : report.issues) {
            std::cout << issue.severity << "  " << issue.code << ": " << issue.detail << std::endl;
            if (!issue.suggestion.empty()) {
                std::cout << "        → " << issue.suggestion << std::endl;
            }
        }

        if (report.issues.empty()) {
            std::cout << "nothing to suggest: the state store matches the system" << std::endl;
        }
    }

    return exit_codes::kSuccess;
}

int command_clean(const Arguments& arguments, bool quiet, bool json_output) {
    (void)quiet;
    msixpkg::StateStore store(msixpkg::resolve_data_roots());
    size_t keep_logs = 20;
    const std::string keep = arguments.get("keep-logs");
    if (!keep.empty()) {
        keep_logs = static_cast<size_t>(std::max(1, std::atoi(keep.c_str())));
    }

    const msixpkg::CleanReport report = msixpkg::clean_data(store, keep_logs, msixpkg::Log{});

    if (json_output) {
        msixpkg::json::Value root = msixpkg::json::Value::make_object();
        root.set("ok", msixpkg::json::Value::make_bool(true));
        root.set("stagingRemoved", msixpkg::json::Value::make_integer(report.staging_removed));
        root.set("logsRemoved", msixpkg::json::Value::make_integer(report.logs_removed));
        root.set("tempFilesRemoved", msixpkg::json::Value::make_integer(report.temp_files_removed));
        root.set("bytesFreed", msixpkg::json::Value::make_integer(static_cast<int64_t>(report.bytes_freed)));
        std::cout << root.to_json();
    } else {
        std::cout << "staging   " << report.staging_removed << " removed" << std::endl;
        std::cout << "logs      " << report.logs_removed << " removed" << std::endl;
        std::cout << "temp      " << report.temp_files_removed << " removed" << std::endl;
        std::cout << "freed     " << report.bytes_freed << " bytes" << std::endl;
    }

    return exit_codes::kSuccess;
}

int command_cert_add(const Arguments& arguments, bool quiet) {
    const std::string container = arguments.require("container");
    const std::string thumbprint = arguments.require("thumbprint-sha1");
    const std::string result_path = arguments.get("result");

    msixpkg::Log log = msixpkg::Log::at(std::filesystem::path(msixpkg::widen_utf8(arguments.get("log"))));
    log.step("cert-add", "elevated helper started");

    const msixpkg::CertificateHelperResult result = msixpkg::certificate_add_from_container(
        std::filesystem::path(msixpkg::widen_utf8(container)), thumbprint, result_path);

    log.step("cert-add", result.added ? "certificate imported" : "certificate was already present");

    if (!quiet) {
        std::cout << "subject   " << result.subject << std::endl;
        std::cout << "sha1      " << result.sha1 << std::endl;
        std::cout << "added     " << (result.added ? "yes" : "no (already present)") << std::endl;
        std::cout << "existed   " << (result.existed_before ? "yes" : "no") << std::endl;
    }

    return exit_codes::kSuccess;
}

int command_cert_remove(const Arguments& arguments, bool quiet) {
    const std::string thumbprint = arguments.require("thumbprint-sha1");
    msixpkg::Log log = msixpkg::Log::at(std::filesystem::path(msixpkg::widen_utf8(arguments.get("log"))));
    log.step("cert-remove", "elevated helper started");

    const bool removed = msixpkg::certificate_remove_by_sha1(thumbprint);
    log.step("cert-remove", removed ? "certificate removed" : "certificate was not present");

    if (!quiet) {
        std::cout << "removed   " << (removed ? "yes" : "no (not present)") << std::endl;
    }

    return exit_codes::kSuccess;
}

void print_usage() {
    std::cout << "msixpkg-native " << msixpkg::kInstallerVersion << " — .msixpkg installer\n";
    std::cout << "\n";
    std::cout << "  msixpkg-native install <container.msixpkg> [options]\n";
    std::cout << "      --pin <sha256>...            允许的发布者指纹（可重复）\n";
    std::cout << "      --require-pin                未命中钉扎即拒绝\n";
    std::cout << "      --dry-run                    只打印将要执行的动作\n";
    std::cout << "      --yes                        无人值守（未命中钉扎仍然拒绝）\n";
    std::cout << "      --all-users / --allow-downgrade / --keep-staging / --no-elevate\n";
    std::cout << "      --json / --quiet\n";
    std::cout << "  msixpkg-native install --msix <App.msix> [--dependency <d.msix>]... [--dry-run]\n";
    std::cout << "  msixpkg-native uninstall [--family-name <PFN>] [--install-id <id>]\n";
    std::cout << "      [--cert-thumbprint <sha1>] [--keep-certificate] [--dry-run] [--no-elevate]\n";
    std::cout << "  msixpkg-native list [--json]\n";
    std::cout << "  msixpkg-native verify [--json]       状态库 vs 实际系统\n";
    std::cout << "  msixpkg-native repair [--json]       只输出修复建议，不改动任何东西\n";
    std::cout << "  msixpkg-native clean [--keep-logs <n>] [--json]\n";
    std::cout << "  msixpkg-native cert-add --container <c> --thumbprint-sha1 <sha1> [--result <file>]\n";
    std::cout << "  msixpkg-native cert-remove --thumbprint-sha1 <sha1>\n";
    std::cout << "\n";
    std::cout << "  退出码: 0 成功 / 1 未预期 / 2 校验失败 / 3 用法 / 4 I/O\n";
    std::cout << "          5 部署失败 / 6 被取消 / 7 前置条件不满足\n";
}

int run(int argc, wchar_t** argv) {
    std::vector<std::string> raw;
    raw.reserve(static_cast<size_t>(std::max(0, argc - 1)));
    for (int index = 1; index < argc; ++index) {
        raw.push_back(msixpkg::narrow_utf8(argv[index]));
    }

    if (raw.empty()) {
        print_usage();
        return exit_codes::kUsage;
    }

    const std::string command = raw.front();
    if (command == "--help" || command == "-h" || command == "help") {
        print_usage();
        return exit_codes::kSuccess;
    }

    if (command == "--version" || command == "-v" || command == "version") {
        std::cout << "msixpkg-native " << msixpkg::kInstallerVersion << std::endl;
        return exit_codes::kSuccess;
    }

    const Arguments arguments(command, std::vector<std::string>(raw.begin() + 1, raw.end()));
    const bool json_output = arguments.has("json");
    const bool quiet = arguments.has("quiet");

    if (command == "install") return command_install(arguments, quiet, json_output);
    if (command == "uninstall") return command_uninstall(arguments, quiet, json_output);
    if (command == "list") return command_list(arguments, quiet, json_output);
    if (command == "verify") return command_verify(arguments, quiet, json_output);
    if (command == "repair") return command_repair(arguments, quiet, json_output);
    if (command == "clean") return command_clean(arguments, quiet, json_output);
    if (command == "cert-add") return command_cert_add(arguments, quiet);
    if (command == "cert-remove") return command_cert_remove(arguments, quiet);

    std::cerr << "unknown command: " << command << std::endl;
    print_usage();
    return exit_codes::kUsage;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    // 控制台按 UTF-8 输出：否则中文路径与提示会被代码页拆坏。
    ::SetConsoleOutputCP(CP_UTF8);

    try {
        return run(argc, argv);
    } catch (const UsageError& error) {
        std::cout.flush();
        std::cerr << "usage: " << error.what() << std::endl;
        return exit_codes::kUsage;
    } catch (const msixpkg::Error& error) {
        std::cout.flush();
        std::cerr << error.to_string() << std::endl;
        return exit_code_for(error.code());
    } catch (const std::exception& error) {
        std::cout.flush();
        std::cerr << "E_UNEXPECTED: " << error.what() << std::endl;
        return exit_codes::kUnexpected;
    } catch (...) {
        // 最后一道防线。注意 C++/WinRT 的 `winrt::hresult_error` **不继承 std::exception**，
        // 部署层内部已经把它翻译成 msixpkg::Error；这里兜住任何漏网的异常类型，
        // 因为静默 abort 会连已经写出的输出一起丢掉（stdout 缓冲来不及 flush）。
        std::cout.flush();
        std::cerr << "E_UNEXPECTED: a non-standard exception escaped (see the log)" << std::endl;
        return exit_codes::kUnexpected;
    }
}
