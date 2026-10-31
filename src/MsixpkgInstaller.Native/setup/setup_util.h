#pragma once

// setup 与 Uninstall.exe 共用的小工具：路径、已知文件夹、权限、错误文本、引号。
//
// 这里刻意不依赖 src/（安装器核心）：setup 是独立进程，只应该拖进 Windows 自带库，
// 这样「安装包的安装包」不会被 MSIX 部署栈（C++/WinRT）与证书代码牵连。

#include <Windows.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace msixpkg::setup {

// 产品常量（注册表 / 目录名一律语言中立；界面文案另走 Strings）。
inline constexpr wchar_t kProductName[] = L"Msixpkg Installer";
inline constexpr wchar_t kProductFolder[] = L"Msixpkg Installer";
inline constexpr wchar_t kCfgKey[] = L"Software\\MsixpkgInstaller";
inline constexpr wchar_t kUninstallKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\MsixpkgInstaller";
inline constexpr wchar_t kCapabilitiesKey[] = L"Software\\MsixpkgInstaller\\Capabilities";
inline constexpr wchar_t kRegisteredApplications[] = L"Software\\RegisteredApplications";
inline constexpr wchar_t kClassesKey[] = L"Software\\Classes";
inline constexpr wchar_t kExtension[] = L".msixpkg";
inline constexpr wchar_t kProgId[] = L"Msixpkg.Package";
inline constexpr wchar_t kMachinePathKey[] =
    L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment";
inline constexpr wchar_t kUserPathKey[] = L"Environment";
inline constexpr wchar_t kGuiExeName[] = L"msixpkg-installer.exe";
inline constexpr wchar_t kCliExeName[] = L"msixpkg-native.exe";
inline constexpr wchar_t kUninstallerExeName[] = L"Uninstall.exe";

// 退出码（与旧分发安装包保持一致，CI / 文档已有依赖）。
inline constexpr int kExitSuccess = 0;
inline constexpr int kExitIncomplete = 4;   // 文件没真的写进去 / 删不掉
inline constexpr int kExitNeedAdmin = 5;    // 需要管理员，或提权被拒绝
inline constexpr int kExitPrecondition = 6; // 参数 / 平台 / 前置条件不满足（含取消）
inline constexpr int kExitBusy = 7;         // 目标文件被占用（向导正在运行）

std::wstring get_env(const wchar_t* name);
std::filesystem::path module_path();
std::filesystem::path known_folder(const GUID& id);
std::filesystem::path local_app_data();
std::filesystem::path program_files_x64();
std::filesystem::path temp_directory();

bool is_process_elevated();
bool is_windows_x64();       // 本机是 64 位 Windows（x64 / arm64）
bool is_windows_10_or_later();

std::wstring format_win32_error(DWORD error);
std::wstring format_hresult(HRESULT hr);
std::wstring format_filetime_iso(const FILETIME& time);

std::wstring trim(std::wstring_view text);
bool equals_ignore_case(std::wstring_view left, std::wstring_view right);
bool starts_with_ignore_case(std::wstring_view text, std::wstring_view prefix);

std::string to_utf8(std::wstring_view text);
std::wstring to_utf16(std::string_view text);

// 按 CommandLineToArgvW 的规则给一个参数加引号（提权重入时用）。
std::wstring quote_argument(std::wstring_view argument);
std::wstring join_arguments(const std::vector<std::wstring>& arguments);

// 记录当前进程的可执行文件路径；提权重入与自删除都用它，避免各处重复 GetModuleFileName。
const std::filesystem::path& self_path();

bool write_file_bytes(const std::filesystem::path& path, const void* data, size_t size,
                      std::wstring& error);
bool read_file_header(const std::filesystem::path& path, void* buffer, size_t size, size_t& read,
                      std::wstring& error);

}  // namespace msixpkg::setup
