#pragma once

// 注册表小封装：统一 64 位视图、统一错误文本。
//
// 安装记录 / ARP / 文件关联 / PATH 都走这里，避免散落的 RegOpenKeyEx 各自忘记
// KEY_WOW64_64KEY（在 32 位宿主上读写到 Wow6432Node 是经典事故）。
//
// 注意：注册表 API **直接返回** LSTATUS，不设置 last error。所有错误文本都从返回值
// 格式化；拿 GetLastError() 会把上一次无关调用的 183/6 当成真正原因（实测踩过）。

#include <Windows.h>

#include <string>
#include <string_view>
#include <vector>

namespace msixpkg::setup {

class RegKey {
public:
    RegKey() = default;
    ~RegKey();

    RegKey(const RegKey&) = delete;
    RegKey& operator=(const RegKey&) = delete;

    LSTATUS open(HKEY root, const wchar_t* subkey, REGSAM access);
    LSTATUS create(HKEY root, const wchar_t* subkey, REGSAM access);
    void close();

    HKEY handle() const { return key_; }
    bool valid() const { return key_ != nullptr; }

private:
    HKEY key_ = nullptr;
};

struct RegValue {
    bool exists = false;
    DWORD type = REG_NONE;
    std::wstring text;          // REG_SZ / REG_EXPAND_SZ
    std::vector<uint8_t> bytes; // 其他类型（目前只给 PATH 的原始读取用）
};

bool reg_read(HKEY root, const wchar_t* subkey, const wchar_t* name, RegValue& value,
              std::wstring& error);
bool reg_read_string(HKEY root, const wchar_t* subkey, const wchar_t* name, std::wstring& value,
                     bool& exists);
bool reg_write_string(HKEY root, const wchar_t* subkey, const wchar_t* name, std::wstring_view value,
                      std::wstring& error);
bool reg_write_expand_string(HKEY root, const wchar_t* subkey, const wchar_t* name,
                             std::wstring_view value, std::wstring& error);
bool reg_write_dword(HKEY root, const wchar_t* subkey, const wchar_t* name, DWORD value,
                     std::wstring& error);
bool reg_read_dword(HKEY root, const wchar_t* subkey, const wchar_t* name, DWORD& value,
                    bool& exists);
bool reg_delete_value(HKEY root, const wchar_t* subkey, const wchar_t* name);
bool reg_delete_key(HKEY root, const wchar_t* subkey);
bool reg_delete_key_if_empty(HKEY root, const wchar_t* subkey);
bool reg_key_exists(HKEY root, const wchar_t* subkey);

std::wstring hkey_text(HKEY root);

}  // namespace msixpkg::setup
