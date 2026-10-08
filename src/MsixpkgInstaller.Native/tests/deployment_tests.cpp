// 部署层「纯函数层」的单元测试：包族名推导、file:// URI、计划渲染、错误码翻译。
// 这些都不需要安装任何包，也不会改动系统状态。

#include <Windows.h>

#include <filesystem>
#include <string>
#include <vector>

#include "deployment.h"
#include "internal.h"
#include "test_harness.h"

namespace {

/// 本机真实已安装包的一对身份（取自 Get-AppxPackage），用来验证 PublisherId 算法。
/// name / publisher / family_name 三者必须自洽，否则推导逻辑就是错的。
constexpr char kRealPublisherDn[] =
    "CN=Microsoft Windows, O=Microsoft Corporation, L=Redmond, S=Washington, C=US";
constexpr char kRealName[] = "Microsoft.BioEnrollment";
constexpr char kRealPublisherId[] = "cw5n1h2txyewy";
constexpr char kRealFamilyName[] = "Microsoft.BioEnrollment_cw5n1h2txyewy";

bool has_step(const std::vector<msixpkg::PlannedAction>& actions, const std::string& step) {
    for (const msixpkg::PlannedAction& action : actions) {
        if (action.step == step) {
            return true;
        }
    }

    return false;
}

std::string detail_of(const std::vector<msixpkg::PlannedAction>& actions, const std::string& step) {
    for (const msixpkg::PlannedAction& action : actions) {
        if (action.step == step) {
            return action.detail;
        }
    }

    return {};
}

}  // namespace

MSIXPKG_TEST(deployment_publisher_id_matches_a_real_installed_package) {
    // Appx 的 PublisherId = base32(SHA-256(publisher DN 的 UTF-16LE 字节)[0..8])。
    // 用本机真实包做交叉验证：算法错一个 bit 都对不上。
    CHECK_EQ(msixpkg::publisher_id_from_dn(kRealPublisherDn), std::string(kRealPublisherId));
    CHECK_EQ(
        msixpkg::package_family_name(kRealName, kRealPublisherDn), std::string(kRealFamilyName));

    // 13 个字符是 8 字节 → base32 的固定长度（64 bit / 5 bit 向上取整）。
    CHECK_EQ(msixpkg::publisher_id_from_dn(kRealPublisherDn).size(), size_t{13});

    // 大小写敏感：DN 不同 → 指纹不同（不能把 DN 归一化后再算）。
    CHECK(msixpkg::publisher_id_from_dn("CN=Microsoft Windows") != std::string(kRealPublisherId));
}

MSIXPKG_TEST(deployment_file_uri_percent_encodes_spaces_and_non_ascii) {
    CHECK_EQ(
        msixpkg::file_uri_from_path(std::filesystem::path(L"C:\\Program Files\\App\\x.msix")),
        std::string("file:///C:/Program%20Files/App/x.msix"));

    // 非 ASCII 必须按 UTF-8 字节百分号编码（部署栈只接受合法 URI）。
    // 中 = E4 B8 AD，文 = E6 96 87。
    CHECK_EQ(
        msixpkg::file_uri_from_path(std::filesystem::path(L"C:\\\u4E2D\u6587\\x.msix")),
        std::string("file:///C:/%E4%B8%AD%E6%96%87/x.msix"));

    // 未保留字符（含 : / - . _ ~）保持原样，避免过度编码。
    CHECK_EQ(
        msixpkg::file_uri_from_path(std::filesystem::path(L"C:\\a-b_c~d\\e.msix")),
        std::string("file:///C:/a-b_c~d/e.msix"));

    // 相对路径会被绝对化，且前缀一定合法。
    const std::string relative = msixpkg::file_uri_from_path(std::filesystem::path(L"x.msix"));
    CHECK(relative.rfind("file:///", 0) == 0);
    CHECK(relative.find("/x.msix") != std::string::npos);
}

MSIXPKG_TEST(deployment_plan_install_lists_every_mutating_step) {
    msixpkg::InstallRequest request;
    request.main_package = std::filesystem::path(L"C:\\staging\\App.msix");
    request.dependencies = {
        std::filesystem::path(L"C:\\staging\\VCLibs.msix"),
        std::filesystem::path(L"C:\\staging\\WinUI.msix")};
    request.identity_name = "Sample.App";
    request.identity_publisher = "CN=AppPublisher";
    request.identity_version = "1.0.0.0";
    request.identity_architecture = "x64";

    const std::vector<msixpkg::PlannedAction> actions = msixpkg::plan_install(request);

    // dry-run 与 GUI 摘要页都靠这份计划；少一步就等于「没告诉用户要动什么」。
    CHECK(has_step(actions, "validate"));
    CHECK(has_step(actions, "stage"));
    CHECK(has_step(actions, "certificate"));
    CHECK(has_step(actions, "dependency"));
    CHECK(has_step(actions, "main"));
    CHECK(has_step(actions, "arp"));
    CHECK(has_step(actions, "journal"));
    // 默认不是所有用户，因此不应出现 provisioning 步骤。
    CHECK(!has_step(actions, "provision"));

    const std::string main = detail_of(actions, "main");
    CHECK(main.find("Sample.App") != std::string::npos);
    CHECK(main.find("1.0.0.0") != std::string::npos);
    CHECK(main.find("x64") != std::string::npos);

    // 两个依赖 → 两条 dependency 动作，且各自带上文件名。
    int dependency_count = 0;
    for (const msixpkg::PlannedAction& action : actions) {
        if (action.step == "dependency") {
            ++dependency_count;
        }
    }

    CHECK_EQ(dependency_count, 2);

    request.all_users = true;
    const std::vector<msixpkg::PlannedAction> elevated = msixpkg::plan_install(request);
    CHECK(has_step(elevated, "provision"));
    // provisioning 的目标一定是推导出来的包族名，而不是应用名。
    CHECK_EQ(
        detail_of(elevated, "provision"),
        std::string("Provision for all users (stage to the system volume, then provision): ") +
            msixpkg::package_family_name(request.identity_name, request.identity_publisher));

    // 没有内嵌证书时不该声称要导证书。
    msixpkg::InstallRequest unsigned_request;
    unsigned_request.main_package = std::filesystem::path(L"C:\\staging\\App.msix");
    CHECK(!has_step(msixpkg::plan_install(unsigned_request), "certificate"));
}

