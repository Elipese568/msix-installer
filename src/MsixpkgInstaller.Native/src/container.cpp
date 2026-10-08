#include "container.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <set>

#include "json.h"

namespace msixpkg {
namespace {

[[noreturn]] void schema_error(const std::string& detail) {
    fail(codes::ManifestSchema, detail);
}

// ---------- JSON 取值助手（严格：缺成员/类型不符一律 E_MANIFEST_SCHEMA） ----------

const json::Value& require_member(const json::Value& parent, const char* name) {
    const json::Value* value = parent.find(name);
    if (value == nullptr) {
        schema_error(std::string("Missing required member '") + name + "'.");
    }
    return *value;
}

const json::Value& require_object(const json::Value& parent, const char* name) {
    const json::Value& value = require_member(parent, name);
    if (value.kind != json::Value::Kind::Object) {
        schema_error(std::string("'") + name + "' must be an object.");
    }
    return value;
}

const std::string& require_string(const json::Value& parent, const char* name) {
    const json::Value& value = require_member(parent, name);
    if (value.kind != json::Value::Kind::String || value.text.empty()) {
        schema_error(std::string("'") + name + "' must be a non-empty string.");
    }
    return value.text;
}

std::string optional_string(const json::Value& parent, const char* name) {
    const json::Value* value = parent.find(name);
    if (value == nullptr || value->kind == json::Value::Kind::Null) {
        return {};
    }
    if (value->kind != json::Value::Kind::String) {
        schema_error(std::string("'") + name + "' must be a string.");
    }
    return value->text;
}

bool require_bool(const json::Value& parent, const char* name) {
    const json::Value& value = require_member(parent, name);
    if (value.kind != json::Value::Kind::Boolean) {
        schema_error(std::string("'") + name + "' must be a boolean.");
    }
    return value.boolean;
}

int64_t require_integer(const json::Value& parent, const char* name, int64_t min, int64_t max) {
    const json::Value& value = require_member(parent, name);
    if (value.kind != json::Value::Kind::Integer || value.integer < min || value.integer > max) {
        schema_error(std::string("'") + name + "' must be an integer in range.");
    }
    return value.integer;
}

std::string require_hex(const json::Value& parent, const char* name, size_t length) {
    const std::string& value = require_string(parent, name);
    if (!is_lower_hex(value, length)) {
        schema_error(std::string("'") + name + "' must be " + std::to_string(length) + " lowercase hex characters.");
    }
    return value;
}

bool is_iso_utc(std::string_view text) {
    // yyyy-MM-ddTHH:mm:ssZ
    if (text.size() != 20 || text[4] != '-' || text[7] != '-' || text[10] != 'T' || text[13] != ':' ||
        text[16] != ':' || text[19] != 'Z') {
        return false;
    }

    const auto digit_at = [&](size_t index) { return text[index] >= '0' && text[index] <= '9'; };
    for (size_t i : {0u, 1u, 2u, 3u, 5u, 6u, 8u, 9u, 11u, 12u, 14u, 15u, 17u, 18u}) {
        if (!digit_at(i)) {
            return false;
        }
    }

    const auto number_at = [&](size_t index) { return (text[index] - '0') * 10 + (text[index + 1] - '0'); };
    const int month = number_at(5);
    const int day = number_at(8);
    const int hour = number_at(11);
    const int minute = number_at(14);
    const int second = number_at(17);

    return month >= 1 && month <= 12 && day >= 1 && day <= 31 && hour <= 23 && minute <= 59 && second <= 59;
}

std::string require_iso_utc(const json::Value& parent, const char* name) {
    const std::string& value = require_string(parent, name);
    if (!is_iso_utc(value)) {
        schema_error(std::string("'") + name + "' must be an ISO-8601 UTC timestamp like 2026-10-08T12:00:00Z.");
    }
    return value;
}

bool is_four_part_version(std::string_view version) {
    size_t start = 0;
    int parts = 0;
    while (true) {
        const size_t dot = version.find('.', start);
        const std::string_view part = (dot == std::string_view::npos) ? version.substr(start) : version.substr(start, dot - start);
        if (part.empty() || part.size() > 5) {
            return false;
        }

        int value = 0;
        for (const char c : part) {
            if (c < '0' || c > '9') {
                return false;
            }
            value = value * 10 + (c - '0');
        }

        if (value > 65535) {
            return false;
        }

        ++parts;
        if (dot == std::string_view::npos) {
            break;
        }
        start = dot + 1;
    }

    return parts == 4;
}

FileRef parse_file_ref(const json::Value& value, bool is_dependency) {
    FileRef ref;
    ref.path = require_string(value, "path");
    ref.sha256 = require_hex(value, "sha256", 64);
    ref.size = static_cast<uint64_t>(require_integer(value, "size", 1, INT64_MAX));
    if (is_dependency) {
        ref.name = require_string(value, "name");
        ref.version = require_string(value, "version");
    }
    return ref;
}

void validate_file_ref(const FileRef& ref, const char* label, bool expect_dependency) {
    if (ref.path.empty()) {
        schema_error(std::string("'") + label + ".path' is empty.");
    }

    EntryKind kind = EntryKind::Manifest;
    try {
        kind = classify_entry(ref.path);
    } catch (const Error&) {
        schema_error(std::string("'") + label + ".path' is not a valid entry name.");
    }

    const EntryKind expected = expect_dependency ? EntryKind::Dependency : EntryKind::MainPackage;
    if (kind != expected) {
        schema_error(std::string("'") + label + ".path' is not a " +
                     (expect_dependency ? "dependency" : "main package") + " entry.");
    }

    if (!is_lower_hex(ref.sha256, 64)) {
        schema_error(std::string("'") + label + ".sha256' must be 64 lowercase hex characters.");
    }

    if (ref.size == 0) {
        schema_error(std::string("'") + label + ".size' must be positive.");
    }

    if (expect_dependency && (ref.name.empty() || ref.version.empty())) {
        schema_error("Dependency entries require 'name' and 'version'.");
    }
}

}  // namespace

// ---------- 条目名与白名单 ----------

void validate_entry_name(std::string_view name) {
    if (name.empty()) {
        fail(codes::EntryNameInvalid, "An entry name is empty.");
    }

    if (name.size() > 512) {
        fail(codes::EntryNameInvalid, "Entry name exceeds 512 characters.");
    }

    if (name.front() == '/' || name.back() == '/') {
        fail(codes::EntryNameInvalid, "Entry name must not start or end with '/'.", std::string(name));
    }

    for (const char c : name) {
        const auto byte = static_cast<unsigned char>(c);
        if (c == '\\') {
            fail(codes::EntryNameInvalid, "Entry name must not contain a backslash.", std::string(name));
        }
        if (byte < 0x20 || byte == 0x7F) {
            fail(codes::EntryNameInvalid, "Entry name contains a control character.", std::string(name));
        }
        switch (c) {
            case ':':
            case '*':
            case '?':
            case '"':
            case '<':
            case '>':
            case '|':
                fail(codes::EntryNameInvalid,
                     std::string("Entry name contains a reserved character '") + c + "'.", std::string(name));
            default:
                break;
        }
    }

    for (const std::string& segment : split(name, '/')) {
        if (segment.empty()) {
            fail(codes::EntryNameInvalid, "Entry name contains an empty path segment.", std::string(name));
        }
        if (segment == "." || segment == "..") {
            fail(codes::EntryNameInvalid, "Entry name contains a relative path segment.", std::string(name));
        }
        if (segment.size() > 1 && segment.back() == '.') {
            fail(codes::EntryNameInvalid, "Entry name segment ends with '.'.", std::string(name));
        }
    }
}

EntryKind classify_entry(std::string_view name) {
    validate_entry_name(name);

    if (name == "msixpkg.json") {
        return EntryKind::Manifest;
    }
    if (name == "msixpkg.sig") {
        return EntryKind::Signature;
    }
    if (equals_ignore_case_ascii(name, "publisher/publisher.cer")) {
        if (name != "publisher/publisher.cer") {
            fail(codes::EntryUnknown, "Entry name differs only by case from a reserved name.", std::string(name));
        }
        return EntryKind::PublisherCertificate;
    }
    if (name == "publisher/chain.p7b") {
        return EntryKind::PublisherChain;
    }

    if (starts_with(name, "payload/") && ends_with(name, ".msix")) {
        const std::vector<std::string> segments = split(name, '/');
        if (segments.size() == 4 && !segments[1].empty() && segments[3].size() > 6) {
            if (segments[2] == "main") {
                return EntryKind::MainPackage;
            }
            if (segments[2] == "deps") {
                return EntryKind::Dependency;
            }
        }
    }

    fail(codes::EntryUnknown, "Entry is not part of the msixpkg v1 whitelist.", std::string(name));
}

bool is_known_architecture(std::string_view architecture) {
    return architecture == "x64" || architecture == "x86" || architecture == "arm64" || architecture == "neutral";
}

// ---------- 清单 ----------

Manifest Manifest::parse(std::string_view text) {
    json::Value root;
    try {
        root = json::parse(text);
    } catch (const json::ParseError& error) {
        fail(codes::ManifestSchema, "msixpkg.json is not valid JSON.", error.what());
    }

    if (root.kind != json::Value::Kind::Object) {
        schema_error("msixpkg.json must be a JSON object.");
    }

    static const std::set<std::string> known_members = {
        "formatVersion", "createdUtc", "identity", "mainPackage", "dependencies", "publisher",
        "minOsBuild", "installerMinVersion", "requiresAdmin", "allowAllUsers",
        "uninstallRemoveCertificate", "display"};

    for (const auto& member : root.members) {
        if (known_members.find(member.first) == known_members.end()) {
            fail(codes::ManifestSchema, "msixpkg.json contains an unknown member.", member.first);
        }
    }

    Manifest manifest;
    manifest.format_version = require_integer(root, "formatVersion", 0, INT64_MAX);
    manifest.created_utc = require_iso_utc(root, "createdUtc");

    const json::Value& identity = require_object(root, "identity");
    manifest.identity.name = require_string(identity, "name");
    manifest.identity.publisher = require_string(identity, "publisher");
    manifest.identity.version = require_string(identity, "version");
    manifest.identity.architecture = require_string(identity, "architecture");

    manifest.main_package = parse_file_ref(require_object(root, "mainPackage"), false);

    const json::Value& dependencies = require_member(root, "dependencies");
    if (dependencies.kind != json::Value::Kind::Array) {
        schema_error("'dependencies' must be an array.");
    }
    for (const json::Value& item : dependencies.items) {
        if (item.kind != json::Value::Kind::Object) {
            schema_error("Each 'dependencies' item must be an object.");
        }
        manifest.dependencies.push_back(parse_file_ref(item, true));
    }

    const json::Value* publisher = root.find("publisher");
    if (publisher != nullptr && publisher->kind != json::Value::Kind::Null) {
        if (publisher->kind != json::Value::Kind::Object) {
            schema_error("'publisher' must be an object.");
        }
        manifest.has_publisher = true;
        manifest.publisher.cert_path = require_string(*publisher, "certPath");
        manifest.publisher.subject = require_string(*publisher, "subject");
        manifest.publisher.thumbprint_sha1 = require_hex(*publisher, "thumbprintSha1", 40);
        manifest.publisher.thumbprint_sha256 = require_hex(*publisher, "thumbprintSha256", 64);
        manifest.publisher.not_before_utc = require_iso_utc(*publisher, "notBeforeUtc");
        manifest.publisher.not_after_utc = require_iso_utc(*publisher, "notAfterUtc");
        manifest.publisher.self_signed = require_bool(*publisher, "selfSigned");
    }

    manifest.min_os_build = require_integer(root, "minOsBuild", 1, INT64_MAX);
    manifest.installer_min_version = require_string(root, "installerMinVersion");
    manifest.requires_admin = require_bool(root, "requiresAdmin");
    manifest.allow_all_users = require_bool(root, "allowAllUsers");
    manifest.uninstall_remove_certificate = require_bool(root, "uninstallRemoveCertificate");

    const json::Value* display = root.find("display");
    if (display != nullptr && display->kind != json::Value::Kind::Null) {
        if (display->kind != json::Value::Kind::Object) {
            schema_error("'display' must be an object.");
        }
        manifest.has_display = true;
        manifest.display.name = optional_string(*display, "name");
        manifest.display.description = optional_string(*display, "description");
        manifest.display.homepage = optional_string(*display, "homepage");
    }

    manifest.validate();
    return manifest;
}

void Manifest::validate() const {
    if (format_version != 1) {
        fail(codes::FormatVersionUnsupported, "Unsupported msixpkg format version.",
             std::to_string(format_version));
    }

    if (!is_iso_utc(created_utc)) {
        schema_error("'createdUtc' must be an ISO-8601 UTC timestamp.");
    }

    if (identity.name.empty()) {
        schema_error("'identity.name' is empty.");
    }
    if (identity.publisher.empty()) {
        schema_error("'identity.publisher' is empty.");
    }
    if (!is_four_part_version(identity.version)) {
        schema_error("'identity.version' must be four dot-separated numbers, each 0-65535.");
    }
    if (!is_known_architecture(identity.architecture)) {
        schema_error("'identity.architecture' is not a known architecture.");
    }

    validate_file_ref(main_package, "mainPackage", false);
    for (const FileRef& dependency : dependencies) {
        validate_file_ref(dependency, "dependencies[]", true);
    }

    if (has_publisher) {
        if (publisher.cert_path != "publisher/publisher.cer") {
            schema_error("'publisher.certPath' must be 'publisher/publisher.cer'.");
        }
        if (publisher.subject.empty()) {
            schema_error("'publisher.subject' is empty.");
        }
        if (!is_iso_utc(publisher.not_before_utc) || !is_iso_utc(publisher.not_after_utc)) {
            schema_error("Publisher timestamps must be ISO-8601 UTC.");
        }
    }

    if (installer_min_version.empty()) {
        schema_error("'installerMinVersion' is empty.");
    }
}

Signature Signature::parse(std::string_view text) {
    json::Value root;
    try {
        root = json::parse(text);
    } catch (const json::ParseError& error) {
        fail(codes::SignatureMalformed, "msixpkg.sig is not valid JSON.", error.what());
    }

    if (root.kind != json::Value::Kind::Object) {
        fail(codes::SignatureMalformed, "msixpkg.sig must be a JSON object.");
    }

    static const std::set<std::string> known_members = {"alg", "keyThumbprintSha256", "contentDigest", "signature"};
    for (const auto& member : root.members) {
        if (known_members.find(member.first) == known_members.end()) {
            fail(codes::SignatureMalformed, "msixpkg.sig contains an unknown member.", member.first);
        }
    }

    Signature signature;
    signature.algorithm = require_string(root, "alg");
    if (!signature_algorithm::is_supported(signature.algorithm)) {
        fail(codes::SignatureAlgorithmUnsupported, "Unsupported container signature algorithm.", signature.algorithm);
    }

    const std::string& thumbprint = require_string(root, "keyThumbprintSha256");
    if (!is_lower_hex(thumbprint, 64)) {
        fail(codes::SignatureMalformed, "'keyThumbprintSha256' must be 64 lowercase hex characters.");
    }
    signature.key_thumbprint_sha256 = thumbprint;

    const std::string& digest = require_string(root, "contentDigest");
    if (!is_lower_hex(digest, 64)) {
        fail(codes::SignatureMalformed, "'contentDigest' must be 64 lowercase hex characters.");
    }
    signature.content_digest = digest;

    signature.signature_base64 = require_string(root, "signature");
    return signature;
}

// ---------- 内容摘要 ----------

std::string content_digest_hex(const std::vector<std::pair<std::string, std::string>>& names_and_hashes) {
    std::vector<std::pair<std::string, std::string>> sorted = names_and_hashes;
    std::sort(sorted.begin(), sorted.end(), [](const auto& left, const auto& right) { return left.first < right.first; });

    for (size_t i = 1; i < sorted.size(); ++i) {
        if (sorted[i - 1].first == sorted[i].first) {
            fail(codes::EntryDuplicate, "Duplicate entry name while computing the content digest.", sorted[i].first);
        }
    }

    Sha256 hash;
    for (const auto& entry : sorted) {
        hash.update(reinterpret_cast<const uint8_t*>(entry.first.data()), entry.first.size());
        const uint8_t separator = 0x00;
        hash.update(&separator, 1);
        hash.update(reinterpret_cast<const uint8_t*>(entry.second.data()), entry.second.size());
        const uint8_t newline = 0x0A;
        hash.update(&newline, 1);
    }

    return to_hex_lower(hash.finish());
}

// ---------- 容器 ----------

Container Container::open(const std::filesystem::path& path) {
    zip::Archive archive = zip::Archive::open(path);

    if (archive.entries().empty() || archive.entries().front().name != "msixpkg.json") {
        fail(codes::ManifestNotFirst, "msixpkg.json must be present and be the first entry in the container.",
             archive.entries().empty() ? std::string() : archive.entries().front().name);
    }

    const zip::Entry* manifest_entry = archive.find("msixpkg.json");
    if (manifest_entry == nullptr) {
        fail(codes::ManifestMissing, "The container has no msixpkg.json.");
    }

    const Bytes manifest_bytes = archive.read_to_memory(*manifest_entry, 16ull * 1024 * 1024);
    const Manifest manifest = Manifest::parse(
        std::string_view(reinterpret_cast<const char*>(manifest_bytes.data()), manifest_bytes.size()));

    check_container_structure(archive, manifest);
    return Container(std::move(archive), manifest);
}

void check_container_structure(const zip::Archive& archive, const Manifest& manifest) {
    std::set<std::string, std::less<>> declared{"msixpkg.json"};
    declared.insert(manifest.main_package.path);
    for (const FileRef& dependency : manifest.dependencies) {
        declared.insert(dependency.path);
    }
    if (manifest.has_publisher) {
        declared.insert(manifest.publisher.cert_path);
    }

    // 条目集合必须与清单声明完全一致（不多不少）；签名条目豁免。
    for (const zip::Entry& entry : archive.entries()) {
        if (entry.name == "msixpkg.sig") {
            continue;
        }
        if (declared.find(entry.name) == declared.end()) {
            fail(codes::EntryUnknown, "The container carries an entry that the manifest does not declare.", entry.name);
        }
    }

    // 反向：清单声明的每个载荷与证书都必须真的存在。
    const auto require_present = [&archive](const std::string& name) {
        if (archive.find(name) == nullptr) {
            fail(codes::FileMissing, "A declared payload entry is missing.", name);
        }
    };

    require_present(manifest.main_package.path);
    for (const FileRef& dependency : manifest.dependencies) {
        require_present(dependency.path);
    }
    if (manifest.has_publisher) {
        require_present(manifest.publisher.cert_path);
    }
}

const zip::Entry& Container::require_entry(std::string_view name, const char* code, const char* message) const {
    const zip::Entry* entry = archive_.find(name);
    if (entry == nullptr) {
        fail(code, message, std::string(name));
    }
    return *entry;
}

Bytes Container::read_entry(std::string_view name, uint64_t cap) const {
    const zip::Entry& entry = require_entry(name, codes::FileMissing, "The container entry is missing.");
    return archive_.read_to_memory(entry, cap);
}

std::string Container::entry_sha256(const zip::Entry& entry) const {
    Sha256 hash;
    archive_.read_stream(entry, [&hash](const uint8_t* data, size_t size) { hash.update(data, size); });
    return to_hex_lower(hash.finish());
}

std::string Container::compute_content_digest() const {
    std::vector<std::pair<std::string, std::string>> entries;
    for (const zip::Entry& entry : archive_.entries()) {
        if (entry.name == "msixpkg.sig") {
            continue;
        }
        entries.emplace_back(entry.name, entry_sha256(entry));
    }
    return content_digest_hex(entries);
}

zip::Archive Container::open_nested(const zip::Entry& entry) const {
    if (entry.method == 0) {
        // store：直接借用映射区间，零拷贝。
        const uint8_t* data = archive_.compressed_data(entry);
        return zip::Archive::open_memory(data, static_cast<size_t>(entry.uncompressed_size));
    }

    // deflate：必须把解压结果保存在成员里，保证返回的 Archive 借用期间不悬垂。
    nested_scratch_ = archive_.read_to_memory(entry, 512ull * 1024 * 1024);
    return zip::Archive::open_memory(nested_scratch_.data(), nested_scratch_.size());
}

}  // namespace msixpkg
