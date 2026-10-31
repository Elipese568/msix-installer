#include "setup_path.h"

#include "setup_registry.h"
#include "setup_util.h"

#include <algorithm>
#include <cwctype>

namespace msixpkg::setup {
namespace {

constexpr size_t kMaxPathCharacters = 1u << 20;

std::wstring_view strip_compare_noise(std::wstring_view text) {
    while (!text.empty() && (text.front() == L' ' || text.front() == L'\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == L' ' || text.back() == L'\t')) {
        text.remove_suffix(1);
    }
    if (text.size() >= 2 && text.front() == L'"' && text.back() == L'"') {
        text.remove_prefix(1);
        text.remove_suffix(1);
    }
    while (!text.empty() && (text.back() == L'\\' || text.back() == L'/')) {
        text.remove_suffix(1);
    }
    return text;
}

const wchar_t* path_key_for(HKEY root) {
    return root == HKEY_LOCAL_MACHINE ? kMachinePathKey : kUserPathKey;
}

}  // namespace

std::vector<std::wstring> split_path(std::wstring_view value) {
    std::vector<std::wstring> segments;
    size_t start = 0;
    for (;;) {
        const size_t separator = value.find(L';', start);
        if (separator == std::wstring_view::npos) {
            segments.emplace_back(value.substr(start));
            break;
        }
        segments.emplace_back(value.substr(start, separator - start));
        start = separator + 1;
    }
    return segments;
}

std::wstring join_path(const std::vector<std::wstring>& segments) {
    std::wstring value;
    for (size_t index = 0; index < segments.size(); ++index) {
        if (index != 0) {
            value.push_back(L';');
        }
        value += segments[index];
    }
    return value;
}

bool path_entry_equal(std::wstring_view left, std::wstring_view right) {
    const std::wstring_view a = strip_compare_noise(left);
    const std::wstring_view b = strip_compare_noise(right);
    if (a.size() != b.size()) {
        return false;
    }
    return ::_wcsnicmp(a.data(), b.data(), a.size()) == 0;
}

bool path_contains(std::wstring_view value, std::wstring_view entry) {
    for (const std::wstring& segment : split_path(value)) {
        if (path_entry_equal(segment, entry)) {
            return true;
        }
    }
    return false;
}

std::wstring path_add(std::wstring_view value, std::wstring_view entry, bool& changed) {
    changed = false;
    if (path_contains(value, entry)) {
        return std::wstring(value);
    }
    changed = true;
    if (value.empty()) {
        return std::wstring(entry);
    }
    std::wstring result(value);
    // 末尾的 ';' 表示「最后还有一个空段」。补一个分隔符再拼条目，空段才会原样保留。
    result.push_back(L';');
    result += entry;
    return result;
}

std::wstring path_remove(std::wstring_view value, std::wstring_view entry, bool& changed) {
    changed = false;
    const std::vector<std::wstring> segments = split_path(value);
    std::vector<std::wstring> kept;
    kept.reserve(segments.size());
    for (const std::wstring& segment : segments) {
        if (path_entry_equal(segment, entry)) {
            changed = true;
            continue;
        }
        kept.push_back(segment);
    }
    if (!changed) {
        return std::wstring(value);
    }
    return join_path(kept);
}

bool read_environment_path(HKEY root, PathValue& value, std::wstring& error) {
    value = PathValue{};
    RegValue raw;
    if (!reg_read(root, path_key_for(root), L"Path", raw, error)) {
        return false;
    }
    if (!raw.exists) {
        return true;
    }
    if (raw.type != REG_SZ && raw.type != REG_EXPAND_SZ) {
        error = L"PATH value has an unexpected registry type: " + std::to_wstring(raw.type);
        return false;
    }

    // RegQueryValueExW 报告的字节数就是真实长度；先挡一次异常值，避免分配失控。
    if (raw.text.size() > kMaxPathCharacters) {
        error = L"PATH is longer than " + std::to_wstring(kMaxPathCharacters) + L" characters; refusing to touch it";
        return false;
    }

    value.exists = true;
    value.type = raw.type;
    value.value = raw.text;
    return true;
}

bool write_environment_path(HKEY root, const PathValue& value, std::wstring& error) {
    if (value.value.size() > kMaxPathCharacters) {
        error = L"PATH is too long to write safely";
        return false;
    }
    if (value.type == REG_EXPAND_SZ) {
        return reg_write_expand_string(root, path_key_for(root), L"Path", value.value, error);
    }
    return reg_write_string(root, path_key_for(root), L"Path", value.value, error);
}

bool delete_environment_path(HKEY root, std::wstring& error) {
    error.clear();
    if (!reg_delete_value(root, path_key_for(root), L"Path")) {
        error = L"could not delete the PATH value";
        return false;
    }
    return true;
}

void broadcast_environment_change() {
    DWORD_PTR result = 0;
    ::SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0,
                          reinterpret_cast<LPARAM>(L"Environment"), SMTO_ABORTIFHUNG, 5000, &result);
}

}  // namespace msixpkg::setup
