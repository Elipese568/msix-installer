// 状态库的单元测试（T3 验收 1：写 → 读 → 原子性）。
//
// 全部在 `artifacts/native-tests/state-*` 下进行，不碰真实的 %ProgramData%。

#include <Windows.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include "internal.h"
#include "state.h"
#include "test_harness.h"

namespace {

std::filesystem::path scratch_root(const char* name) {
    const std::filesystem::path path =
        std::filesystem::path(MSIXPKG_REPO_ROOT) / "artifacts" / "native-tests" / name;
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
    std::filesystem::create_directories(path);
    return path;
}

/// 目录里除了期望的文件之外还剩什么（用于断言「不留临时文件」）。
std::vector<std::string> file_names(const std::filesystem::path& directory) {
    std::vector<std::string> names;
    std::error_code error;
    if (!std::filesystem::is_directory(directory, error)) {
        return names;
    }

    for (const auto& entry : std::filesystem::directory_iterator(directory, error)) {
        names.push_back(msixpkg::narrow_utf8(entry.path().filename().wstring()));
    }

    return names;
}

msixpkg::InstallationRecord sample_record(const std::string& install_id) {
    msixpkg::InstallationRecord record;
    record.install_id = install_id;
    record.container_path = "D:\\apps\\Sample_1.0.0.0_x64.msixpkg";
    record.content_digest = "e67cf3483677ed9d4f33b23e2c0ff7e20ca2648124dd1f248772bea13513b4fc";
    record.identity_name = "Msixpkg.Sample";
    record.identity_publisher = "CN=AppPublisher";
    record.identity_version = "1.0.0.0";
    record.identity_architecture = "x64";
    record.package_full_name = "Msixpkg.Sample_1.0.0.0_x64__abcdefghijklm";
    record.package_family_name = "Msixpkg.Sample_abcdefghijklm";
    record.publisher_subject = "CN=AppPublisher";
    record.certificate_sha1 = "9cd702035dd941aa6fe12870550ffc61633281da08";
    record.certificate_sha256 = "9cd702035dd941aa6fe12870550ffc61633281da08839cfb0dfb3a95e1b913b9";
    record.certificate_added_by_installer = true;
    record.certificate_existed_before = false;
    record.all_users = false;
    record.provisioned = false;
    record.arp_entry_created = true;
    record.arp_hive = "HKCU";
    record.uninstall_remove_certificate = true;
    record.dependencies = {"Microsoft.VCLibs.140.00 14.0.30704.0"};
    record.payload_sha256 = {"aa", "bb"};
    record.log_path = "C:\\ProgramData\\Msixpkg\\logs\\installer-20260101-000000-000.log";
    record.result = "installed";
    record.started_utc = "2026-01-01T00:00:00Z";
    record.finished_utc = "2026-01-01T00:01:00Z";
    return record;
}

}  // namespace

MSIXPKG_TEST(state_record_round_trips_through_json) {
    const msixpkg::InstallationRecord original = sample_record("0123456789abcdef0123456789abcdef");
    const msixpkg::InstallationRecord parsed =
        msixpkg::InstallationRecord::from_json(original.to_json());

    CHECK_EQ(parsed.install_id, original.install_id);
    CHECK_EQ(parsed.container_path, original.container_path);
    CHECK_EQ(parsed.content_digest, original.content_digest);
    CHECK_EQ(parsed.identity_name, original.identity_name);
    CHECK_EQ(parsed.identity_publisher, original.identity_publisher);
    CHECK_EQ(parsed.identity_version, original.identity_version);
    CHECK_EQ(parsed.identity_architecture, original.identity_architecture);
    CHECK_EQ(parsed.package_full_name, original.package_full_name);
    CHECK_EQ(parsed.package_family_name, original.package_family_name);
    CHECK_EQ(parsed.publisher_subject, original.publisher_subject);
    CHECK_EQ(parsed.certificate_sha1, original.certificate_sha1);
    CHECK_EQ(parsed.certificate_sha256, original.certificate_sha256);
    CHECK_EQ(parsed.certificate_added_by_installer, original.certificate_added_by_installer);
    CHECK_EQ(parsed.certificate_existed_before, original.certificate_existed_before);
    CHECK_EQ(parsed.arp_entry_created, original.arp_entry_created);
    CHECK_EQ(parsed.arp_hive, original.arp_hive);
    CHECK_EQ(parsed.uninstall_remove_certificate, original.uninstall_remove_certificate);
    CHECK_EQ(parsed.dependencies.size(), original.dependencies.size());
    CHECK_EQ(parsed.dependencies.front(), original.dependencies.front());
    CHECK_EQ(parsed.payload_sha256.size(), original.payload_sha256.size());
    CHECK_EQ(parsed.log_path, original.log_path);
    CHECK_EQ(parsed.result, original.result);
    CHECK_EQ(parsed.started_utc, original.started_utc);
    CHECK_EQ(parsed.finished_utc, original.finished_utc);

    // 缺字段的旧文件必须仍然能读（前向兼容：新字段有默认值）。
    const msixpkg::InstallationRecord sparse =
        msixpkg::InstallationRecord::from_json("{\n  \"installId\": \"aa\",\n  \"result\": \"installed\"\n}\n");
    CHECK_EQ(sparse.install_id, std::string("aa"));
    CHECK_EQ(sparse.result, std::string("installed"));
    CHECK(!sparse.certificate_added_by_installer);
}

