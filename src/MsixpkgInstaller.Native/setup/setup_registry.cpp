#include "setup_registry.h"

#include "setup_util.h"

#include <cstring>
#include <vector>

namespace msixpkg::setup {
namespace {

std::wstring describe_status(LSTATUS status) {
    return format_win32_error(static_cast<DWORD>(status));
}

}  // namespace

RegKey::~RegKey() {
    close();
}

void RegKey::close() {
    if (key_ != nullptr) {
        ::RegCloseKey(key_);
        key_ = nullptr;
    }
}

LSTATUS RegKey::open(HKEY root, const wchar_t* subkey, REGSAM access) {
    close();
    const LSTATUS status = ::RegOpenKeyExW(root, subkey, 0, access | KEY_WOW64_64KEY, &key_);
    if (status != ERROR_SUCCESS) {
        key_ = nullptr;
    }
    return status;
}

LSTATUS RegKey::create(HKEY root, const wchar_t* subkey, REGSAM access) {
    close();
    DWORD disposition = 0;
    const LSTATUS status = ::RegCreateKeyExW(root, subkey, 0, nullptr, REG_OPTION_NON_VOLATILE,
                                             access | KEY_WOW64_64KEY, nullptr, &key_, &disposition);
    if (status != ERROR_SUCCESS) {
        if (key_ != nullptr) {
            ::RegCloseKey(key_);
            key_ = nullptr;
        }
    }
    return status;
}

std::wstring hkey_text(HKEY root) {
    if (root == HKEY_LOCAL_MACHINE) {
        return L"HKLM";
    }
    if (root == HKEY_CURRENT_USER) {
        return L"HKCU";
    }
    return L"HKEY?";
}

bool reg_read(HKEY root, const wchar_t* subkey, const wchar_t* name, RegValue& value,
              std::wstring& error) {
    value = RegValue{};
    RegKey key;
    const LSTATUS open_status = key.open(root, subkey, KEY_QUERY_VALUE);
    if (open_status != ERROR_SUCCESS) {
        if (open_status == ERROR_FILE_NOT_FOUND || open_status == ERROR_PATH_NOT_FOUND) {
            return true;
        }
        error = L"RegOpenKeyExW(" + hkey_text(root) + L"\\" + subkey + L"): " + describe_status(open_status);
        return false;
    }

    DWORD type = REG_NONE;
    DWORD size = 0;
    LSTATUS status = ::RegQueryValueExW(key.handle(), name, nullptr, &type, nullptr, &size);
    if (status == ERROR_FILE_NOT_FOUND) {
        return true;
    }
    if (status != ERROR_SUCCESS) {
        error = L"RegQueryValueExW(" + std::wstring(name) + L"): " + describe_status(status);
        return false;
    }

    std::vector<uint8_t> buffer(size + sizeof(wchar_t) * 2);
    DWORD buffer_size = static_cast<DWORD>(buffer.size());
    status = ::RegQueryValueExW(key.handle(), name, nullptr, &type, buffer.data(), &buffer_size);
    if (status != ERROR_SUCCESS) {
        error = L"RegQueryValueExW(" + std::wstring(name) + L"): " + describe_status(status);
        return false;
    }

    value.exists = true;
    value.type = type;
    value.bytes.assign(buffer.begin(), buffer.begin() + buffer_size);
    if (type == REG_SZ || type == REG_EXPAND_SZ) {
        const auto* text = reinterpret_cast<const wchar_t*>(value.bytes.data());
        const size_t characters = buffer_size / sizeof(wchar_t);
        size_t length = 0;
        while (length < characters && text[length] != L'\0') {
            ++length;
        }
        value.text.assign(text, length);
    }
    return true;
}

bool reg_read_string(HKEY root, const wchar_t* subkey, const wchar_t* name, std::wstring& value,
                     bool& exists) {
    RegValue raw;
    std::wstring error;
    if (!reg_read(root, subkey, name, raw, error)) {
        exists = false;
        return false;
    }
    exists = raw.exists;
    if (raw.exists && (raw.type == REG_SZ || raw.type == REG_EXPAND_SZ)) {
        value = raw.text;
    }
    return true;
}

bool reg_write_string(HKEY root, const wchar_t* subkey, const wchar_t* name, std::wstring_view value,
                      std::wstring& error) {
    RegKey key;
    const LSTATUS open_status = key.create(root, subkey, KEY_SET_VALUE);
    if (open_status != ERROR_SUCCESS) {
        error = L"RegCreateKeyExW(" + hkey_text(root) + L"\\" + subkey + L"): " +
                describe_status(open_status);
        return false;
    }
    const LONG status = ::RegSetValueExW(
        key.handle(), name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.data()),
        static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    if (status != ERROR_SUCCESS) {
        error = L"RegSetValueExW(" + std::wstring(name) + L"): " + describe_status(status);
        return false;
    }
    return true;
}

bool reg_write_expand_string(HKEY root, const wchar_t* subkey, const wchar_t* name,
                             std::wstring_view value, std::wstring& error) {
    RegKey key;
    const LSTATUS open_status = key.create(root, subkey, KEY_SET_VALUE);
    if (open_status != ERROR_SUCCESS) {
        error = L"RegCreateKeyExW(" + hkey_text(root) + L"\\" + subkey + L"): " +
                describe_status(open_status);
        return false;
    }
    const LONG status = ::RegSetValueExW(
        key.handle(), name, 0, REG_EXPAND_SZ, reinterpret_cast<const BYTE*>(value.data()),
        static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    if (status != ERROR_SUCCESS) {
        error = L"RegSetValueExW(" + std::wstring(name) + L"): " + describe_status(status);
        return false;
    }
    return true;
}

bool reg_write_dword(HKEY root, const wchar_t* subkey, const wchar_t* name, DWORD value,
                     std::wstring& error) {
    RegKey key;
    const LSTATUS open_status = key.create(root, subkey, KEY_SET_VALUE);
    if (open_status != ERROR_SUCCESS) {
        error = L"RegCreateKeyExW(" + hkey_text(root) + L"\\" + subkey + L"): " +
                describe_status(open_status);
        return false;
    }
    const LONG status = ::RegSetValueExW(key.handle(), name, 0, REG_DWORD,
                                         reinterpret_cast<const BYTE*>(&value), sizeof(value));
    if (status != ERROR_SUCCESS) {
        error = L"RegSetValueExW(" + std::wstring(name) + L"): " + describe_status(status);
        return false;
    }
    return true;
}

bool reg_read_dword(HKEY root, const wchar_t* subkey, const wchar_t* name, DWORD& value,
                    bool& exists) {
    RegValue raw;
    std::wstring error;
    if (!reg_read(root, subkey, name, raw, error)) {
        exists = false;
        return false;
    }
    exists = raw.exists && raw.type == REG_DWORD && raw.bytes.size() >= sizeof(DWORD);
    if (exists) {
        std::memcpy(&value, raw.bytes.data(), sizeof(DWORD));
    }
    return true;
}

bool reg_delete_value(HKEY root, const wchar_t* subkey, const wchar_t* name) {
    RegKey key;
    const LSTATUS open_status = key.open(root, subkey, KEY_SET_VALUE);
    if (open_status != ERROR_SUCCESS) {
        return open_status == ERROR_FILE_NOT_FOUND || open_status == ERROR_PATH_NOT_FOUND;
    }
    const LONG status = ::RegDeleteValueW(key.handle(), name);
    return status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND;
}

bool reg_delete_key(HKEY root, const wchar_t* subkey) {
    // RegDeleteTreeW 在当前进程的默认注册表视图里递归删除；setup 是 64 位进程，
    // 默认视图就是 64 位，和本文件其余调用一致。
    const LONG status = ::RegDeleteTreeW(root, subkey);
    return status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND;
}

bool reg_delete_key_if_empty(HKEY root, const wchar_t* subkey) {
    RegKey key;
    const LSTATUS open_status = key.open(root, subkey, KEY_QUERY_VALUE | KEY_SET_VALUE);
    if (open_status != ERROR_SUCCESS) {
        return open_status == ERROR_FILE_NOT_FOUND || open_status == ERROR_PATH_NOT_FOUND;
    }
    DWORD subkeys = 0;
    DWORD values = 0;
    const LONG status = ::RegQueryInfoKeyW(key.handle(), nullptr, nullptr, nullptr, &subkeys, nullptr,
                                           nullptr, &values, nullptr, nullptr, nullptr, nullptr);
    if (status != ERROR_SUCCESS) {
        return false;
    }
    if (subkeys != 0 || values != 0) {
        return true;
    }
    return ::RegDeleteKeyExW(root, subkey, KEY_WOW64_64KEY, 0) == ERROR_SUCCESS;
}

bool reg_key_exists(HKEY root, const wchar_t* subkey) {
    RegKey key;
    const LSTATUS status = key.open(root, subkey, KEY_QUERY_VALUE);
    if (status == ERROR_SUCCESS) {
        return true;
    }
    return status != ERROR_FILE_NOT_FOUND && status != ERROR_PATH_NOT_FOUND;
}

}  // namespace msixpkg::setup
