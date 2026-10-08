#include "arp.h"

#include <Windows.h>

#include <string>
#include <vector>

#include "internal.h"
#include "msixpkg/errors.h"

namespace msixpkg {
namespace {

constexpr wchar_t kUninstallKey[] = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall";

std::wstring quote_widen(std::string_view value) {
    return quote_argument(widen_utf8(value));
}

void set_string_value(HKEY key, const wchar_t* name, const std::string& value) {
    const std::wstring wide = widen_utf8(value);
    const DWORD size = static_cast<DWORD>((wide.size() + 1) * sizeof(wchar_t));
    const LSTATUS status = ::RegSetValueExW(
        key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(wide.c_str()), size);
    if (status != ERROR_SUCCESS) {
        throw Error(
            codes::IoError,
            "Unable to write the uninstall entry.",
            narrow_utf8(name) + ": Win32 error " + std::to_string(status));
    }
}

/// 尝试在一个配置单元下写入条目；失败返回 Win32 错误码。
LSTATUS write_under(HKEY root, const ArpEntry& entry) {
    const std::wstring subkey = std::wstring(kUninstallKey) + L"\\" + widen_utf8(entry.key_name);
    HKEY key = nullptr;
    const LSTATUS created = ::RegCreateKeyExW(
        root, subkey.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr);
    if (created != ERROR_SUCCESS) {
        return created;
    }

    LSTATUS status = ERROR_SUCCESS;
    try {
        set_string_value(key, L"DisplayName", entry.display_name);
        set_string_value(key, L"DisplayVersion", entry.display_version);
        set_string_value(key, L"Publisher", entry.publisher);
        set_string_value(key, L"UninstallString", entry.uninstall_string);

        if (!entry.display_icon.empty()) {
            set_string_value(key, L"DisplayIcon", entry.display_icon);
        }

        if (!entry.install_location.empty()) {
            set_string_value(key, L"InstallLocation", entry.install_location);
        }

        // 明确告诉「设置」这不是 MSIX 自带条目，避免和系统的 MSIX 条目混淆。
        set_string_value(key, L"Comments", "Managed by the msixpkg installer (reclaims the publisher certificate on uninstall).");
        const DWORD zero = 0;
        ::RegSetValueExW(key, L"NoModify", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&zero), sizeof(zero));
    } catch (const Error& error) {
        // 把异常转成状态码，交给调用方决定要不要换一个配置单元。
        status = ERROR_WRITE_FAULT;
        (void)error;
    }

    ::RegCloseKey(key);
    return status;
}

bool key_exists(HKEY root, const std::wstring& subkey) {
    HKEY key = nullptr;
    const LSTATUS status = ::RegOpenKeyExW(root, subkey.c_str(), 0, KEY_READ, &key);
    if (status != ERROR_SUCCESS) {
        return false;
    }

    ::RegCloseKey(key);
    return true;
}

}  // namespace

std::string arp_key_name(std::string_view identity_name) {
    return "Msixpkg." + std::string(identity_name);
}

ArpEntry make_arp_entry(
    std::string_view identity_name,
    std::string_view display_name,
    std::string_view version,
    std::string_view publisher,
    std::string_view executable_path,
    std::string_view family_name,
    std::string_view install_location) {
    ArpEntry entry;
    entry.key_name = arp_key_name(identity_name);
    // 后缀是必需的：设置里 MSIX 自己那条也在，两条同名会让用户不知道该点哪个。
    entry.display_name = std::string(display_name) + " (msixpkg: also removes the publisher certificate)";
    entry.display_icon = std::string(executable_path);
    entry.display_version = std::string(version);
    entry.publisher = std::string(publisher);
    // 路径可能带空格：必须引用，否则 ShellExecute 会把后半截当成参数。
    entry.uninstall_string = std::string(quote_argument(executable_path)) + " uninstall --family-name " +
                             quote_argument(family_name);
    entry.install_location = std::string(install_location);
    return entry;
}

std::string write_arp_entry(const ArpEntry& entry) {
    if (entry.key_name.empty() || entry.uninstall_string.empty()) {
        throw Error(codes::IoError, "The uninstall entry requires a key name and an uninstall command.");
    }

    const LSTATUS machine = write_under(HKEY_LOCAL_MACHINE, entry);
    if (machine == ERROR_SUCCESS) {
        return "HKLM";
    }

    const LSTATUS user = write_under(HKEY_CURRENT_USER, entry);
    if (user == ERROR_SUCCESS) {
        return "HKCU";
    }

    throw Error(
        codes::IoError,
        "Unable to create the \"Apps & features\" uninstall entry.",
        "HKLM: " + std::to_string(machine) + ", HKCU: " + std::to_string(user));
}

bool remove_arp_entry(std::string_view key_name) {
    if (key_name.empty()) {
        return false;
    }

    const std::wstring subkey = std::wstring(kUninstallKey) + L"\\" + widen_utf8(key_name);
    bool removed = false;
    for (HKEY root : {HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER}) {
        if (::RegDeleteTreeW(root, subkey.c_str()) == ERROR_SUCCESS) {
            removed = true;
        }
    }

    return removed;
}

std::string find_arp_entry_hive(std::string_view key_name) {
    if (key_name.empty()) {
        return {};
    }

    const std::wstring subkey = std::wstring(kUninstallKey) + L"\\" + widen_utf8(key_name);
    if (key_exists(HKEY_LOCAL_MACHINE, subkey)) {
        return "HKLM";
    }

    if (key_exists(HKEY_CURRENT_USER, subkey)) {
        return "HKCU";
    }

    return {};
}

}  // namespace msixpkg
