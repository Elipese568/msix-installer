#include "setup_payload.h"

#include "setup_sha256.h"
#include "setup_util.h"

#include <algorithm>
#include <cstring>

namespace msixpkg::setup {
namespace {

constexpr char kMagic[8] = {'M', 'S', 'X', 'S', 'E', 'T', 'U', 'P'};
constexpr size_t kFooterMagicOffset = 0;
constexpr size_t kFooterVersionOffset = 8;
constexpr size_t kFooterFlagsOffset = 12;
constexpr size_t kFooterIndexOffset = 16;
constexpr size_t kFooterIndexSize = 24;
constexpr size_t kFooterPayloadSize = 32;
constexpr size_t kFooterReserved = 40;
constexpr size_t kFooterDigest = 48;

constexpr uint64_t kMaxIndexSize = 16u << 20;      // 16 MiB，条目名列表不可能更大
constexpr uint64_t kMaxPayloadSize = 2ull << 30;   // 2 GiB，setup 的载荷远小于这个

uint16_t read_u16(const uint8_t* data) {
    return static_cast<uint16_t>(data[0] | (data[1] << 8));
}

uint32_t read_u32(const uint8_t* data) {
    return static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8) |
           (static_cast<uint32_t>(data[2]) << 16) | (static_cast<uint32_t>(data[3]) << 24);
}

uint64_t read_u64(const uint8_t* data) {
    uint64_t value = 0;
    for (int index = 7; index >= 0; --index) {
        value = (value << 8) | data[index];
    }
    return value;
}

bool valid_entry_name(std::string_view name) {
    if (name.empty() || name.size() > 240) {
        return false;
    }
    for (const char ch : name) {
        if (ch == '/' || ch == '\\' || ch == ':' || ch == '\0' || ch < 0x20) {
            return false;
        }
    }
    return name != "." && name != "..";
}

}  // namespace

Payload::~Payload() {
    close();
}

void Payload::close() {
    if (file_ != INVALID_HANDLE_VALUE) {
        ::CloseHandle(file_);
        file_ = INVALID_HANDLE_VALUE;
    }
    entries_.clear();
}

