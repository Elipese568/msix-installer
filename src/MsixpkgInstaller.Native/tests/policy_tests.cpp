// 钉扎策略（machine whitelist + TOFU 记忆）与只读预演 `Installer::plan()` 的单元测试。
//
// 这里盯住两件事：
//   1. 「信任此发布者（将记住）」必须**真的**记住 —— 否则 UI 在说假话；
//   2. `plan()` / `--dry-run` 必须**零副作用** —— 只预览一次不该在 %ProgramData% 下
//      建目录、写日志、甚至收紧 ACL。

#include <Windows.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#include "installer.h"
#include "interface.h"
#include "policy.h"
#include "state.h"
#include "test_harness.h"

namespace {

/// 黄金样本内嵌证书的 SHA-256（见 tests/vectors/README.md）。
constexpr char kGoldenThumbprint[] = "9cd702035dd941aa6fe12870550ffc61633281da08839cfb0dfb3a95e1b913b9";
constexpr char kGoldenSubject[] = "CN=Msixpkg Test Publisher, O=MsixpkgInstaller, C=CN";

std::filesystem::path golden_container() {
    return std::filesystem::path(MSIXPKG_VECTORS_DIR) / "Msixpkg.Sample_1.0.0.0_x64.msixpkg";
}

std::filesystem::path scratch_root(const char* name) {
    const std::filesystem::path path =
        std::filesystem::path(MSIXPKG_REPO_ROOT) / "artifacts" / "native-tests" / name;
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
    std::filesystem::create_directories(path);
    return path;
}

/// 把数据根指到某个目录，析构时恢复原值（测试之间不能互相污染）。
struct DataRootScope {
    explicit DataRootScope(const std::filesystem::path& root) {
        const DWORD needed = ::GetEnvironmentVariableW(L"MSIXPKG_DATA_ROOT", nullptr, 0);
        if (needed > 0) {
            previous_.resize(needed, L'\0');
            const DWORD written = ::GetEnvironmentVariableW(L"MSIXPKG_DATA_ROOT", previous_.data(), needed);
            previous_.resize(written);
            had_previous_ = true;
        }

        ::SetEnvironmentVariableW(L"MSIXPKG_DATA_ROOT", root.c_str());
    }

    DataRootScope(const DataRootScope&) = delete;
    DataRootScope& operator=(const DataRootScope&) = delete;

    ~DataRootScope() {
        ::SetEnvironmentVariableW(
            L"MSIXPKG_DATA_ROOT", had_previous_ ? previous_.c_str() : nullptr);
    }

    std::wstring previous_;
    bool had_previous_ = false;
};

msixpkg::InstallOptions golden_options() {
    msixpkg::InstallOptions options;
    options.container = golden_container();
    return options;
}

/// dry-run 版本：与 golden_options() 同一份输入，只是明确要求「什么都别做」。
msixpkg::InstallOptions golden_options_with_dry_run() {
    msixpkg::InstallOptions options = golden_options();
    options.dry_run = true;
    return options;
}

}  // namespace

MSIXPKG_TEST(policy_normalizes_thumbprints_and_rejects_junk) {
    CHECK_EQ(
        msixpkg::normalize_thumbprint("9CD70203 5DD941AA6FE12870550FFC61633281DA08839CFB0DFB3A95E1B913B9"),
        std::string(kGoldenThumbprint));
    CHECK_EQ(msixpkg::normalize_thumbprint("  9cd702035dd941aa6fe12870550ffc61633281da08839cfb0dfb3a95e1b913b9\n"),
             std::string(kGoldenThumbprint));

    // 长度不对 / 含非十六进制字符 → 空串（调用方据此忽略该条目）。
    CHECK(msixpkg::normalize_thumbprint("").empty());
    CHECK(msixpkg::normalize_thumbprint("abcd").empty());
    CHECK(msixpkg::normalize_thumbprint(std::string(64, 'z')).empty());
    CHECK(msixpkg::normalize_thumbprint(std::string(kGoldenThumbprint) + "00").empty());
}

MSIXPKG_TEST(policy_missing_machine_whitelist_is_empty_and_never_throws) {
    // 没部署 HKLM 白名单是常态；此时必须是「空集合 + 不抛」，也就是默认拒绝那条安全路径。
    const msixpkg::DataRoots roots = msixpkg::data_roots_at(scratch_root("policy-empty"));

    msixpkg::PublisherPolicy policy;
    bool threw = false;
    try {
        policy = msixpkg::load_publisher_policy(roots);
    } catch (...) {
        threw = true;
    }

    CHECK_MSG(!threw, "load_publisher_policy must not throw");
    CHECK(!policy.is_pinned(kGoldenThumbprint));
    CHECK(policy.remembered.empty());
}

