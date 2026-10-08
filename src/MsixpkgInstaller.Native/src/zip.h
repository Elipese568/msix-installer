#pragma once

// 最小 ZIP 读取器：中央目录解析 + 条目流式读取（store / deflate）。
// 用内存映射文件访问容器：既避免把 60 MB 乃至更大的包整体拷进内存，
// 也让压缩数据在内存里完整可见，于是 DEFLATE 只需要处理「输出分段」。

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "internal.h"
#include "msixpkg/errors.h"

namespace msixpkg::zip {

/// 体积与压缩比限额。与 C# 端 MsixpkgLimits 的默认值保持一致。
struct Limits {
    uint32_t max_entry_count = 4096;
    uint64_t max_container_size = 8ull * 1024 * 1024 * 1024;
    uint64_t max_entry_size = 2ull * 1024 * 1024 * 1024;
    uint64_t max_total_uncompressed = 8ull * 1024 * 1024 * 1024;
    uint64_t ratio_guard_threshold = 16ull * 1024 * 1024;
    uint64_t max_compression_ratio = 200;
};

struct Entry {
    std::string name;
    uint16_t method = 0;  // 0 = store，8 = deflate
    uint16_t flags = 0;
    uint32_t crc = 0;
    uint64_t compressed_size = 0;
    uint64_t uncompressed_size = 0;
    uint64_t local_header_offset = 0;
    size_t index = 0;
};

/// 只读内存映射文件（RAII）。
class MappedFile {
public:
    explicit MappedFile(const std::filesystem::path& path);
    ~MappedFile();

    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;
    MappedFile(MappedFile&& other) noexcept;
    MappedFile& operator=(MappedFile&& other) noexcept;

    [[nodiscard]] const uint8_t* data() const noexcept { return data_; }
    [[nodiscard]] uint64_t size() const noexcept { return size_; }

private:
    void* file_ = nullptr;
    void* mapping_ = nullptr;
    const uint8_t* data_ = nullptr;
    uint64_t size_ = 0;
};

class Archive {
public:
    /// 从文件打开（内部用内存映射）。
    static Archive open(const std::filesystem::path& path, const Limits& limits = Limits{});

    /// 从一段已在内存中的字节打开。用于**嵌套**场景：外层容器把内层 .msix 以 store 存放，
    /// 于是内层 ZIP 就是映射文件里的一个连续区间 —— 直接借用，不需要拷贝 60 MB。
    static Archive open_memory(const uint8_t* data, size_t size, const Limits& limits = Limits{});

    [[nodiscard]] const std::vector<Entry>& entries() const noexcept { return entries_; }

    /// 精确匹配条目名（序数比较）；找不到返回 nullptr。
    [[nodiscard]] const Entry* find(std::string_view name) const;

    /// 把条目解压进内存；超过 cap 抛 E_ENTRY_TOO_LARGE。
    [[nodiscard]] Bytes read_to_memory(const Entry& entry, uint64_t cap = 64ull * 1024 * 1024) const;

    /// 流式读取（按块回调），适合对 60 MB 载荷做哈希而完全不驻留内存。
    /// 结束后会校验解压长度与 CRC-32。
    void read_stream(const Entry& entry, const std::function<void(const uint8_t*, size_t)>& sink) const;

    /// 条目的压缩字节（用于诊断与哈希；deflate 条目返回的是压缩后的字节）。
    [[nodiscard]] const uint8_t* compressed_data(const Entry& entry) const;

    /// 归档数据的基地址与长度（测试用偏移计算等）。
    [[nodiscard]] const uint8_t* base() const noexcept { return data_; }
    [[nodiscard]] uint64_t size() const noexcept { return size_; }

private:
    Archive(const uint8_t* data, uint64_t size, std::unique_ptr<MappedFile> owner)
        : owner_(std::move(owner)), data_(data), size_(size) {}

    void parse(const Limits& limits);
    uint64_t data_offset_of(const Entry& entry) const;

    std::unique_ptr<MappedFile> owner_;
    const uint8_t* data_ = nullptr;
    uint64_t size_ = 0;
    std::vector<Entry> entries_;
    std::map<std::string, size_t, std::less<>> by_name_;
};

}  // namespace msixpkg::zip