MSIXPKG_TEST(deployment_plan_uninstall_removes_the_app_before_reclaiming_the_certificate) {
    msixpkg::UninstallRequest request;
    request.family_name = kRealFamilyName;

    const std::vector<msixpkg::PlannedAction> actions = msixpkg::plan_uninstall(request);
    CHECK(has_step(actions, "locate"));
    CHECK(has_step(actions, "shutdown"));
    CHECK(has_step(actions, "remove"));
    CHECK(has_step(actions, "certificate"));
    CHECK(has_step(actions, "arp"));
    CHECK(has_step(actions, "journal"));

    // 顺序是安全语义的一半：必须先卸应用再谈回收证书
    // （反过来会留下「应用还在、信任已丢」的不可自愈状态）。
    size_t remove_index = actions.size();
    size_t certificate_index = actions.size();
    for (size_t index = 0; index < actions.size(); ++index) {
        if (actions[index].step == "remove") remove_index = index;
        if (actions[index].step == "certificate") certificate_index = index;
    }

    CHECK(remove_index < certificate_index);
    CHECK(detail_of(actions, "certificate").find("no other installation references it") != std::string::npos);
    CHECK(detail_of(actions, "locate").find(kRealFamilyName) != std::string::npos);
}

MSIXPKG_TEST(deployment_failure_mapping_translates_the_important_codes) {
    // 「证书不受信」—— sideload 自签名包最常见的失败，必须给可操作建议。
    const msixpkg::DeploymentFailure untrusted = msixpkg::describe_deployment_failure(0x800B0109u);
    CHECK_EQ(untrusted.name, std::string("CERT_E_UNTRUSTEDROOT"));
    CHECK_EQ(untrusted.code, std::string(msixpkg::deployment_codes::PublisherNotTrusted));
    CHECK(untrusted.hint.find("TrustedPeople") != std::string::npos);

    // 签名本身的问题不能被笼统归到「不受信」。
    const msixpkg::DeploymentFailure no_signature = msixpkg::describe_deployment_failure(0x800B0100u);
    CHECK_EQ(no_signature.code, std::string(msixpkg::deployment_codes::SignatureInvalid));
    const msixpkg::DeploymentFailure bad_digest = msixpkg::describe_deployment_failure(0x80096010u);
    CHECK_EQ(bad_digest.code, std::string(msixpkg::deployment_codes::SignatureInvalid));

    struct Expectation {
        uint32_t hresult;
        const char* code;
        const char* name;
    };

    const Expectation expectations[] = {
        {0x80073CF0u, msixpkg::deployment_codes::PackageInvalid, "ERROR_INSTALL_OPEN_PACKAGE_FAILED"},
        {0x80073CF1u, msixpkg::deployment_codes::PackageNotFound, "ERROR_INSTALL_PACKAGE_NOT_FOUND"},
        {0x80073CF3u, msixpkg::deployment_codes::DependencyMissing, "ERROR_INSTALL_RESOLVE_DEPENDENCY_FAILED"},
        {0x80073CF4u, msixpkg::deployment_codes::OutOfDiskSpace, "ERROR_INSTALL_OUT_OF_DISK_SPACE"},
        {0x80073CF8u, msixpkg::deployment_codes::Cancelled, "ERROR_INSTALL_CANCEL"},
        {0x80073CFDu, msixpkg::deployment_codes::DependencyMissing, "ERROR_INSTALL_PREREQUISITE_FAILED"},
        {0x80073D01u, msixpkg::deployment_codes::BlockedByPolicy, "ERROR_DEPLOYMENT_BLOCKED_BY_POLICY"},
        {0x80073D06u, msixpkg::deployment_codes::HigherVersionInstalled, "ERROR_INSTALL_PACKAGE_DOWNGRADE"},
        {0x80073D10u, msixpkg::deployment_codes::ArchitectureMismatch, "ERROR_INSTALL_WRONG_PROCESSOR_ARCHITECTURE"},
        {0x80070005u, msixpkg::deployment_codes::AccessDenied, "ERROR_ACCESS_DENIED"},
    };

    for (const Expectation& expectation : expectations) {
        const msixpkg::DeploymentFailure failure =
            msixpkg::describe_deployment_failure(expectation.hresult);
        CHECK_EQ(failure.code, std::string(expectation.code));
        CHECK_EQ(failure.name, std::string(expectation.name));
        CHECK_EQ(failure.hresult, expectation.hresult);

        // 任何一条映射都必须给出非空提示，否则用户只会看到一个数字。
        CHECK(!failure.hint.empty());
    }

    // 表里没有的码也要给出稳定码与包含该数字的提示，而不是空字符串。
    const msixpkg::DeploymentFailure unknown = msixpkg::describe_deployment_failure(0xDEADBEEFu);
    CHECK_EQ(unknown.code, std::string(msixpkg::deployment_codes::Failed));
    CHECK(unknown.name.empty());
    CHECK(unknown.hint.find("0xDEADBEEF") != std::string::npos);
}