MSIXPKG_TEST(policy_remembered_publisher_round_trips_and_deduplicates) {
    const std::filesystem::path root = scratch_root("policy-remember");
    const msixpkg::DataRoots roots = msixpkg::data_roots_at(root);

    // 记住一次 → 立刻生效。
    CHECK(msixpkg::remember_publisher(roots, kGoldenThumbprint, kGoldenSubject));
    CHECK(std::filesystem::is_regular_file(msixpkg::remembered_publishers_path(roots)));

    msixpkg::PublisherPolicy policy = msixpkg::load_publisher_policy(roots);
    CHECK_EQ(policy.remembered.size(), size_t{1});
    CHECK(policy.is_pinned(kGoldenThumbprint));
    CHECK(policy.is_pinned(std::string(kGoldenThumbprint)));

    // 重复记住同一条：不能变成两条（大小写不同也算同一条）。
    CHECK(msixpkg::remember_publisher(roots, kGoldenThumbprint, kGoldenSubject));
    std::string upper(kGoldenThumbprint);
    for (char& c : upper) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }

    CHECK(msixpkg::remember_publisher(roots, upper, "CN=Renamed"));
    policy = msixpkg::load_publisher_policy(roots);
    CHECK_EQ(policy.remembered.size(), size_t{1});
    CHECK_EQ(policy.remembered.front(), std::string(kGoldenThumbprint));

    // 记一个无效指纹：必须被拒（不然会往白名单里塞垃圾）。
    CHECK(!msixpkg::remember_publisher(roots, "not-a-thumbprint", kGoldenSubject));
}

MSIXPKG_TEST(policy_ignores_a_corrupt_remembered_file) {
    const std::filesystem::path root = scratch_root("policy-corrupt");
    const msixpkg::DataRoots roots = msixpkg::data_roots_at(root);
    std::filesystem::create_directories(root);

    {
        std::ofstream broken(msixpkg::remembered_publishers_path(roots), std::ios::binary);
        broken << "{ this is not json";
    }

    const msixpkg::PublisherPolicy policy = msixpkg::load_publisher_policy(roots);
    // 坏文件 = 「什么都没记住」→ 回到「未命中钉扎 → 需要明确同意」，这是安全方向。
    CHECK(policy.remembered.empty());
    CHECK(!policy.is_pinned(kGoldenThumbprint));
    CHECK(!policy.warnings.empty());
}

MSIXPKG_TEST(plan_is_read_only_and_never_creates_the_data_root) {
    // 数据根指向一个**不存在**的目录：如果 plan() 偷偷调用了 resolve_data_roots()
    // （它会建目录），这个目录就会冒出来 —— 这正是要被拦住的回归。
    const std::filesystem::path root =
        std::filesystem::path(MSIXPKG_REPO_ROOT) / "artifacts" / "native-tests" / "plan-must-not-create";
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    CHECK(!std::filesystem::exists(root));

    DataRootScope scope(root);

    msixpkg::SilentProgressSink sink;
    msixpkg::AutoApproveConsent consent;
    msixpkg::NeverCancel cancel;
    msixpkg::Installer installer(sink, consent, cancel);

    const msixpkg::InstallPlan plan = installer.plan(golden_options());

    CHECK_MSG(plan.valid, plan.failure_code + ": " + plan.failure_message);
    CHECK(plan.has_manifest);
    CHECK(plan.has_publisher_certificate);
    CHECK_EQ(plan.manifest.identity.name, std::string("Msixpkg.Sample"));
    CHECK_EQ(plan.package_family_name, std::string("Msixpkg.Sample_") + msixpkg::publisher_id_from_dn(kGoldenSubject));
    CHECK_EQ(plan.certificate_sha256, std::string(kGoldenThumbprint));
    CHECK_EQ(plan.publisher_subject, std::string(kGoldenSubject));
    CHECK(!plan.content_digest.empty());
    CHECK(!plan.actions.empty());

    // 同意页需要的信息必须齐备，否则 GUI 只能自己再 validate 一遍。
    CHECK_EQ(plan.consent.package.name, std::string("Msixpkg.Sample"));
    CHECK_EQ(plan.consent.package.version, std::string("1.0.0.0"));
    CHECK_EQ(plan.consent.package.architecture, std::string("x64"));
    CHECK_EQ(plan.consent.publisher.thumbprint_sha256, std::string(kGoldenThumbprint));
    CHECK(!plan.consent.publisher.not_after_utc.empty());
    CHECK(plan.consent.allow_all_users);
    // 没有钉扎白名单 → 未命中钉扎 → 同意必须显式批准。
    CHECK(!plan.publisher_pinned);
    CHECK(plan.consent.unpinned_publisher);

    // dry-run 与预览一样：一个文件都不该被创建。
    const msixpkg::InstallOutcome outcome = installer.install(golden_options_with_dry_run());
    CHECK(outcome.dry_run);
    CHECK(!outcome.installed);
    CHECK(!outcome.plan.empty());

    CHECK_MSG(!std::filesystem::exists(root), "plan()/dry-run must not create the data directory");
}

