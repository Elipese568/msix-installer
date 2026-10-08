#pragma once

// 容器层：条目白名单与命名规则、msixpkg.json / msixpkg.sig 的强类型模型与严格 schema、
// 内容摘要计算。规则全部来自 docs/msixpkg-format-v1.md，错误码与 C# 端逐字一致。

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "certs.h"
#include "internal.h"
#include "msixpkg/errors.h"
#include "zip.h"

namespace msixpkg {

enum class EntryKind {
    Manifest,
    Signature,
    MainPackage,
    Dependency,
    PublisherCertificate,
    PublisherChain,
};

/// 条目名合法性（§2.1）；不合法抛 E_ENTRY_NAME_INVALID。
void validate_entry_name(std::string_view name);

/// 按白名单判定类别；不在白名单内抛 E_ENTRY_UNKNOWN。
EntryKind classify_entry(std::string_view name);

bool is_known_architecture(std::string_view architecture);

// ---------- msixpkg.json 模型 ----------

struct FileRef {
    std::string path;
    std::string sha256;
    uint64_t size = 0;
    std::string name;      // 依赖包标识名（仅依赖使用）
    std::string version;   // 依赖包版本（仅依赖使用）
};

struct Identity {
    std::string name;
    std::string publisher;
    std::string version;
    std::string architecture;
};

struct PublisherInfo {
    std::string cert_path;
    std::string subject;
    std::string thumbprint_sha1;
    std::string thumbprint_sha256;
    std::string not_before_utc;
    std::string not_after_utc;
    bool self_signed = false;
};

struct DisplayInfo {
    std::string name;
    std::string description;
    std::string homepage;
};

struct Manifest {
    int64_t format_version = 0;
    std::string created_utc;
    Identity identity;
    FileRef main_package;
    std::vector<FileRef> dependencies;
    bool has_publisher = false;
    PublisherInfo publisher;
    int64_t min_os_build = 0;
    std::string installer_min_version;
    bool requires_admin = false;
    bool allow_all_users = false;
    bool uninstall_remove_certificate = false;
    bool has_display = false;
    DisplayInfo display;

    static Manifest parse(std::string_view json);
    /// 结构自检（不接触磁盘），失败抛对应的稳定错误码。
    void validate() const;
};

struct Signature {
    std::string algorithm;
    std::string key_thumbprint_sha256;
    std::string content_digest;
    std::string signature_base64;

    static Signature parse(std::string_view json);
};

/// 规范 §4.1 的内容摘要：按条目名序数排序后拼接 "名称\0小写hex\n" 再 SHA-256。
std::string content_digest_hex(const std::vector<std::pair<std::string, std::string>>& names_and_hashes);

// ---------- 容器 ----------

class Container {
public:
    static Container open(const std::filesystem::path& path);

    /// 直接用已经打开的 ZIP 与已解析的清单构造（供 verify_memory 这类内存路径复用）。
    static Container adopt(zip::Archive archive, Manifest manifest) {
        return Container(std::move(archive), std::move(manifest));
    }

    [[nodiscard]] const zip::Archive& archive() const noexcept { return archive_; }
    [[nodiscard]] const Manifest& manifest() const noexcept { return manifest_; }
    [[nodiscard]] bool has_signature() const { return archive_.find("msixpkg.sig") != nullptr; }

    [[nodiscard]] const zip::Entry& require_entry(std::string_view name, const char* code, const char* message) const;
    [[nodiscard]] Bytes read_entry(std::string_view name, uint64_t cap = 64ull * 1024 * 1024) const;
    [[nodiscard]] std::string entry_sha256(const zip::Entry& entry) const;

    /// 规范 §4.1 的内容摘要（排除 msixpkg.sig）。
    [[nodiscard]] std::string compute_content_digest() const;

    /// 把内层 .msix 当作 ZIP 打开。容器写出者一律用 store，因此通常直接借用映射区间；
    /// 若某个第三方容器把载荷 deflate 了，则退化为一次性解压到成员缓冲（保持其生命周期）。
    [[nodiscard]] zip::Archive open_nested(const zip::Entry& entry) const;

private:
    Container(zip::Archive archive, Manifest manifest) : archive_(std::move(archive)), manifest_(std::move(manifest)) {}

    zip::Archive archive_;
    Manifest manifest_;
    mutable Bytes nested_scratch_;
};

/// 校验条目名与白名单、清单是否是第一个条目、条目集合与清单声明是否完全一致。
void check_container_structure(const zip::Archive& archive, const Manifest& manifest);

}  // namespace msixpkg