bool Payload::open(const std::filesystem::path& executable, std::wstring& error) {
    close();
    file_ = ::CreateFileW(executable.c_str(), GENERIC_READ,
                          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                          OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file_ == INVALID_HANDLE_VALUE) {
        error = L"CreateFileW(" + executable.wstring() + L"): " + format_win32_error(::GetLastError());
        return false;
    }

    LARGE_INTEGER size{};
    if (!::GetFileSizeEx(file_, &size) || size.QuadPart < static_cast<LONGLONG>(kPayloadFooterSize)) {
        error = L"file is too small to carry a setup payload: " + executable.wstring();
        close();
        return false;
    }
    const uint64_t file_size = static_cast<uint64_t>(size.QuadPart);

    // Authenticode 会在文件末尾追加证书表。footer 不再位于 EOF-80，而是证书表之前；
    // 因此从文件尾部向前找魔数，并用索引摘要排除证书表里的偶然匹配。
    const uint64_t scan_size = std::min<uint64_t>(file_size, 16ull << 20);
    std::vector<uint8_t> tail(static_cast<size_t>(scan_size));
    if (!read_at(file_size - scan_size, tail.data(), tail.size(), error)) {
        close();
        return false;
    }

    uint64_t index_offset = 0;
    uint64_t index_size = 0;
    uint64_t payload_size = 0;
    std::vector<uint8_t> index;
    bool found = false;
    for (size_t cursor = tail.size(); cursor >= kPayloadFooterSize; --cursor) {
        const size_t offset = cursor - kPayloadFooterSize;
        if (std::memcmp(tail.data() + offset, kMagic, sizeof(kMagic)) != 0) {
            if (cursor == kPayloadFooterSize) {
                break;
            }
            continue;
        }

        const uint8_t* candidate = tail.data() + offset;
        const uint64_t candidate_offset = file_size - scan_size + offset;
        const uint32_t version = read_u32(candidate + kFooterVersionOffset);
        const uint64_t candidate_index_offset = read_u64(candidate + kFooterIndexOffset);
        const uint64_t candidate_index_size = read_u64(candidate + kFooterIndexSize);
        const uint64_t candidate_payload_size = read_u64(candidate + kFooterPayloadSize);
        if (version == kPayloadFormatVersion && candidate_index_size > 0 &&
            candidate_index_size <= kMaxIndexSize && candidate_payload_size <= kMaxPayloadSize &&
            candidate_index_offset >= candidate_payload_size &&
            candidate_index_offset + candidate_index_size == candidate_offset &&
            candidate_offset >= candidate_payload_size) {
            std::vector<uint8_t> candidate_index(static_cast<size_t>(candidate_index_size));
            if (!read_at(candidate_index_offset, candidate_index.data(), candidate_index.size(), error)) {
                if (cursor == kPayloadFooterSize) {
                    break;
                }
                continue;
            }
            std::array<uint8_t, 32> expected_digest{};
            std::memcpy(expected_digest.data(), candidate + kFooterDigest, expected_digest.size());
            if (sha256_of(candidate_index.data(), candidate_index.size()) == expected_digest) {
                index_offset = candidate_index_offset;
                index_size = candidate_index_size;
                payload_size = candidate_payload_size;
                index = std::move(candidate_index);
                found = true;
                break;
            }
        }
        if (cursor == kPayloadFooterSize) {
            break;
        }
    }

    if (!found) {
        error = L"setup payload footer is missing (not a packed setup exe?)";
        close();
        return false;
    }
    (void)index_offset;
    (void)index_size;

    if (index.size() < 4) {
        error = L"setup payload index is truncated";
        close();
        return false;
    }
    const uint32_t count = read_u32(index.data());
    if (count == 0 || count > 64) {
        error = L"setup payload entry count is invalid: " + std::to_wstring(count);
        close();
        return false;
    }

    size_t cursor = 4;
    uint64_t summed_size = 0;
    entries_.reserve(count);
    for (uint32_t item = 0; item < count; ++item) {
        if (cursor + 2 > index.size()) {
            error = L"setup payload index entry is truncated";
            close();
            return false;
        }
        const uint16_t name_size = read_u16(index.data() + cursor);
        cursor += 2;
        if (name_size == 0 || cursor + name_size + 8 + 8 + 32 > index.size()) {
            error = L"setup payload entry name is invalid";
            close();
            return false;
        }

        PayloadEntry entry;
        entry.name.assign(reinterpret_cast<const char*>(index.data() + cursor), name_size);
        cursor += name_size;
        entry.offset = read_u64(index.data() + cursor);
        cursor += 8;
        entry.size = read_u64(index.data() + cursor);
        cursor += 8;
        std::memcpy(entry.sha256.data(), index.data() + cursor, entry.sha256.size());
        cursor += entry.sha256.size();

        if (!valid_entry_name(entry.name) || entry.offset < index_offset - payload_size ||
            entry.size == 0 || entry.offset + entry.size > index_offset) {
            error = L"setup payload entry is out of range: " + to_utf16(entry.name);
            close();
            return false;
        }
        if (find(entry.name) != nullptr) {
            error = L"setup payload has a duplicate entry: " + to_utf16(entry.name);
            close();
            return false;
        }
        summed_size += entry.size;
        entries_.push_back(std::move(entry));
    }

    if (cursor != index.size()) {
        error = L"setup payload index has trailing bytes";
        close();
        return false;
    }
    if (summed_size != payload_size) {
        error = L"setup payload size does not match its entries";
        close();
        return false;
    }

    error.clear();
    return true;
}

const PayloadEntry* Payload::find(std::string_view name) const {
    for (const PayloadEntry& entry : entries_) {
        if (entry.name == name) {
            return &entry;
        }
    }
    return nullptr;
}

uint64_t Payload::total_size() const {
    uint64_t total = 0;
    for (const PayloadEntry& entry : entries_) {
        total += entry.size;
    }
    return total;
}

bool Payload::read_at(uint64_t offset, void* buffer, size_t size, std::wstring& error) const {
    if (file_ == INVALID_HANDLE_VALUE) {
        error = L"payload is not open";
        return false;
    }
    LARGE_INTEGER position{};
    position.QuadPart = static_cast<LONGLONG>(offset);
    if (!::SetFilePointerEx(file_, position, nullptr, FILE_BEGIN)) {
        error = L"SetFilePointerEx failed: " + format_win32_error(::GetLastError());
        return false;
    }

    auto* bytes = static_cast<uint8_t*>(buffer);
    size_t done = 0;
    while (done < size) {
        DWORD chunk = 0;
        const DWORD wanted = static_cast<DWORD>(std::min<size_t>(size - done, 1u << 20));
        if (!::ReadFile(file_, bytes + done, wanted, &chunk, nullptr) || chunk == 0) {
            error = L"ReadFile failed: " + format_win32_error(::GetLastError());
            return false;
        }
        done += chunk;
    }
    return true;
}

