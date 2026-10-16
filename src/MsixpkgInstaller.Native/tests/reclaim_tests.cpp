// 证书回收判定（T4 验收 1–3）的单元测试。
//
// 四条判定集中在 `Installer::evaluate_certificate_reclaim`，uninstall / verify / repair /
// GUI 都复用它。这里用**构造出来的**状态库直接测判定本身：不需要真实安装、不需要管理员。

#include <Windows.h>

#include <filesystem>
#include <string>
#include <system_error>

#include "installer.h"
#include "state.h"
#include "test_harness.h"

namespace {

constexpr char kCertificateSha1[] = "9cd702035dd941aa6fe12870550ffc61633281da08";
constexpr char kCertificateSha256[] = "9cd702035dd941aa6fe12870550ffc61633281da08839cfb0dfb3a95e1b913b9";
constexpr char kPublisher[] = "CN=Msixpkg Test Publisher, O=MsixpkgInstaller, C=CN";

std::filesystem::path scratch_root(const char* name) {
    const std::filesystem::path path =
        std::filesystem::path(MSIXPKG_REPO_ROOT) / "artifacts" / "native-tests" / name;
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
    std::filesystem::create_directories(path);
    return path;
}

msixpkg::InstallationRecord record_for(const std::string& install_id, const std::string& identity) {
    msixpkg::InstallationRecord record;
    record.install_id = install_id;
    record.identity_name = identity;
    record.identity_publisher = kPublisher;
    record.identity_version = "1.0.0.0";
    record.identity_architecture = "x64";
    record.package_family_name = identity + "_abcdefghijklm";
    record.package_full_name = identity + "_1.0.0.0_x64__abcdefghijklm";
    record.publisher_subject = kPublisher;
    record.certificate_sha1 = kCertificateSha1;
    record.certificate_sha256 = kCertificateSha256;
    // 默认：本次安装新增、安装前不存在、策略允许、已同意 —— 也就是「够格删除」。
    record.certificate_added_by_installer = true;
    record.certificate_existed_before = false;
    record.uninstall_remove_certificate = true;
    record.result = "installed";
    return record;
}

}  // namespace

MSIXPKG_TEST(reclaim_removes_the_certificate_when_nothing_else_needs_it) {
    const std::filesystem::path root = scratch_root("reclaim-single");
    msixpkg::StateStore store(msixpkg::data_roots_at(root));
    store.ensure_directories();

    const msixpkg::InstallationRecord record = record_for("11111111111111111111111111111111", "Msixpkg.Sample");
    store.save(record);

    const msixpkg::CertificateReclaimDecision decision = msixpkg::Installer::evaluate_certificate_reclaim(
        store, record, /*policy_allows=*/true, /*consent_granted=*/true);

    CHECK(decision.added_by_installer);
    CHECK(!decision.existed_before);
    CHECK(decision.policy_allows);
    CHECK(decision.consent_granted);

    // 枚举包仓库在被限制的进程里会失败（E_ACCESS_DENIED），此时判定必须保守地
    // 「保留证书」而不是抛异常 —— 两种结果都算对，但都要求没有别的门禁拦下它。
    CHECK(!decision.other_installations_reference);
    CHECK(!decision.other_packages_use_publisher);
    CHECK_MSG(decision.remove || decision.repository_unavailable, decision.reason);
    CHECK(!decision.reason.empty());
}

MSIXPKG_TEST(reclaim_keeps_the_certificate_while_another_installation_references_it) {
    // T4 验收 1：同一证书被两个容器共用 → 卸第一个时证书不删。
    const std::filesystem::path root = scratch_root("reclaim-shared");
    msixpkg::StateStore store(msixpkg::data_roots_at(root));
    store.ensure_directories();

    const msixpkg::InstallationRecord first = record_for("11111111111111111111111111111111", "Msixpkg.Sample");
    const msixpkg::InstallationRecord second = record_for("22222222222222222222222222222222", "Msixpkg.Other");
    store.save(first);
    store.save(second);

    const msixpkg::CertificateReclaimDecision decision = msixpkg::Installer::evaluate_certificate_reclaim(
        store, first, true, true);

    CHECK(decision.other_installations_reference);
    CHECK(!decision.remove);
    CHECK(decision.reason.find("another installation") != std::string::npos);

    // 第二个也卸掉之后（记录被删），判定才允许回收。
    store.remove(second.install_id);
    const msixpkg::CertificateReclaimDecision after = msixpkg::Installer::evaluate_certificate_reclaim(
        store, first, true, true);
    CHECK(!after.other_installations_reference);
    CHECK_MSG(after.remove || after.repository_unavailable, after.reason);
}

