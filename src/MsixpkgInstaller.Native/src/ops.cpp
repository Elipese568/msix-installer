#include "ops.h"

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

#include "arp.h"
#include "certstore.h"
#include "deployment.h"
#include "internal.h"
#include "msixpkg/errors.h"

namespace msixpkg {
namespace {

constexpr size_t kDefaultKeepLogs = 20;

/// 名字不能叫 file_size：实参是 std::filesystem::path，ADL 会把
/// std::filesystem::file_size 一起拉进重载集，导致调用有歧义。
uint64_t path_file_size(const std::filesystem::path& path) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    return error ? 0 : static_cast<uint64_t>(size);
}

uint64_t directory_size(const std::filesystem::path& path) {
    uint64_t total = 0;
    std::error_code error;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(path, error)) {
        if (entry.is_regular_file()) {
            total += path_file_size(entry.path());
        }
    }

    return total;
}

bool is_temporary_state_file(const std::filesystem::path& path) {
    return path.extension() == ".tmp" || path.filename().string().find(".json.tmp-") != std::string::npos;
}

}  // namespace

bool VerifyReport::ok() const {
    return !has_errors();
}

bool VerifyReport::has_errors() const {
    return std::any_of(issues.begin(), issues.end(), [](const VerifyIssue& issue) {
        return issue.severity == "error";
    });
}

VerifyReport verify_state(const StateStore& store, const Log& log) {
    VerifyReport report;
    const std::vector<InstallationRecord> records = store.load_all();

    // 一次性取回系统现状：每个记录都去查一遍仓库会非常慢。
    std::vector<RegisteredPackage> packages;
    try {
        packages = find_registered_packages();
    } catch (const Error& error) {
        report.issues.push_back(
            {"error", "E_VERIFY_NO_PACKAGE_REPOSITORY", "The package repository could not be read: " + error.to_string(), {}});
        log.line("verify: " + error.to_string());
    }

    for (const InstallationRecord& record : records) {
        InstallationStatus status;
        status.record = record;

        for (const RegisteredPackage& package : packages) {
            if (equals_ignore_case_ascii(package.family_name, record.package_family_name) ||
                equals_ignore_case_ascii(package.full_name, record.package_full_name)) {
                status.package_present = true;
                break;
            }
        }

        if (!record.certificate_sha1.empty()) {
            try {
                status.certificate_present = is_publisher_trusted_hex(record.certificate_sha1);
            } catch (const Error& error) {
                report.issues.push_back(
                    {"error", "E_VERIFY_NO_CERTIFICATE_STORE", "The trust store could not be read: " + error.to_string(), {}});
            }
        }

        status.arp_hive = find_arp_entry_hive(arp_key_name(record.identity_name));

        // ---- 逐条比对 ----
        if (record.result == "installed" && !status.package_present) {
            report.issues.push_back(
                {"error",
                 "E_VERIFY_PACKAGE_MISSING",
                 "The package recorded for install id " + record.install_id + " (" + record.identity_name +
                     ") is no longer registered.",
                 "msixpkg-native repair, then reinstall if it is still wanted"});

            if (record.certificate_added_by_installer && !record.certificate_existed_before &&
                status.certificate_present) {
                report.issues.push_back(
                    {"warning",
                     "E_VERIFY_CERTIFICATE_ORPHANED",
                     "The publisher certificate " + record.certificate_sha1 +
                         " was added for that package but nothing reclaims it now.",
                     "msixpkg-native uninstall --family-name " + record.package_family_name +
                         " --cert-thumbprint " + record.certificate_sha1});
            }
        }

        if (record.result == "partial") {
            report.issues.push_back(
                {"warning",
                 "E_VERIFY_PARTIAL_INSTALL",
                 "Install id " + record.install_id + " did not finish" +
                     (record.failure_code.empty() ? std::string() : (": " + record.failure_code)) +
                     ". The package is registered but the bookkeeping is incomplete.",
                 "msixpkg-native uninstall --install-id " + record.install_id});
        }

        if (!record.certificate_added_by_installer && record.certificate_existed_before) {
            report.issues.push_back(
                {"info",
                 "E_VERIFY_CERTIFICATE_PREEXISTING",
                 "The certificate " + record.certificate_sha1 +
                     " was already trusted before the installation, so the installer will never remove it.",
                 {}});
        }

        if (!status.package_present && record.arp_entry_created && status.arp_hive.empty()) {
            report.issues.push_back(
                {"info",
                 "E_VERIFY_ARP_MISSING",
                 "The \"Apps & features\" entry for install id " + record.install_id + " is gone.",
                 {}});
        }

        report.installations.push_back(std::move(status));
    }

    // ---- 暂存目录残留 ----
    std::error_code error;
    if (std::filesystem::is_directory(store.roots().staging, error)) {
        for (const auto& entry : std::filesystem::directory_iterator(store.roots().staging, error)) {
            if (!entry.is_directory()) {
                continue;
            }

            const std::string name = narrow_utf8(entry.path().filename().wstring());
            const bool referenced = std::any_of(
                records.begin(), records.end(), [&name](const InstallationRecord& record) {
                    return record.install_id == name;
                });
            if (!referenced) {
                report.orphan_staging.push_back(name);
            }
        }
    }

    if (!report.orphan_staging.empty()) {
        report.issues.push_back(
            {"warning",
             "E_VERIFY_STAGING_LEFTOVER",
             std::to_string(report.orphan_staging.size()) +
                 " staging director(y/ies) are not referenced by any installation record.",
             "msixpkg-native clean"});
    }

    log.step("verify", std::to_string(report.installations.size()) + " record(s), " +
                            std::to_string(report.issues.size()) + " issue(s)");
    return report;
}

