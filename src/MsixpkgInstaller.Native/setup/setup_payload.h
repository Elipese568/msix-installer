#pragma once

// setup exe 的**自附加载荷**（payload）：把向导、CLI 与 Uninstall.exe 直接追加在
// setup exe 末尾，运行时从自己的文件里按偏移读出。
//
// 为什么不用 PE 资源：资源要在编译期把载荷路径塞进 .rc，构建顺序与增量构建都变脆；
// 追加归档只需要「先编 exe、再 pack」两步，而且签名必须发生在追加之后 —— 顺序天然正确。
//
// 布局（小端）：
//   [原 exe 字节][entry0][entry1]…[index][footer 80]
// 注意：签名后的文件在 footer 之后还有 Authenticode 证书表，所以读取端从文件尾部
// 向前找 "MSXSETUP" 魔数，并用索引摘要排除证书表里的偶然匹配；绝不能假设 footer
// 就在 EOF-80。
// footer：
//   0  char magic[8]      "MSXSETUP"
//   8  u32  version       1
//   12 u32  flags         0
//   16 u64  index_offset  （绝对偏移）
//   24 u64  index_size
//   32 u64  payload_size  （所有 entry 的 size 之和）
//   40 u64  reserved      0
//   48 u8   digest[32]    SHA-256(index)
// index：
//   u32 count
//   count × { u16 name_size; name[utf8]; u64 offset; u64 size; u8 sha256[32] }

#include <Windows.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace msixpkg::setup {

inline constexpr size_t kPayloadFooterSize = 80;
inline constexpr uint32_t kPayloadFormatVersion = 1;

struct PayloadEntry {
    std::string name;
    uint64_t offset = 0;
    uint64_t size = 0;
    std::array<uint8_t, 32> sha256{};
};

class Payload {
public:
    Payload() = default;
    ~Payload();

    Payload(const Payload&) = delete;
    Payload& operator=(const Payload&) = delete;

    /// 打开并完整校验 footer/index（不读 entry 正文）。
    bool open(const std::filesystem::path& executable, std::wstring& error);
    void close();

    bool is_open() const { return file_ != INVALID_HANDLE_VALUE; }
    const std::vector<PayloadEntry>& entries() const { return entries_; }
    const PayloadEntry* find(std::string_view name) const;
    uint64_t total_size() const;

    /// 逐字节校验某个 entry 的 SHA-256。
    bool verify_entry(const PayloadEntry& entry, std::wstring& error) const;

    /// 校验全部 entry，并返回可读的结果（每条一行）。
    bool verify_all(std::vector<std::wstring>& report, std::wstring& error) const;

    /// 原子写入目标：先写 `<target>.msixpkg-tmp`，校验哈希、flush，再 MoveFileEx 覆盖。
    bool extract_to(const PayloadEntry& entry, const std::filesystem::path& target,
                    std::wstring& error) const;

private:
    bool read_at(uint64_t offset, void* buffer, size_t size, std::wstring& error) const;
    bool hash_entry(const PayloadEntry& entry, std::array<uint8_t, 32>& digest,
                    std::wstring& error) const;

    HANDLE file_ = INVALID_HANDLE_VALUE;
    std::vector<PayloadEntry> entries_;
};

}  // namespace msixpkg::setup
