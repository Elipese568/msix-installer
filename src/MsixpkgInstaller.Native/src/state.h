#pragma once

// 状态库与数据目录（docs/installer-design.md §7）。
//
// 位置：`%ProgramData%\Msixpkg\`，其中
//   state\<installId>.json   每次安装的账本
//   staging\<random>\        解包出来的载荷（部署前用 extracted_main_package_path 复检）
//   logs\installer-<ts>.log  每步 / 每个指纹 / 每个 HRESULT
//
// 「程序数据目录」必须带**可写性探测回落链**：受限执行环境里
// `%ProgramData%` 或 `%TEMP%` 都可能不可写（本仓库已经踩过两次）。
// 回落会削弱「状态库只能由管理员改」的性质，因此回落时返回 warning，
// 由调用方写进日志与界面。
//
// 写状态一律**原子**：同目录临时文件 → FlushFileBuffers → ReplaceFile。

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace msixpkg {

/// 一次安装 / 卸载的账本。
struct InstallationRecord {
    std::string install_id;
    std::string container_path;

    /// 容器内容摘要（规范 §4.1），用于 repair 时判断「同一份容器」。
    std::string content_digest;

    std::string identity_name;
    std::string identity_publisher;
    std::string identity_version;
    std::string identity_architecture;

    /// 部署栈回读到的真实身份。
    std::string package_full_name;
    std::string package_family_name;

    std::string publisher_subject;
    std::string certificate_sha1;    // 小写 hex（40）
    std::string certificate_sha256;  // 小写 hex（64）
    /// 证书是不是**本次安装**加进去的；false 表示安装前就已受信任。
    bool certificate_added_by_installer = false;
    /// 导入前是否已存在。为 true 时**永不删除**（红线 4）。
    bool certificate_existed_before = false;
    /// 卸载时是否真的把证书删了（repair 用）。
    bool certificate_removed = false;

    bool all_users = false;
    bool provisioned = false;
    bool arp_entry_created = false;
    /// ARP 条目落在哪个配置单元（"HKLM" / "HKCU"；空 = 没写成功）。
    std::string arp_hive;
    /// 容器清单声明的卸载策略：`uninstallRemoveCertificate`（回收判定第 4 条的「策略允许」）。
    bool uninstall_remove_certificate = false;

    /// 依赖包的可读描述（"Name Version"）。依赖包不回滚（框架包共享）。
    std::vector<std::string> dependencies;
    /// 各载荷（主包 + 依赖）的 SHA-256，供 repair / verify 比对。
    std::vector<std::string> payload_sha256;

    std::string log_path;
    /// "installed" / "failed" / "uninstalled"。
    std::string result;
    std::string failure_code;
    std::string failure_detail;

    std::string started_utc;
    std::string finished_utc;

    [[nodiscard]] std::string to_json() const;
    static InstallationRecord from_json(std::string_view text);
};

/// 解析出来的数据目录集合。
struct DataRoots {
    std::filesystem::path root;
    std::filesystem::path state;
    std::filesystem::path staging;
    std::filesystem::path logs;
    /// root 是否落在 `%ProgramData%`（即机器级、可收紧 ACL 的那一档）。
    bool machine_wide = false;
    /// 回落时的说明（空 = 用的就是首选位置）。
    std::string warning;
};

/// 首选 = `%MSIXPKG_DATA_ROOT%` → `%ProgramData%\Msixpkg` → `%LOCALAPPDATA%\Msixpkg` →
/// `<当前目录>\artifacts\data`。每个候选都要通过真实写入探测才会被选中。
DataRoots resolve_data_roots();

/**
 * 与 `resolve_data_roots()` 同样的候选顺序，但**绝不创建任何东西**：
 * 取第一个已经存在的候选；都不存在时返回首选位置的路径（只供展示）。
 *
 * 存在的意义：dry-run 与 GUI 摘要页只想「看一眼」，不该因为一次预览就在
 * `%ProgramData%` 下建目录、写日志、甚至收紧 ACL。
 */
DataRoots peek_data_roots();

/// 显式指定根目录（测试与 E2E 用）。
DataRoots data_roots_at(const std::filesystem::path& root, bool machine_wide = false);

class StateStore {
public:
    explicit StateStore(DataRoots roots) : roots_(std::move(roots)) {}

    [[nodiscard]] const DataRoots& roots() const noexcept { return roots_; }

    /// 建出 state / staging / logs 三个目录。
    void ensure_directories() const;

    /// 原子写 `<state>\<install_id>.json`。
    void save(const InstallationRecord& record) const;
    [[nodiscard]] std::optional<InstallationRecord> load(std::string_view install_id) const;
    /// 目录里能解析出来的全部记录（坏文件跳过，不让 list / verify 整体失败）。
    [[nodiscard]] std::vector<InstallationRecord> load_all() const;
    /// 删除记录文件；不存在也返回 true（幂等）。
    bool remove(std::string_view install_id) const;

    [[nodiscard]] std::vector<InstallationRecord> find_by_family_name(std::string_view family_name) const;
    /// 按 SHA-1 指纹找引用（引用计数的第 1、2 条判定）。
    [[nodiscard]] std::vector<InstallationRecord> find_by_certificate(std::string_view sha1_hex) const;

private:
    DataRoots roots_;
};

/// 原子写文本：同目录临时文件 → flush → ReplaceFile（目标不存在则 MoveFileEx）。
void write_text_atomic(const std::filesystem::path& path, const std::string& text);

/// 读整个文本文件；不存在返回 nullopt。
std::optional<std::string> read_text_file(const std::filesystem::path& path);

/// 在 `<staging>\` 下建一个随机名目录并返回它。
std::filesystem::path create_staging_directory(const DataRoots& roots);

/// 递归删除；不存在不算失败。
void remove_tree_if_present(const std::filesystem::path& path);

/// 把目录 ACL 收紧成 Administrators + SYSTEM 独占。
/// 返回 false 表示没能收紧（`error` 给出原因）—— 调用方记日志后继续，
/// 因为「装给当前用户」本来就不该要求管理员。
bool restrict_directory_to_administrators(const std::filesystem::path& path, std::string& error);

/// UTC 时间戳：`YYYY-MM-DDTHH:MM:SSZ`。
std::string utc_now();

/// 32 位十六进制的随机 install id。
std::string new_install_id();

}  // namespace msixpkg