VerifyReport repair_plan(const StateStore& store, const Log& log) {
    // repair 与 verify 看到的是同一份事实；区别只在「必须给出下一步」。
    VerifyReport report = verify_state(store, log);

    for (VerifyIssue& issue : report.issues) {
        if (!issue.suggestion.empty()) {
            continue;
        }

        if (issue.code == "E_VERIFY_CERTIFICATE_ORPHANED") {
            issue.suggestion = "msixpkg-native clean, or remove the certificate manually from certlm.msc";
        } else if (issue.code == "E_VERIFY_STAGING_LEFTOVER") {
            issue.suggestion = "msixpkg-native clean";
        } else if (issue.code == "E_VERIFY_NO_PACKAGE_REPOSITORY" ||
                   issue.code == "E_VERIFY_NO_CERTIFICATE_STORE") {
            issue.suggestion = "run this command again from an elevated prompt";
        } else {
            issue.suggestion = "review the log and decide; repair never changes anything by itself";
        }
    }

    log.step("repair", "produced " + std::to_string(report.issues.size()) + " suggestion(s)");
    return report;
}

CleanReport clean_data(const StateStore& store, size_t keep_logs, const Log& log) {
    CleanReport report;
    const std::vector<InstallationRecord> records = store.load_all();
    std::error_code error;

    // 1) 暂存残留：只删没有被任何记录引用的目录。
    if (std::filesystem::is_directory(store.roots().staging, error)) {
        for (const auto& entry : std::filesystem::directory_iterator(store.roots().staging, error)) {
            if (!entry.is_directory()) {
                continue;
            }

            const std::string name = narrow_utf8(entry.path().filename().wstring());
            const bool referenced = std::any_of(
                records.begin(), records.end(), [&name](const InstallationRecord& record) {
                    return record.install_id == name;
                });
            if (referenced) {
                continue;
            }

            report.bytes_freed += directory_size(entry.path());
            std::error_code remove_error;
            std::filesystem::remove_all(entry.path(), remove_error);
            if (!remove_error) {
                ++report.staging_removed;
            }
        }
    }

    // 2) 原子写留下的临时文件。
    if (std::filesystem::is_directory(store.roots().state, error)) {
        for (const auto& entry : std::filesystem::directory_iterator(store.roots().state, error)) {
            if (!entry.is_regular_file() || !is_temporary_state_file(entry.path())) {
                continue;
            }

            report.bytes_freed += path_file_size(entry.path());
            std::error_code remove_error;
            std::filesystem::remove(entry.path(), remove_error);
            if (!remove_error) {
                ++report.temp_files_removed;
            }
        }
    }

    // 3) 过期日志：保留最新 keep_logs 份。
    const size_t keep = keep_logs == 0 ? kDefaultKeepLogs : keep_logs;
    std::vector<std::filesystem::path> logs;
    if (std::filesystem::is_directory(store.roots().logs, error)) {
        for (const auto& entry : std::filesystem::directory_iterator(store.roots().logs, error)) {
            if (entry.is_regular_file() && entry.path().extension() == ".log") {
                logs.push_back(entry.path());
            }
        }
    }

    // 文件名里带时间戳，按名字排序就是按时间排序。
    std::sort(logs.begin(), logs.end());
    while (logs.size() > keep) {
        const std::filesystem::path oldest = logs.front();
        logs.erase(logs.begin());
        report.bytes_freed += path_file_size(oldest);
        std::error_code remove_error;
        std::filesystem::remove(oldest, remove_error);
        if (!remove_error) {
            ++report.logs_removed;
        }
    }

    log.step(
        "clean",
        "staging=" + std::to_string(report.staging_removed) + " logs=" + std::to_string(report.logs_removed) +
            " temp=" + std::to_string(report.temp_files_removed));
    return report;
}

}  // namespace msixpkg
