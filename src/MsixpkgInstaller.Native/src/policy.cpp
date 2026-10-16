#include "policy.h"

#include <Windows.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "internal.h"
#include "json.h"
#include "msixpkg/errors.h"

namespace msixpkg {
namespace {

/// 每个源最多接受这么多条：白名单是策展列表，不是数据表。
/// 设上限是为了让「有人往注册表里灌了 10 万条」不至于让我们每次安装都卡住。
constexpr size_t kMaxPinnedPublishers = 4096;

void add_unique(std::vector<std::string>& target, std::string_view raw) {
    const std::string normalized = normalize_thumbprint(raw);
    if (normalized.empty()) {
        return;
    }

    if (target.size() >= kMaxPinnedPublishers) {
        return;
    }

    if (std::find(target.begin(), target.end(), normalized) == target.end()) {
        target.push_back(normalized);
    }
}

/// 把一段文本按 `;` `,` 空白与换行切开，逐段尝试。
void add_from_text(std::vector<std::string>& target, std::string_view text) {
    size_t start = 0;
    while (start <= text.size()) {
        const size_t separator = text.find_first_of(";, \t\r\n", start);
        const size_t end = separator == std::string_view::npos ? text.size() : separator;
        if (end > start) {
            add_unique(target, text.substr(start, end - start));
        }

        if (separator == std::string_view::npos) {
            break;
        }

        start = separator + 1;
    }
}

/// 读一个 REG_SZ / REG_MULTI_SZ / REG_EXPAND_SZ 值。
void read_registry_value(HKEY key, const wchar_t* name, std::vector<std::string>& target, std::vector<std::string>& warnings) {
    DWORD type = 0;
    DWORD size = 0;
    if (::RegQueryValueExW(key, name, nullptr, &type, nullptr, &size) != ERROR_SUCCESS || size == 0) {
        return;
    }

    if (type != REG_SZ && type != REG_MULTI_SZ && type != REG_EXPAND_SZ) {
        warnings.push_back(
            "the AllowedPublishers registry value is not a string (type " + std::to_string(type) + "); ignored");
        return;
    }

    // +2 个字符的余量：值可能在查询与读取之间变长。
    std::wstring buffer(size / sizeof(wchar_t) + 2, L'\0');
    DWORD actual = static_cast<DWORD>(buffer.size() * sizeof(wchar_t));
    if (::RegQueryValueExW(
            key, name, nullptr, &type, reinterpret_cast<BYTE*>(buffer.data()), &actual) != ERROR_SUCCESS) {
        warnings.push_back("the AllowedPublishers registry value could not be read; ignored");
        return;
    }

    buffer.resize(actual / sizeof(wchar_t));
    if (type == REG_MULTI_SZ) {
        // REG_MULTI_SZ 是「以 \0 分隔、以空串结尾」的一串。
        size_t start = 0;
        for (size_t index = 0; index <= buffer.size(); ++index) {
            if (index == buffer.size() || buffer[index] == L'\0') {
                if (index > start) {
                    add_unique(target, narrow_utf8(std::wstring_view(buffer.data() + start, index - start)));
                }

                start = index + 1;
            }
        }

        return;
    }

    add_from_text(target, narrow_utf8(buffer));
}

/// 读同名**子键**下的所有值：值名随意（一般是人类可读的备注），值数据是指纹。
void read_registry_subkey(HKEY root, std::vector<std::string>& target, std::vector<std::string>& warnings) {
    HKEY subkey = nullptr;
    if (::RegOpenKeyExW(root, kPublisherPolicyValue, 0, KEY_READ | KEY_WOW64_64KEY, &subkey) != ERROR_SUCCESS) {
        return;
    }

    for (DWORD index = 0;; ++index) {
        wchar_t name[512] = {};
        DWORD name_length = 512;
        DWORD type = 0;
        BYTE data[1024] = {};
        DWORD data_length = sizeof(data);

        const LSTATUS status = ::RegEnumValueW(
            subkey, index, name, &name_length, nullptr, &type, data, &data_length);
        if (status == ERROR_NO_MORE_ITEMS) {
            break;
        }

        if (status != ERROR_SUCCESS) {
            warnings.push_back(
                "enumerating the AllowedPublishers subkey stopped early (Win32 error " +
                std::to_string(status) + ")");
            break;
        }

        if (type != REG_SZ && type != REG_EXPAND_SZ) {
            continue;
        }

        const size_t characters = data_length / sizeof(wchar_t);
        if (characters == 0) {
            continue;
        }

        const std::wstring value(reinterpret_cast<const wchar_t*>(data), characters);
        add_from_text(target, narrow_utf8(value));
    }

    ::RegCloseKey(subkey);
}

PublisherPolicy read_machine_policy() {
    PublisherPolicy policy;
    HKEY key = nullptr;
    // KEY_WOW64_64KEY：白名单是管理员为「这台机器」部署的，不该被 32 位视图重定向。
    if (::RegOpenKeyExW(
            HKEY_LOCAL_MACHINE, kPublisherPolicyKey, 0, KEY_READ | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS) {
        // 没部署白名单是常态（默认拒绝），不是错误，也不必刷警告。
        return policy;
    }

    read_registry_value(key, kPublisherPolicyValue, policy.machine_pinned, policy.warnings);
    read_registry_subkey(key, policy.machine_pinned, policy.warnings);
    ::RegCloseKey(key);
    return policy;
}

std::string get_string(const json::Value& object, const char* name) {
    const json::Value* value = object.find(name);
    if (value == nullptr || value->kind != json::Value::Kind::String) {
        return {};
    }

    return value->text;
}

}  // namespace

std::string normalize_thumbprint(std::string_view thumbprint) {
    std::string normalized;
    normalized.reserve(thumbprint.size());
    for (const char raw : thumbprint) {
        const auto value = static_cast<unsigned char>(raw);
        if (value == ' ' || value == '\t' || value == '\r' || value == '\n') {
            continue;
        }

        normalized.push_back(static_cast<char>(std::tolower(value)));
    }

    if (!is_lower_hex(normalized, 64)) {
        return {};
    }

    return normalized;
}

std::vector<std::string> PublisherPolicy::allowed() const {
    std::vector<std::string> merged = machine_pinned;
    for (const std::string& thumbprint : remembered) {
        if (std::find(merged.begin(), merged.end(), thumbprint) == merged.end()) {
            merged.push_back(thumbprint);
        }
    }

    return merged;
}

bool PublisherPolicy::is_pinned(std::string_view sha256) const {
    const std::string normalized = normalize_thumbprint(sha256);
    if (normalized.empty()) {
        return false;
    }

    const std::vector<std::string> merged = allowed();
    return std::find(merged.begin(), merged.end(), normalized) != merged.end();
}

std::filesystem::path remembered_publishers_path(const DataRoots& roots) {
    return roots.root / "publishers.json";
}

PublisherPolicy load_publisher_policy(const DataRoots& roots) {
    PublisherPolicy policy = read_machine_policy();

    const std::optional<std::string> text = read_text_file(remembered_publishers_path(roots));
    if (!text.has_value()) {
        return policy;
    }

    try {
        const json::Value root = json::parse(*text);
        const json::Value* publishers = root.find("publishers");
        if (publishers == nullptr || publishers->kind != json::Value::Kind::Array) {
            policy.warnings.push_back("the remembered-publisher file has no \"publishers\" array; ignored");
            return policy;
        }

        for (const json::Value& item : publishers->items) {
            if (item.kind != json::Value::Kind::Object) {
                continue;
            }

            add_unique(policy.remembered, get_string(item, "sha256"));
        }
    } catch (const std::exception&) {
        // 记录文件被人手改坏时，最安全的做法是当成「没有记住任何东西」，
        // 也就是回到「未命中钉扎 → 需要明确同意」这条路径。
        policy.warnings.push_back("the remembered-publisher file is not valid JSON; ignored");
    }

    return policy;
}

bool remember_publisher(
    const DataRoots& roots,
    std::string_view sha256,
    std::string_view subject) {
    const std::string normalized = normalize_thumbprint(sha256);
    if (normalized.empty()) {
        return false;
    }

    std::vector<std::pair<std::string, std::string>> entries;
    const std::filesystem::path path = remembered_publishers_path(roots);

    // 先读回已有的：这个文件同时是「审计记录」，不该在追加时丢掉历史。
    if (const std::optional<std::string> text = read_text_file(path); text.has_value()) {
        try {
            const json::Value root = json::parse(*text);
            if (const json::Value* publishers = root.find("publishers");
                publishers != nullptr && publishers->kind == json::Value::Kind::Array) {
                for (const json::Value& item : publishers->items) {
                    if (item.kind != json::Value::Kind::Object) {
                        continue;
                    }

                    const std::string existing = normalize_thumbprint(get_string(item, "sha256"));
                    if (!existing.empty()) {
                        entries.emplace_back(existing, get_string(item, "subject"));
                    }
                }
            }
        } catch (const std::exception&) {
            // 坏文件：从空开始重建，至少让本次的信任决定落盘。
        }
    }

    const std::string subject_text(subject);
    bool replaced = false;
    for (auto& entry : entries) {
        if (entry.first == normalized) {
            entry.second = subject_text;
            replaced = true;
        }
    }

    if (!replaced) {
        entries.emplace_back(normalized, subject_text);
    }

    json::Value root = json::Value::make_object();
    json::Value publishers = json::Value::make_array();
    for (const auto& entry : entries) {
        json::Value item = json::Value::make_object();
        item.set("sha256", json::Value::make_string(entry.first));
        item.set("subject", json::Value::make_string(entry.second));
        publishers.push(std::move(item));
    }

    root.set("publishers", std::move(publishers));
    root.set("note", json::Value::make_string(
        "Publishers the user explicitly trusted on this machine (TOFU). "
        "Deleting an entry makes the installer ask again."));
    root.set("updatedUtc", json::Value::make_string(utc_now()));

    try {
        write_text_atomic(path, root.to_json());
        return true;
    } catch (const Error&) {
        return false;
    }
}

}  // namespace msixpkg
