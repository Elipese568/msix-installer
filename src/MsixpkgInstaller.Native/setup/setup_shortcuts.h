#pragma once

// 快捷方式（IShellLink）+ 已知文件夹。COM 必须在主线程初始化（setup_main 负责）。

#include <Windows.h>

#include <filesystem>
#include <string>

namespace msixpkg::setup {

std::filesystem::path start_menu_folder(bool all_users);
std::filesystem::path desktop_folder(bool all_users);
std::filesystem::path public_start_menu();

struct ShortcutSpec {
    std::filesystem::path link;
    std::filesystem::path target;
    std::wstring arguments;
    std::filesystem::path working_directory;
    std::filesystem::path icon;
    std::wstring description;
};

bool create_shortcut(const ShortcutSpec& spec, std::wstring& error);
bool delete_file_checked(const std::filesystem::path& path, std::wstring& error);
bool remove_directory_tree(const std::filesystem::path& path, std::wstring& error);
bool directory_is_empty(const std::filesystem::path& path);
bool file_size(const std::filesystem::path& path, uint64_t& size);

}  // namespace msixpkg::setup