MSIXPKG_TEST(plan_rejects_a_broken_container_with_a_stable_code) {
    DataRootScope scope(std::filesystem::path(MSIXPKG_REPO_ROOT) / "artifacts" / "native-tests" / "plan-broken");

    msixpkg::SilentProgressSink sink;
    msixpkg::AutoApproveConsent consent;
    msixpkg::NeverCancel cancel;
    msixpkg::Installer installer(sink, consent, cancel);

    msixpkg::InstallOptions options;
    options.container = std::filesystem::path(MSIXPKG_VECTORS_DIR) / "negative-expired.msixpkg";

    const msixpkg::InstallPlan plan = installer.plan(options);
    CHECK(!plan.valid);
    CHECK_EQ(plan.failure_code, std::string(msixpkg::codes::CertificateExpired));
    CHECK(!plan.failure_message.empty());

    // 不存在的容器同样给出稳定码而不是崩溃。
    msixpkg::InstallOptions missing;
    missing.container = std::filesystem::path(MSIXPKG_REPO_ROOT) / "artifacts" / "no-such.msixpkg";
    const msixpkg::InstallPlan missing_plan = installer.plan(missing);
    CHECK(!missing_plan.valid);
    CHECK_EQ(missing_plan.failure_code, std::string(msixpkg::codes::IoError));
}

MSIXPKG_TEST(plan_honours_an_explicit_pin) {
    DataRootScope scope(std::filesystem::path(MSIXPKG_REPO_ROOT) / "artifacts" / "native-tests" / "plan-pin");

    msixpkg::SilentProgressSink sink;
    msixpkg::AutoApproveConsent consent;
    msixpkg::NeverCancel cancel;
    msixpkg::Installer installer(sink, consent, cancel);

    msixpkg::InstallOptions options = golden_options();
    options.allowed_publisher_thumbprints = {kGoldenThumbprint};
    options.require_pinned_publisher = true;

    const msixpkg::InstallPlan plan = installer.plan(options);
    CHECK_MSG(plan.valid, plan.failure_code + ": " + plan.failure_message);
    CHECK(plan.publisher_pinned);
    CHECK(!plan.publisher_remembered);
    CHECK(!plan.consent.unpinned_publisher);

    // 钉错指纹时必须被拒（门禁不能形同虚设）。
    msixpkg::InstallOptions wrong = golden_options();
    wrong.allowed_publisher_thumbprints = {std::string(64, '0')};
    wrong.require_pinned_publisher = true;
    const msixpkg::InstallPlan rejected = installer.plan(wrong);
    CHECK(!rejected.valid);
    CHECK_EQ(rejected.failure_code, std::string(msixpkg::codes::NotPinned));
}

MSIXPKG_TEST(plan_treats_a_remembered_publisher_as_pinned_on_the_next_run) {
    // 这是「信任此发布者（将记住）」的闭环：第一次记住之后，第二次预演就必须
    // 把它当作已钉扎，而且能区分出「这是用户自己上次同意的」。
    const std::filesystem::path root = scratch_root("plan-remembered");
    DataRootScope scope(root);
    std::filesystem::create_directories(root);

    msixpkg::SilentProgressSink sink;
    msixpkg::AutoApproveConsent consent;
    msixpkg::NeverCancel cancel;
    msixpkg::Installer installer(sink, consent, cancel);

    const msixpkg::InstallPlan before = installer.plan(golden_options());
    CHECK_MSG(before.valid, before.failure_code);
    CHECK(!before.publisher_pinned);

    CHECK(msixpkg::remember_publisher(msixpkg::data_roots_at(root), kGoldenThumbprint, kGoldenSubject));

    const msixpkg::InstallPlan after = installer.plan(golden_options());
    CHECK_MSG(after.valid, after.failure_code);
    CHECK_MSG(after.publisher_pinned, "a remembered publisher must count as pinned");
    CHECK_MSG(after.publisher_remembered, "it must be distinguishable from an admin whitelist entry");
    CHECK(!after.consent.unpinned_publisher);

    // 关掉记忆开关就回到「每次都要问」。
    msixpkg::InstallOptions forgetful = golden_options();
    forgetful.use_remembered_publishers = false;
    const msixpkg::InstallPlan forgot = installer.plan(forgetful);
    CHECK(!forgot.publisher_pinned);
    CHECK(forgot.consent.unpinned_publisher);
}
