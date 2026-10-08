#include "zip.h"

#include <Windows.h>

#include <cstring>
#include <limits>

#include "inflate.h"

namespace msixpkg::zip {
namespace {

constexpr uint32_t kEndOfCentralDirectorySignature = 0x06054B50u;   // PK\x05\x06
constexpr uint32_t kZip64LocatorSignature = 0x07064B50u;           // PK\x06\x07
constexpr uint32_t kZip64EndSignature = 0x06064B50u;               // PK\x06\x06
constexpr uint32_t kCentralDirectorySignature = 0x02014B50u;       // PK\x01\x02
constexpr uint32_t kLocalHeaderSignature = 0x04034B50u;            // PK\x03\x04

constexpr uint64_t kEndOfCentralDirectorySize = 22;
constexpr uint64_t kMaxCommentSize = 0xFFFF;

[[noreturn]] void invalid(const std::string& message, const std::string& detail = {}) {
    fail(codes::ContainerInvalid, message, detail);
}

struct EndOfCentralDirectory {
    uint64_t entry_count = 0;
    uint64_t directory_offset = 0;
    uint64_t directory_size = 0;
};

bool read_zip64_extra(const uint8_t* extra, size_t extra_size,
                      uint64_t& uncompressed, uint64_t& compressed, uint64_t& offset,
                      bool need_uncompressed, bool need_compressed, bool need_offset) {
    size_t position = 0;
    while (position + 4 <= extra_size) {
        const uint16_t header_id = read_le16(extra + position);
        const uint16_t data_size = read_le16(extra + position + 2);
        position += 4;
        if (position + data_size > extra_size) {
            return false;
        }

        if (header_id == 0x0001) {
            size_t cursor = position;
            const size_t end = position + data_size;
            const auto take = [&](uint64_t& target) {
                if (cursor + 8 > end) {
                    return false;
                }
                target = static_cast<uint64_t>(read_le32(extra + cursor)) |
                         (static_cast<uint64_t>(read_le32(extra + cursor + 4)) << 32);
                cursor += 8;
                return true;
            };

            if (need_uncompressed && !take(uncompressed)) return false;
            if (need_compressed && !take(compressed)) return false;
            if (need_offset && !take(offset)) return false;
            return true;
        }

        position += data_size;
    }

    return false;
}

}  // namespace

MappedFile::MappedFile(const std::filesystem::path& path) {
    const std::wstring wide = path.wstring();
    HANDLE file = ::CreateFileW(wide.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        fail(codes::IoError, "The container file could not be opened.", path.string());
    }

    LARGE_INTEGER size{};
    if (!::GetFileSizeEx(file, &size)) {
        ::CloseHandle(file);
        fail(codes::IoError, "The container file size could not be read.", path.string());
    }

    if (size.QuadPart == 0) {
        ::CloseHandle(file);
        fail(codes::ContainerEmpty, "The container file is empty.", path.string());
    }

    HANDLE mapping = ::CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (mapping == nullptr) {
        ::CloseHandle(file);
        fail(codes::IoError, "The container file could not be mapped.", path.string());
    }

    const void* view = ::MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
    if (view == nullptr) {
        ::CloseHandle(mapping);
        ::CloseHandle(file);
        fail(codes::IoError, "The container file view could not be mapped.", path.string());
    }

    file_ = file;
    mapping_ = mapping;
    data_ = static_cast<const uint8_t*>(view);
    size_ = static_cast<uint64_t>(size.QuadPart);
}

MappedFile::MappedFile(MappedFile&& other) noexcept
    : file_(other.file_), mapping_(other.mapping_), data_(other.data_), size_(other.size_) {
    other.file_ = nullptr;
    other.mapping_ = nullptr;
    other.data_ = nullptr;
    other.size_ = 0;
}

MappedFile& MappedFile::operator=(MappedFile&& other) noexcept {
    if (this != &other) {
        if (data_ != nullptr) {
            ::UnmapViewOfFile(data_);
        }
        if (mapping_ != nullptr) {
            ::CloseHandle(static_cast<HANDLE>(mapping_));
        }
        if (file_ != nullptr) {
            ::CloseHandle(static_cast<HANDLE>(file_));
        }

        file_ = other.file_;
        mapping_ = other.mapping_;
        data_ = other.data_;
        size_ = other.size_;

        other.file_ = nullptr;
        other.mapping_ = nullptr;
        other.data_ = nullptr;
        other.size_ = 0;
    }

    return *this;
}

MappedFile::~MappedFile() {
    if (data_ != nullptr) {
        ::UnmapViewOfFile(data_);
    }
    if (mapping_ != nullptr) {
        ::CloseHandle(static_cast<HANDLE>(mapping_));
    }
    if (file_ != nullptr) {
        ::CloseHandle(static_cast<HANDLE>(file_));
    }
}

Archive Archive::open(const std::filesystem::path& path, const Limits& limits) {
    auto mapped = std::make_unique<MappedFile>(path);

    // 必须先把裸指针/长度取出来再移交所有权：函数实参求值顺序未定义，
    // 而按值传递的 unique_ptr 形参会在实参求值阶段就完成移动 ——
    // 若先移动再取 mapped->data()，就是在空指针上取值（实测为 0xC0000005）。
    const uint8_t* data = mapped->data();
    const uint64_t size = mapped->size();

    Archive archive(data, size, std::move(mapped));
    archive.parse(limits);
    return archive;
}

Archive Archive::open_memory(const uint8_t* data, size_t size, const Limits& limits) {
    if (data == nullptr || size == 0) {
        fail(codes::ContainerEmpty, "The container buffer is empty.");
    }

    Archive archive(data, size, nullptr);
    archive.parse(limits);
    return archive;
}

void Archive::parse(const Limits& limits) {
    const uint8_t* data = data_;
    const uint64_t size = size_;

    if (size > limits.max_container_size) {
        fail(codes::ContainerTooLarge, "The container exceeds the maximum allowed size.",
             std::to_string(size));
    }

    if (size < kEndOfCentralDirectorySize) {
        invalid("The file is too small to be a ZIP archive.");
    }

    // ---- 定位 EOCD（从尾部向前搜索，注释最长 64 KiB）----
    const uint64_t search_span = (size < kEndOfCentralDirectorySize + kMaxCommentSize)
                                     ? size
                                     : kEndOfCentralDirectorySize + kMaxCommentSize;
    uint64_t eocd_offset = 0;
    bool found = false;
    for (uint64_t i = size - kEndOfCentralDirectorySize + 1; i-- > size - search_span;) {
        if (read_le32(data + i) == kEndOfCentralDirectorySignature) {
            eocd_offset = i;
            found = true;
            break;
        }
        if (i == 0) {
            break;
        }
    }

    if (!found) {
        invalid("No ZIP end-of-central-directory record was found.");
    }

    const uint16_t disk_number = read_le16(data + eocd_offset + 4);
    const uint16_t directory_disk = read_le16(data + eocd_offset + 6);
    const uint64_t entry_count_raw = read_le16(data + eocd_offset + 10);
    const uint64_t directory_size_raw = read_le32(data + eocd_offset + 12);
    const uint64_t directory_offset_raw = read_le32(data + eocd_offset + 16);

    if (disk_number != 0 || directory_disk != 0) {
        invalid("Multi-disk ZIP archives are not supported.");
    }

    EndOfCentralDirectory end;
    end.entry_count = entry_count_raw;
    end.directory_size = directory_size_raw;
    end.directory_offset = directory_offset_raw;

    const bool needs_zip64 = entry_count_raw == 0xFFFF || directory_size_raw == 0xFFFFFFFFull ||
                             directory_offset_raw == 0xFFFFFFFFull;

    if (needs_zip64) {
        if (eocd_offset < 20) {
            invalid("A ZIP64 archive has no ZIP64 end-of-central-directory locator.");
        }

        const uint64_t locator_offset = eocd_offset - 20;
        if (read_le32(data + locator_offset) != kZip64LocatorSignature) {
            invalid("A ZIP64 archive has no ZIP64 end-of-central-directory locator.");
        }

        const uint64_t zip64_offset = static_cast<uint64_t>(read_le32(data + locator_offset + 8)) |
                                      (static_cast<uint64_t>(read_le32(data + locator_offset + 12)) << 32);

        if (zip64_offset + 56 > size || read_le32(data + zip64_offset) != kZip64EndSignature) {
            invalid("The ZIP64 end-of-central-directory record is missing or truncated.");
        }

        end.entry_count = static_cast<uint64_t>(read_le32(data + zip64_offset + 32)) |
                          (static_cast<uint64_t>(read_le32(data + zip64_offset + 36)) << 32);
        end.directory_size = static_cast<uint64_t>(read_le32(data + zip64_offset + 40)) |
                             (static_cast<uint64_t>(read_le32(data + zip64_offset + 44)) << 32);
        end.directory_offset = static_cast<uint64_t>(read_le32(data + zip64_offset + 48)) |
                               (static_cast<uint64_t>(read_le32(data + zip64_offset + 52)) << 32);
    }

    if (end.entry_count > limits.max_entry_count) {
        fail(codes::EntryCountExceeded, "The container contains too many entries.",
             std::to_string(end.entry_count));
    }

    if (end.directory_offset > size || end.directory_size > size - end.directory_offset) {
        invalid("The central directory lies outside the file.");
    }

    // ---- 中央目录 ----
    uint64_t cursor = end.directory_offset;
    uint64_t total_uncompressed = 0;
    entries_.reserve(static_cast<size_t>(end.entry_count));

    for (uint64_t i = 0; i < end.entry_count; ++i) {
        if (cursor + 46 > size || read_le32(data + cursor) != kCentralDirectorySignature) {
            invalid("A central directory entry is missing or truncated.", std::to_string(i));
        }

        Entry entry;
        entry.index = static_cast<size_t>(i);
        entry.flags = read_le16(data + cursor + 8);
        entry.method = read_le16(data + cursor + 10);
        entry.crc = read_le32(data + cursor + 16);
        uint64_t compressed = read_le32(data + cursor + 20);
        uint64_t uncompressed = read_le32(data + cursor + 24);
        const uint16_t name_length = read_le16(data + cursor + 28);
        const uint16_t extra_length = read_le16(data + cursor + 30);
        const uint16_t comment_length = read_le16(data + cursor + 32);
        uint64_t local_offset = read_le32(data + cursor + 42);

        if (cursor + 46ull + name_length + extra_length + comment_length > size) {
            invalid("A central directory entry overruns the file.", std::to_string(i));
        }

        entry.name.assign(reinterpret_cast<const char*>(data + cursor + 46), name_length);

        const bool need_uncompressed = uncompressed == 0xFFFFFFFFull;
        const bool need_compressed = compressed == 0xFFFFFFFFull;
        const bool need_offset = local_offset == 0xFFFFFFFFull;

        if (need_uncompressed || need_compressed || need_offset) {
            const uint8_t* extra = data + cursor + 46 + name_length;
            if (!read_zip64_extra(extra, extra_length, uncompressed, compressed, local_offset,
                                  need_uncompressed, need_compressed, need_offset)) {
                invalid("A ZIP64 extra field is missing or malformed.", entry.name);
            }
        }

        entry.compressed_size = compressed;
        entry.uncompressed_size = uncompressed;
        entry.local_header_offset = local_offset;

        if (entry.uncompressed_size > limits.max_entry_size) {
            fail(codes::EntryTooLarge, "A container entry exceeds the maximum allowed size.", entry.name);
        }

        if (entry.uncompressed_size >= limits.ratio_guard_threshold && entry.compressed_size > 0 &&
            entry.uncompressed_size / entry.compressed_size > limits.max_compression_ratio) {
            fail(codes::EntryRatioExceeded,
                 "A container entry exceeds the maximum allowed compression ratio.", entry.name);
        }

        total_uncompressed += entry.uncompressed_size;
        if (total_uncompressed > limits.max_total_uncompressed) {
            fail(codes::TotalSizeExceeded, "The container expands beyond the maximum allowed total size.");
        }

        if (entry.method != 0 && entry.method != 8) {
            invalid("A container entry uses an unsupported compression method.",
                    entry.name + " (method " + std::to_string(entry.method) + ")");
        }

        if ((entry.flags & 0x0001) != 0) {
            invalid("Encrypted container entries are not supported.", entry.name);
        }

        if (entry.local_header_offset + 30 > size) {
            invalid("A local file header lies outside the file.", entry.name);
        }

        entries_.push_back(std::move(entry));
        cursor += 46ull + name_length + extra_length + comment_length;
    }

    for (size_t i = 0; i < entries_.size(); ++i) {
        by_name_.emplace(entries_[i].name, i);
    }
}

const Entry* Archive::find(std::string_view name) const {
    const auto position = by_name_.find(name);
    if (position == by_name_.end()) {
        return nullptr;
    }
    return &entries_[position->second];
}

uint64_t Archive::data_offset_of(const Entry& entry) const {
    const uint8_t* data = data_;
    const uint64_t size = size_;
    const uint64_t header = entry.local_header_offset;

    if (header + 30 > size || read_le32(data + header) != kLocalHeaderSignature) {
        invalid("A local file header is missing or truncated.", entry.name);
    }

    const uint16_t name_length = read_le16(data + header + 26);
    const uint16_t extra_length = read_le16(data + header + 28);
    const uint64_t offset = header + 30ull + name_length + extra_length;

    if (offset > size || entry.compressed_size > size - offset) {
        invalid("Entry data overruns the file.", entry.name);
    }

    return offset;
}

const uint8_t* Archive::compressed_data(const Entry& entry) const {
    return data_ + data_offset_of(entry);
}

Bytes Archive::read_to_memory(const Entry& entry, uint64_t cap) const {
    if (entry.uncompressed_size > cap) {
        fail(codes::EntryTooLarge, "The entry is too large to read into memory.", entry.name);
    }

    Bytes result;
    result.reserve(static_cast<size_t>(entry.uncompressed_size));
    read_stream(entry, [&result](const uint8_t* data, size_t size) {
        result.insert(result.end(), data, data + size);
    });
    return result;
}

void Archive::read_stream(const Entry& entry, const std::function<void(const uint8_t*, size_t)>& sink) const {
    const uint64_t offset = data_offset_of(entry);
    const uint8_t* compressed = data_ + offset;
    const uint64_t compressed_size = entry.compressed_size;

    uint64_t produced = 0;
    uint32_t running_crc = 0;

    const auto consume = [&](const uint8_t* data, size_t size) {
        running_crc = crc32(data, size, running_crc);
        produced += size;
        if (produced > entry.uncompressed_size) {
            invalid("A container entry produced more data than its declared size.", entry.name);
        }
        sink(data, size);
    };

    if (entry.method == 0) {
        if (compressed_size != entry.uncompressed_size) {
            invalid("A stored entry has mismatching compressed and uncompressed sizes.", entry.name);
        }
        if (compressed_size > 0) {
            consume(compressed, static_cast<size_t>(compressed_size));
        }
    } else {
        Inflater inflater(compressed, static_cast<size_t>(compressed_size), consume);
        inflater.run();
    }

    if (produced != entry.uncompressed_size) {
        invalid("A container entry produced a different size than declared.", entry.name);
    }

    if (running_crc != entry.crc) {
        invalid("A container entry failed its CRC-32 check.",
                entry.name + " (declared " + std::to_string(entry.crc) +
                    ", computed " + std::to_string(running_crc) + ")");
    }
}

}  // namespace msixpkg::zip