bool Payload::hash_entry(const PayloadEntry& entry, std::array<uint8_t, 32>& digest,
                         std::wstring& error) const {
    if (file_ == INVALID_HANDLE_VALUE) {
        error = L"payload is not open";
        return false;
    }
    LARGE_INTEGER position{};
    position.QuadPart = static_cast<LONGLONG>(entry.offset);
    if (!::SetFilePointerEx(file_, position, nullptr, FILE_BEGIN)) {
        error = L"SetFilePointerEx failed: " + format_win32_error(::GetLastError());
        return false;
    }

    Sha256 hash;
    if (!hash.valid()) {
        error = L"BCrypt SHA-256 is unavailable";
        return false;
    }

    std::vector<uint8_t> buffer(64 * 1024);
    uint64_t remaining = entry.size;
    while (remaining > 0) {
        const DWORD wanted = static_cast<DWORD>(std::min<uint64_t>(remaining, buffer.size()));
        DWORD got = 0;
        if (!::ReadFile(file_, buffer.data(), wanted, &got, nullptr) || got == 0) {
            error = L"ReadFile failed: " + format_win32_error(::GetLastError());
            return false;
        }
        if (!hash.update(buffer.data(), got)) {
            error = L"BCryptHashData failed";
            return false;
        }
        remaining -= got;
    }
    digest = hash.finish();
    return true;
}

bool Payload::verify_entry(const PayloadEntry& entry, std::wstring& error) const {
    std::array<uint8_t, 32> digest{};
    if (!hash_entry(entry, digest, error)) {
        return false;
    }
    if (digest != entry.sha256) {
        error = L"payload entry digest mismatch: " + to_utf16(entry.name);
        return false;
    }
    return true;
}

bool Payload::verify_all(std::vector<std::wstring>& report, std::wstring& error) const {
    report.clear();
    for (const PayloadEntry& entry : entries_) {
        std::array<uint8_t, 32> digest{};
        if (!hash_entry(entry, digest, error)) {
            return false;
        }
        if (digest != entry.sha256) {
            error = L"payload entry digest mismatch: " + to_utf16(entry.name);
            return false;
        }
        report.push_back(to_utf16(entry.name) + L"  " + std::to_wstring(entry.size) + L" bytes  " +
                         to_hex(digest));
    }
    return true;
}

bool Payload::extract_to(const PayloadEntry& entry, const std::filesystem::path& target,
                         std::wstring& error) const {
    std::error_code code;
    const std::filesystem::path parent = target.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, code);
        if (code) {
            error = L"cannot create " + parent.wstring() + L": " + to_utf16(code.message());
            return false;
        }
    }

    const std::filesystem::path temporary = target.wstring() + L".msixpkg-tmp";
    const HANDLE output = ::CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (output == INVALID_HANDLE_VALUE) {
        error = L"CreateFileW(" + temporary.wstring() + L"): " + format_win32_error(::GetLastError());
        return false;
    }

    LARGE_INTEGER position{};
    position.QuadPart = static_cast<LONGLONG>(entry.offset);
    if (!::SetFilePointerEx(file_, position, nullptr, FILE_BEGIN)) {
        error = L"SetFilePointerEx failed: " + format_win32_error(::GetLastError());
        ::CloseHandle(output);
        ::DeleteFileW(temporary.c_str());
        return false;
    }

    Sha256 hash;
    std::vector<uint8_t> buffer(64 * 1024);
    uint64_t remaining = entry.size;
    bool ok = hash.valid();
    while (ok && remaining > 0) {
        const DWORD wanted = static_cast<DWORD>(std::min<uint64_t>(remaining, buffer.size()));
        DWORD got = 0;
        if (!::ReadFile(file_, buffer.data(), wanted, &got, nullptr) || got == 0) {
            error = L"ReadFile failed: " + format_win32_error(::GetLastError());
            ok = false;
            break;
        }
        DWORD written = 0;
        if (!::WriteFile(output, buffer.data(), got, &written, nullptr) || written != got) {
            error = L"WriteFile(" + temporary.wstring() + L"): " + format_win32_error(::GetLastError());
            ok = false;
            break;
        }
        if (!hash.update(buffer.data(), got)) {
            error = L"BCryptHashData failed";
            ok = false;
            break;
        }
        remaining -= got;
    }

    if (ok) {
        ::FlushFileBuffers(output);
    }
    ::CloseHandle(output);

    if (!ok) {
        ::DeleteFileW(temporary.c_str());
        return false;
    }

    if (hash.finish() != entry.sha256) {
        ::DeleteFileW(temporary.c_str());
        error = L"payload entry digest mismatch: " + to_utf16(entry.name);
        return false;
    }

    if (!::MoveFileExW(temporary.c_str(), target.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error = L"MoveFileExW(" + target.wstring() + L"): " + format_win32_error(::GetLastError());
        ::DeleteFileW(temporary.c_str());
        return false;
    }
    return true;
}

}  // namespace msixpkg::setup