MSIXPKG_TEST(reclaim_never_removes_a_certificate_that_existed_before) {
    // T4 验收 2：existed_before 为真时永不删除 —— 哪怕其它三条全部满足。
    const std::filesystem::path root = scratch_root("reclaim-preexisting");
    msixpkg::StateStore store(msixpkg::data_roots_at(root));
    store.ensure_directories();

    msixpkg::InstallationRecord record = record_for("11111111111111111111111111111111", "Msixpkg.Sample");
    record.certificate_existed_before = true;
    record.certificate_added_by_installer = false;
    store.save(record);

    const msixpkg::CertificateReclaimDecision decision = msixpkg::Installer::evaluate_certificate_reclaim(
        store, record, true, true);

    CHECK(!decision.remove);
    CHECK(decision.reason.find("already existed") != std::string::npos);

    // 反向验证：把 existed_before 改回 false 之后就不该再被这一条拦下 ——
    // 说明拦下它的确实是「安装前已存在」。
    msixpkg::InstallationRecord mutable_record = record;
    mutable_record.certificate_existed_before = false;
    mutable_record.certificate_added_by_installer = true;
    const msixpkg::CertificateReclaimDecision allowed =
        msixpkg::Installer::evaluate_certificate_reclaim(store, mutable_record, true, true);
    CHECK(allowed.reason.find("already existed") == std::string::npos);
    CHECK_MSG(allowed.remove || allowed.repository_unavailable, allowed.reason);
}

MSIXPKG_TEST(reclaim_requires_installer_ownership_policy_and_consent) {
    const std::filesystem::path root = scratch_root("reclaim-gates");
    msixpkg::StateStore store(msixpkg::data_roots_at(root));
    store.ensure_directories();

    const msixpkg::InstallationRecord record = record_for("11111111111111111111111111111111", "Msixpkg.Sample");
    store.save(record);

    // a) 不是安装器加的 → 不删。
    {
        msixpkg::InstallationRecord other = record;
        other.certificate_added_by_installer = false;
        const auto decision = msixpkg::Installer::evaluate_certificate_reclaim(store, other, true, true);
        CHECK(!decision.remove);
        CHECK(decision.reason.find("did not add") != std::string::npos);
    }

    // e) 容器策略不允许 → 不删。
    {
        const auto decision = msixpkg::Installer::evaluate_certificate_reclaim(store, record, false, true);
        CHECK(!decision.remove);
        CHECK(decision.reason.find("does not ask") != std::string::npos);
    }

    // f) 没同意 → 不删。
    {
        const auto decision = msixpkg::Installer::evaluate_certificate_reclaim(store, record, true, false);
        CHECK(!decision.remove);
        CHECK(decision.reason.find("not approved") != std::string::npos);
    }

    // 没有记录证书指纹时也不删（状态库缺字段 / 容器本来就没带证书）。
    {
        msixpkg::InstallationRecord without_certificate = record;
        without_certificate.certificate_sha1.clear();
        const auto decision =
            msixpkg::Installer::evaluate_certificate_reclaim(store, without_certificate, true, true);
        CHECK(!decision.remove);
        CHECK(!decision.reason.empty());
    }
}

MSIXPKG_TEST(reclaim_is_total_and_conservative_when_the_repository_is_unreadable) {
    // 判定函数必须「全函数」：它被 uninstall / verify / repair 复用，抛异常会让
    // 卸载这条最需要确定性的路径半途而废。仓库读不出来时必须保守（留着证书）。
    const std::filesystem::path root = scratch_root("reclaim-repository");
    msixpkg::StateStore store(msixpkg::data_roots_at(root));
    store.ensure_directories();

    const msixpkg::InstallationRecord record = record_for("11111111111111111111111111111111", "Msixpkg.Sample");
    store.save(record);

    msixpkg::CertificateReclaimDecision decision;
    bool threw = false;
    try {
        decision = msixpkg::Installer::evaluate_certificate_reclaim(store, record, true, true);
    } catch (...) {
        threw = true;
    }

    CHECK_MSG(!threw, "evaluate_certificate_reclaim must not throw");
    CHECK(!threw);
    if (threw) {
        return;
    }

    // 在能枚举包仓库的环境里（本机受限环境下不能）这里会是 remove == true；
    // 读不出来时必须是「保留 + 说明原因」。
    if (decision.repository_unavailable) {
        CHECK(!decision.remove);
        CHECK(decision.reason.find("could not be enumerated") != std::string::npos);
        CHECK(!decision.other_packages_use_publisher);
    } else {
        CHECK_MSG(decision.remove, decision.reason);
    }}

MSIXPKG_TEST(reclaim_supports_the_manual_fallback_record) {
    // T4 验收 3 的「状态库被删」兜底：CLI 会构造一条只带指纹的合成记录，
    // 并把它当作「本次新增」的显式断言。判定本身必须照样能跑完。
    msixpkg::InstallationRecord fallback;
    fallback.package_family_name = "Msixpkg.Sample_abcdefghijklm";
    fallback.certificate_sha1 = kCertificateSha1;
    fallback.certificate_added_by_installer = true;
    fallback.uninstall_remove_certificate = true;

    const std::filesystem::path root = scratch_root("reclaim-fallback");
    msixpkg::StateStore store(msixpkg::data_roots_at(root));
    store.ensure_directories();

    const auto decision = msixpkg::Installer::evaluate_certificate_reclaim(store, fallback, true, true);
    CHECK(!decision.existed_before);
    CHECK(decision.added_by_installer);
    CHECK_MSG(decision.remove || decision.repository_unavailable, decision.reason);
}
