#pragma once

// 面向安装器的校验入口：按 docs/msixpkg-format-v1.md §5 的顺序执行全部检查，
// 返回包含稳定错误码的报告（而不是抛出单个异常）—— 因为 GUI 需要把「所有问题」一次展示给用户。

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "container.h"
#include "internal.h"
#include "msixpkg/errors.h"

namespace msixpkg {

struct ValidateOptions {
    /// 安装器版本；空表示跳过 installerMinVersion 门禁。
    std::string installer_version;

    /// 本机 Windows 构建号；0 表示跳过 minOsBuild 门禁。
    int64_t host_build = 0;

    /// 本机架构（x64/x86/arm64）；空表示跳过。
    std::string host_architecture;

    /// 允许的发布者 SHA-256 指纹白名单（小写）。空且 require_pinned_publisher 为 false 时不判定钉扎。
    std::vector<std::string> allowed_publisher_thumbprints;

    bool require_pinned_publisher = false;
    bool require_signature = true;
    bool validate_inner_appx = true;

    /// 安装器已经解包到磁盘的主包路径。非空时会校验它与清单声明的大小/哈希一致
    /// （防「校验通过后、安装之前被替换」的 TOCTOU），并用它做内部 Appx 校验。
    std::string extracted_main_package_path;

    /// 覆盖「当前时间」（FILETIME）；0 表示取系统时间。
    uint64_t now_filetime = 0;
};

struct Issue {
    std::string code;
    std::string message;
    std::string detail;

    [[nodiscard]] std::string to_string() const {
        std::string result = code + ": " + message;
        if (!detail.empty()) {
            result += " [" + detail + "]";
        }
        return result;
    }
};

struct Report {
    std::vector<Issue> errors;
    std::vector<Issue> warnings;

    Manifest manifest;
    bool has_manifest = false;
    Certificate publisher_certificate;
    bool has_publisher_certificate = false;
    bool publisher_was_pinned = false;
    std::string content_digest_hex;

    [[nodiscard]] bool valid() const { return errors.empty(); }
    [[nodiscard]] std::string summary() const;
};

Report validate(const std::filesystem::path& container_path, const ValidateOptions& options = ValidateOptions{});
Report validate_memory(const uint8_t* data, size_t size, const ValidateOptions& options = ValidateOptions{});

}  // namespace msixpkg