MSIXPKG_TEST(state_save_load_and_remove_behave_like_a_store) {
    const std::filesystem::path root = scratch_root("state-basic");
    msixpkg::StateStore store(msixpkg::data_roots_at(root));
    store.ensure_directories();

    CHECK(std::filesystem::is_directory(store.roots().state));
    CHECK(std::filesystem::is_directory(store.roots().staging));
    CHECK(std::filesystem::is_directory(store.roots().logs));

    const std::string id = "0123456789abcdef0123456789abcdef";
    store.save(sample_record(id));

    const std::optional<msixpkg::InstallationRecord> loaded = store.load(id);
    CHECK(loaded.has_value());
    CHECK_EQ(loaded->install_id, id);
    CHECK_EQ(loaded->package_family_name, std::string("Msixpkg.Sample_abcdefghijklm"));

    // 原子写不该留下临时文件。
    const std::vector<std::string> names = file_names(store.roots().state);
    CHECK_EQ(names.size(), size_t{1});
    CHECK_EQ(names.front(), id + ".json");

    // 按族名与证书指纹检索（引用计数判定靠它们）。
    CHECK_EQ(store.find_by_family_name("Msixpkg.Sample_abcdefghijklm").size(), size_t{1});
    CHECK_EQ(store.find_by_family_name("MSIXPKG.SAMPLE_ABCDEFGHIJKLM").size(), size_t{1});
    CHECK_EQ(store.find_by_family_name("other_family").size(), size_t{0});
    CHECK_EQ(store.find_by_certificate("9CD702035DD941AA6FE12870550FFC61633281DA08").size(), size_t{1});
    CHECK_EQ(store.find_by_certificate("00").size(), size_t{0});

    // 覆盖写：第二次保存必须原子替换，内容是最新的。
    // 注意：这里必须先存成具名的 optional 再解引用。
    // 写成 `CHECK_EQ(store.load(id)->result, ...)` 会踩到「临时 optional 在
    // CHECK_EQ 的第一条语句结束时就被销毁、而 auto&& 还指着它」的悬垂读。
    msixpkg::InstallationRecord updated = sample_record(id);
    updated.result = "uninstalled";
    updated.certificate_removed = true;
    store.save(updated);
    CHECK_EQ(file_names(store.roots().state).size(), size_t{1});

    // 先看原始文件，把「写坏了」和「读坏了」分开。
    const std::optional<std::string> raw = msixpkg::read_text_file(store.roots().state / (id + ".json"));
    CHECK(raw.has_value());
    CHECK_MSG(
        raw.has_value() && raw->find("\"result\": \"uninstalled\"") != std::string::npos,
        raw.has_value() ? *raw : std::string("(no file)"));

    const std::optional<msixpkg::InstallationRecord> reloaded = store.load(id);
    CHECK(reloaded.has_value());
    if (reloaded.has_value()) {
        CHECK_EQ(reloaded->result, std::string("uninstalled"));
        CHECK(reloaded->certificate_removed);
    }

    // 删除是幂等的。
    CHECK(store.remove(id));
    CHECK(!store.load(id).has_value());
    CHECK(store.remove(id));
}

MSIXPKG_TEST(state_store_survives_half_written_and_corrupt_files) {
    const std::filesystem::path root = scratch_root("state-corrupt");
    msixpkg::StateStore store(msixpkg::data_roots_at(root));
    store.ensure_directories();

    const std::string id = "11111111111111111111111111111111";
    store.save(sample_record(id));

    // 模拟「原子写被打断」：留下一个 .json.tmp-* 半成品。它不能影响 load_all，
    // 也不能被当成一条安装记录（否则引用计数会凭空多一条引用）。
    {
        std::ofstream half(store.roots().state / (id + ".json.tmp-deadbeef"), std::ios::binary);
        half << "{\n  \"installId\": \"1111111111111111111111111111111";
    }

    // 模拟被人手改坏的记录文件。
    {
        std::ofstream corrupt(store.roots().state / "22222222222222222222222222222222.json", std::ios::binary);
        corrupt << "{ this is not json";
    }

    const std::vector<msixpkg::InstallationRecord> records = store.load_all();
    CHECK_EQ(records.size(), size_t{1});
    CHECK_EQ(records.front().install_id, id);
}

