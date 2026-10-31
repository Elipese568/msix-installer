#pragma once

// PATH 的纯逻辑（可单测）+ 注册表读写。
//
// 两条不变量（来自旧 msixpkg-path.nsh，已由 13 个 NSIS 用例锁定，这里原样保留）：
//   1. 只按 ';' 分段做**整段**比较，绝不做子串替换 —— 否则 C:\a 会误伤 C:\ab；
//   2. 写回时保留原来的 REG_SZ / REG_EXPAND_SZ 类型，绝不把用户的长 PATH 截断。
//
// C++ 版本没有 NSIS_MAX_STRLEN 上限：读之前先用 RegQueryValueExW 问真实字节数，
// 再按需分配，所以「PATH 太长就拒绝改动」在这里只作为异常防御（上限 1M 字符）。

#include <Windows.h>

#include <string>
#include <string_view>
#include <vector>

namespace msixpkg::setup {

std::vector<std::wstring> split_path(std::wstring_view value);
std::wstring join_path(const std::vector<std::wstring>& segments);

/// 条目比较：忽略大小写、去掉两端的引号与结尾的 \ 或 /。不做任何其他规范化。
bool path_entry_equal(std::wstring_view left, std::wstring_view right);
bool path_contains(std::wstring_view value, std::wstring_view entry);

/// 返回新值；changed 表示是否真的动了。保留原有空段与顺序。
std::wstring path_add(std::wstring_view value, std::wstring_view entry, bool& changed);
std::wstring path_remove(std::wstring_view value, std::wstring_view entry, bool& changed);

struct PathValue {
    bool exists = false;
    DWORD type = REG_EXPAND_SZ;
    std::wstring value;
};

/// 读写当前用户 / 所有用户的 PATH。root 决定键：HKLM 或 HKCU。
bool read_environment_path(HKEY root, PathValue& value, std::wstring& error);
bool write_environment_path(HKEY root, const PathValue& value, std::wstring& error);
bool delete_environment_path(HKEY root, std::wstring& error);
void broadcast_environment_change();

}  // namespace msixpkg::setup
