#include "internal.h"

#include <Windows.h>

#include <algorithm>
#include <cstdio>
#include <fstream>

namespace msixpkg {
namespace {

constexpr char kHexDigitsLower[] = "0123456789abcdef";
constexpr char kHexDigitsUpper[] = "0123456789ABCDEF";

int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string to_hex(const uint8_t* data, size_t size, const char* digits) {
    std::string result;
    result.reserve(size * 2);
    for (size_t i = 0; i < size; ++i) {
        result.push_back(digits[(data[i] >> 4) & 0x0F]);
        result.push_back(digits[data[i] & 0x0F]);
    }
    return result;
}

}  // namespace

std::string to_hex_lower(const uint8_t* data, size_t size) {
    return to_hex(data, size, kHexDigitsLower);
}

std::string to_hex_upper(const uint8_t* data, size_t size) {
    return to_hex(data, size, kHexDigitsUpper);
}

bool try_from_hex(std::string_view text, Bytes& out) {
    if (text.size() % 2 != 0) {
        return false;
    }

    out.clear();
    out.reserve(text.size() / 2);
    for (size_t i = 0; i < text.size(); i += 2) {
        const int high = hex_digit(text[i]);
        const int low = hex_digit(text[i + 1]);
        if (high < 0 || low < 0) {
            return false;
        }
        out.push_back(static_cast<uint8_t>((high << 4) | low));
    }

    return true;
}

bool is_lower_hex(std::string_view text, size_t expected_length) {
    if (text.size() != expected_length) {
        return false;
    }

    // 规范要求小写：'A'-'F' 一律视为非法，与 C# 端 MsixpkgManifest.IsHex 一致。
    return std::all_of(text.begin(), text.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

bool equals_ignore_case_ascii(std::string_view left, std::string_view right) {
    if (left.size() != right.size()) {
        return false;
    }

    for (size_t i = 0; i < left.size(); ++i) {
        char a = left[i];
        char b = right[i];
        if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
        if (a != b) {
            return false;
        }
    }

    return true;
}

bool starts_with(std::string_view text, std::string_view prefix) {
    return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

bool ends_with(std::string_view text, std::string_view suffix) {
    return text.size() >= suffix.size() &&
           text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string trim_ascii(std::string_view text) {
    size_t begin = 0;
    size_t end = text.size();
    while (begin < end && (text[begin] == ' ' || text[begin] == '\t' || text[begin] == '\r' || text[begin] == '\n')) {
        ++begin;
    }
    while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '\r' || text[end - 1] == '\n')) {
        --end;
    }
    return std::string(text.substr(begin, end - begin));
}

std::vector<std::string> split(std::string_view text, char separator) {
    std::vector<std::string> parts;
    size_t start = 0;
    while (true) {
        const size_t position = text.find(separator, start);
        if (position == std::string_view::npos) {
            parts.emplace_back(text.substr(start));
            return parts;
        }
        parts.emplace_back(text.substr(start, position - start));
        start = position + 1;
    }
}

std::wstring widen_utf8(std::string_view text) {
    if (text.empty()) {
        return {};
    }

    const int length = ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0) {
        return {};
    }

    std::wstring result(static_cast<size_t>(length), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), length);
    return result;
}

std::string narrow_utf8(std::wstring_view text) {
    if (text.empty()) {
        return {};
    }

    const int length = ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0) {
        return {};
    }

    std::string result(static_cast<size_t>(length), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), length, nullptr, nullptr);
    return result;
}

std::wstring quote_argument(std::wstring_view value) {
    const bool needs_quoting =
        value.empty() || value.find_first_of(L" \t\n\v\"") != std::wstring_view::npos;
    if (!needs_quoting) {
        return std::wstring(value);
    }

    // CommandLineToArgvW 的反解析规则：反斜杠只在引号前成对翻倍。
    std::wstring result;
    result.push_back(L'"');
    for (auto cursor = value.begin();; ++cursor) {
        size_t backslashes = 0;
        while (cursor != value.end() && *cursor == L'\\') {
            ++cursor;
            ++backslashes;
        }

        if (cursor == value.end()) {
            result.append(backslashes * 2, L'\\');
            break;
        }

        if (*cursor == L'"') {
            result.append(backslashes * 2 + 1, L'\\');
            result.push_back(L'"');
            continue;
        }

        result.append(backslashes, L'\\');
        result.push_back(*cursor);
    }

    result.push_back(L'"');
    return result;
}

std::string quote_argument(std::string_view value) {
    return narrow_utf8(quote_argument(widen_utf8(value)));
}

Bytes read_file(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        fail(codes::IoError, "The file could not be opened.", path.string());
    }

    stream.seekg(0, std::ios::end);
    const std::streamoff size = stream.tellg();
    if (size < 0) {
        fail(codes::IoError, "The file size could not be determined.", path.string());
    }

    stream.seekg(0, std::ios::beg);
    Bytes bytes(static_cast<size_t>(size));
    if (!bytes.empty() && !stream.read(reinterpret_cast<char*>(bytes.data()), size)) {
        fail(codes::IoError, "The file could not be read.", path.string());
    }

    return bytes;
}

void write_file(const std::filesystem::path& path, const uint8_t* data, size_t size) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) {
        fail(codes::IoError, "The file could not be created.", path.string());
    }

    if (size > 0) {
        stream.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
    }

    if (!stream) {
        fail(codes::IoError, "The file could not be written.", path.string());
    }
}

}  // namespace msixpkg
