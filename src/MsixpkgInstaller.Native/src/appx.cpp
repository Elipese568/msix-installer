#include "appx.h"

#include <algorithm>
#include <cctype>
#include <map>

namespace msixpkg {
namespace {

constexpr std::string_view kManifestEntry = "AppxManifest.xml";
constexpr std::string_view kSignatureEntry = "AppxSignature.p7x";

/// 把 XML 注释替换成空格，避免注释里的伪元素被扫描到。注释之间的换行保留。
std::string strip_comments(std::string_view text) {
    std::string result(text);
    size_t position = 0;
    while (true) {
        const size_t begin = result.find("<!--", position);
        if (begin == std::string::npos) {
            return result;
        }
        const size_t end = result.find("-->", begin + 4);
        if (end == std::string::npos) {
            return result;
        }
        std::fill(result.begin() + static_cast<std::ptrdiff_t>(begin),
                  result.begin() + static_cast<std::ptrdiff_t>(end + 3), ' ');
        position = end + 3;
    }
}

/// 取出 `<element ...>` 开标签内部的属性表。找不到元素返回空 map。
std::map<std::string, std::string> scan_element_attributes(std::string_view text, std::string_view element) {
    std::map<std::string, std::string> attributes;

    const std::string needle = "<" + std::string(element);
    size_t position = 0;
    while (true) {
        const size_t start = text.find(needle, position);
        if (start == std::string_view::npos) {
            return attributes;
        }

        const size_t after_name = start + needle.size();
        const bool boundary = after_name >= text.size() || text[after_name] == ' ' || text[after_name] == '\t' ||
                              text[after_name] == '\r' || text[after_name] == '\n' || text[after_name] == '>' ||
                              text[after_name] == '/';
        if (!boundary) {
            position = after_name;
            continue;
        }

        const size_t tag_end = text.find('>', after_name);
        if (tag_end == std::string_view::npos) {
            return attributes;
        }

        const std::string_view tag = text.substr(after_name, tag_end - after_name);
        size_t cursor = 0;
        while (cursor < tag.size()) {
            while (cursor < tag.size() && std::isspace(static_cast<unsigned char>(tag[cursor])) != 0) {
                ++cursor;
            }

            const size_t name_start = cursor;
            while (cursor < tag.size() && tag[cursor] != '=' && tag[cursor] != '/' &&
                   std::isspace(static_cast<unsigned char>(tag[cursor])) == 0) {
                ++cursor;
            }
            if (cursor >= tag.size() || tag[cursor] != '=') {
                // 跳过这个记号（可能是自闭合的 '/' 或多余空白）。
                if (cursor == name_start) {
                    ++cursor;
                }
                continue;
            }

            const std::string name(tag.substr(name_start, cursor - name_start));
            ++cursor;  // '='
            if (cursor >= tag.size() || (tag[cursor] != '"' && tag[cursor] != '\'')) {
                continue;
            }

            const char quote = tag[cursor++];
            const size_t value_start = cursor;
            const size_t value_end = tag.find(quote, cursor);
            if (value_end == std::string_view::npos) {
                return attributes;
            }

            if (!name.empty()) {
                attributes.emplace(name, std::string(tag.substr(value_start, value_end - value_start)));
            }
            cursor = value_end + 1;
        }

        return attributes;
    }
}

std::string read_text_entry(const zip::Archive& archive, std::string_view name, std::string_view code,
                            std::string_view message, uint64_t cap) {
    const zip::Entry* entry = archive.find(name);
    if (entry == nullptr) {
        fail(std::string(code), std::string(message), std::string(name));
    }

    const Bytes bytes = archive.read_to_memory(*entry, cap);
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

}  // namespace

std::string normalize_appx_version(std::string_view version) {
    std::string result(version);
    size_t dots = static_cast<size_t>(std::count(result.begin(), result.end(), '.'));
    while (dots < 3) {
        result += ".0";
        ++dots;
    }
    return result;
}

std::string normalize_appx_architecture(std::string_view architecture) {
    std::string value = trim_ascii(architecture);
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    if (value == "amd64") {
        return "x64";
    }
    if (value == "x86" || value == "x64" || value == "arm64" || value == "neutral") {
        return value;
    }
    return value;
}

AppxIdentity read_appx_identity(const zip::Archive& inner) {
    const std::string raw = read_text_entry(inner, kManifestEntry, codes::AppxManifestMissing,
                                            "The MSIX package has no AppxManifest.xml.", 16ull * 1024 * 1024);
    const std::string text = strip_comments(raw);

    const std::map<std::string, std::string> identity = scan_element_attributes(text, "Identity");
    if (identity.empty() || identity.find("Name") == identity.end() || identity.find("Publisher") == identity.end() ||
        identity.find("Version") == identity.end() || identity.find("ProcessorArchitecture") == identity.end()) {
        fail(codes::AppxInvalid, "AppxManifest.xml has no complete Identity element.");
    }

    AppxIdentity result;
    result.name = identity.at("Name");
    result.publisher = identity.at("Publisher");
    result.version = identity.at("Version");
    result.architecture = identity.at("ProcessorArchitecture");

    // 依赖列表：<PackageDependency Name="..." MinVersion="..." />
    size_t position = 0;
    while (true) {
        const size_t start = text.find("<PackageDependency", position);
        if (start == std::string::npos) {
            break;
        }
        const size_t tag_end = text.find('>', start);
        if (tag_end == std::string::npos) {
            break;
        }
        const auto dependency = scan_element_attributes(text.substr(start, tag_end - start + 1), "PackageDependency");
        const auto name = dependency.find("Name");
        if (name != dependency.end() && !name->second.empty()) {
            result.dependency_names.push_back(name->second);
        }
        position = tag_end + 1;
    }

    return result;
}

std::vector<Certificate> read_appx_signer_certificates(const zip::Archive& inner) {
    const zip::Entry* entry = inner.find(kSignatureEntry);
    if (entry == nullptr) {
        fail(codes::AppxSignatureMissing, "The MSIX package is not signed (AppxSignature.p7x is missing).");
    }

    const Bytes signature = inner.read_to_memory(*entry, 16ull * 1024 * 1024);
    return extract_certificates_from_appx_signature(signature);
}

}  // namespace msixpkg
