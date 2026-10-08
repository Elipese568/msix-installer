#pragma once

// 原生核心的内部工具（不对外暴露）：字节、十六进制、宽窄字符串、小端读取、文件读取。

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "msixpkg/errors.h"

namespace msixpkg {

using Bytes = std::vector<uint8_t>;

// ---------- 十六进制 ----------

std::string to_hex_lower(const uint8_t* data, size_t size);
std::string to_hex_upper(const uint8_t* data, size_t size);

inline std::string to_hex_lower(const Bytes& bytes) { return to_hex_lower(bytes.data(), bytes.size()); }
inline std::string to_hex_upper(const Bytes& bytes) { return to_hex_upper(bytes.data(), bytes.size()); }

/// 严格解析小写或大写十六进制；奇数长度或含非法字符时返回 false。
bool try_from_hex(std::string_view text, Bytes& out);

/// 规范要求小写十六进制：非小写一律拒绝（与 C# 实现 IsHex 一致）。
bool is_lower_hex(std::string_view text, size_t expected_length);

// ---------- 字符串 ----------

bool equals_ignore_case_ascii(std::string_view left, std::string_view right);
bool starts_with(std::string_view text, std::string_view prefix);
bool ends_with(std::string_view text, std::string_view suffix);
std::string trim_ascii(std::string_view text);
std::vector<std::string> split(std::string_view text, char separator);

std::wstring widen_utf8(std::string_view text);
std::string narrow_utf8(std::wstring_view text);

/// Windows 命令行参数引用：含空格/制表/引号时按 CommandLineToArgvW 的反解析规则加引号。
/// ARP 的 UninstallString 与提权助手的参数都靠它，否则带空格的路径会被拆成两个参数。
std::wstring quote_argument(std::wstring_view value);
std::string quote_argument(std::string_view value);

// ---------- 小端读取 ----------

inline uint16_t read_le16(const uint8_t* data) {
    return static_cast<uint16_t>(data[0]) | static_cast<uint16_t>(data[1] << 8);
}

inline uint32_t read_le32(const uint8_t* data) {
    return static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8) |
           (static_cast<uint32_t>(data[2]) << 16) | (static_cast<uint32_t>(data[3]) << 24);
}

// ---------- 文件 ----------

Bytes read_file(const std::filesystem::path& path);
void write_file(const std::filesystem::path& path, const uint8_t* data, size_t size);
inline void write_file(const std::filesystem::path& path, const Bytes& bytes) {
    write_file(path, bytes.data(), bytes.size());
}

}  // namespace msixpkg