MSIXPKG_TEST(state_save_rejects_install_ids_that_are_not_hexadecimal) {
    const std::filesystem::path root = scratch_root("state-badid");
    msixpkg::StateStore store(msixpkg::data_roots_at(root));
    store.ensure_directories();

    // install id 会直接变成文件名：路径穿越必须在入口就被拒绝。
    msixpkg::InstallationRecord evil = sample_record("../../evil");
    CHECK_THROWS_CODE(store.save(evil), msixpkg::codes::IoError);

    msixpkg::InstallationRecord uppercase = sample_record("AABBCCDDEEFF00112233445566778899");
    CHECK_THROWS_CODE(store.save(uppercase), msixpkg::codes::IoError);

    msixpkg::InstallationRecord empty = sample_record("");
    CHECK_THROWS_CODE(store.save(empty), msixpkg::codes::IoError);

    // 没有留下任何文件。
    CHECK_EQ(file_names(store.roots().state).size(), size_t{0});
}

MSIXPKG_TEST(state_atomic_write_replaces_content_whole) {
    const std::filesystem::path root = scratch_root("state-atomic");
    const std::filesystem::path target = root / "nested" / "file.json";

    // 目标不存在时也要能创建（ReplaceFile 在目标缺失时会失败，代码里退到 MoveFileEx）。
    msixpkg::write_text_atomic(target, "first");
    CHECK(std::filesystem::is_regular_file(target));
    CHECK_EQ(msixpkg::read_text_file(target).value_or(""), std::string("first"));

    // 覆盖写一个明显更大的内容：必须整体替换，不能出现「旧尾巴」。
    const std::string big(200000, 'x');
    msixpkg::write_text_atomic(target, big);
    const std::optional<std::string> written = msixpkg::read_text_file(target);
    CHECK(written.has_value());
    CHECK_EQ(written->size(), big.size());
    CHECK_EQ(*written, big);

    // 目录里只应剩下目标文件本身。
    const std::vector<std::string> names = file_names(target.parent_path());
    CHECK_EQ(names.size(), size_t{1});
    CHECK_EQ(names.front(), std::string("file.json"));
}

MSIXPKG_TEST(state_staging_and_ids_and_timestamps_are_well_formed) {
    const std::filesystem::path root = scratch_root("state-staging");
    const msixpkg::DataRoots roots = msixpkg::data_roots_at(root);
    msixpkg::StateStore store(roots);
    store.ensure_directories();

    const std::filesystem::path staging = msixpkg::create_staging_directory(roots);
    CHECK(std::filesystem::is_directory(staging));
    CHECK_EQ(staging.parent_path(), roots.staging);

    // install id：32 位小写十六进制，两次调用不相同。
    const std::string first = msixpkg::new_install_id();
    const std::string second = msixpkg::new_install_id();
    CHECK_EQ(first.size(), size_t{32});
    CHECK(msixpkg::is_lower_hex(first, 32));
    CHECK(msixpkg::is_lower_hex(second, 32));
    CHECK(first != second);

    // 暂存目录名就是一次性的 install id（repair / clean 靠它把目录和记录对上）。
    const std::string staging_name = msixpkg::narrow_utf8(staging.filename().wstring());
    CHECK_EQ(staging_name.size(), size_t{32});
    CHECK(msixpkg::is_lower_hex(staging_name, 32));

    // 时间戳：YYYY-MM-DDTHH:MM:SSZ。
    const std::string now = msixpkg::utc_now();
    CHECK_EQ(now.size(), size_t{20});
    CHECK_EQ(now.back(), 'Z');
    CHECK_EQ(now[4], '-');
    CHECK_EQ(now[10], 'T');
    CHECK_EQ(now[13], ':');

    // 删除目录：不存在也不算失败。
    msixpkg::remove_tree_if_present(staging);
    CHECK(!std::filesystem::exists(staging));
    msixpkg::remove_tree_if_present(staging);
}

MSIXPKG_TEST(state_data_roots_layout_is_state_staging_logs) {
    const std::filesystem::path root = scratch_root("state-roots");
    const msixpkg::DataRoots roots = msixpkg::data_roots_at(root);

    CHECK_EQ(roots.root, root);
    CHECK_EQ(roots.state, root / "state");
    CHECK_EQ(roots.staging, root / "staging");
    CHECK_EQ(roots.logs, root / "logs");
    CHECK(!roots.machine_wide);

    // 解析出来的默认位置必须真的可写（探测链的最后一步一定成立）。
    const msixpkg::DataRoots resolved = msixpkg::resolve_data_roots();
    CHECK(!resolved.root.empty());
    CHECK(std::filesystem::is_directory(resolved.logs));
}
