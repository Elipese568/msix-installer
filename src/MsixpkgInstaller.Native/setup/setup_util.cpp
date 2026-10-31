#include "setup_util.h"

#include <ShlObj.h>
#include <VersionHelpers.h>

#include <algorithm>
#include <cstdio>
#include <cwchar>
#include <system_error>
#include <vector>

namespace msixpkg::setup {
namespace {

std::wstring format_message(DWORD error) {
    wchar_t* buffer = nullptr;
    const DWORD length = ::FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, error, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    if (length == 0 || buffer == nullptr) {
        return L"Win32 error " + std::to_wstring(error);
    }

    std::wstring message(buffer, length);
    ::LocalFree(buffer);
    while (!message.empty() && (message.back() == L'\r' || message.back() == L'\n' ||
                                message.back() == L' ' || message.back() == L'\t')) {
        message.pop_back();
    }
    return message;
}

}  // namespace

std::wstring get_env(const wchar_t* name) {
    const DWORD needed = ::GetEnvironmentVariableW(name, nullptr, 0);
    if (needed == 0) {
        return {};
    }
    std::wstring value(needed, L'\0');
    const DWORD written = ::GetEnvironmentVariableW(name, value.data(), needed);
    if (written == 0) {
        return {};
    }
    value.resize(written);
    return value;
}

std::filesystem::path module_path() {
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length = ::GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            return {};
        }
        if (length < buffer.size()) {
            buffer.resize(length);
            return std::filesystem::path(buffer);
        }
        buffer.resize(buffer.size() * 2);
    }
}

const std::filesystem::path& self_path() {
    static const std::filesystem::path path = module_path();
    return path;
}

std::filesystem::path known_folder(const GUID& id) {
    PWSTR raw = nullptr;
    if (FAILED(::SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &raw)) || raw == nullptr) {
        return {};
    }
    std::filesystem::path path(raw);
    ::CoTaskMemFree(raw);
    return path;
}

std::filesystem::path local_app_data() {
    return known_folder(FOLDERID_LocalAppData);
}

std::filesystem::path program_files_x64() {
    // FOLDERID_ProgramFilesX64 在 32 位进程里不可用；setup 本身是 x64，直接取。
    return known_folder(FOLDERID_ProgramFilesX64);
}

std::filesystem::path temp_directory() {
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length = ::GetTempPathW(static_cast<DWORD>(buffer.size()), buffer.data());
        if (length == 0) {
            return {};
        }
        if (length < buffer.size()) {
            buffer.resize(length);
            return std::filesystem::path(buffer);
        }
        buffer.resize(buffer.size() * 2);
    }
}

bool is_process_elevated() {
    HANDLE token = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return false;
    }

    TOKEN_ELEVATION elevation{};
    DWORD size = sizeof(elevation);
    const bool ok = ::GetTokenInformation(token, TokenElevation, &elevation, size, &size) != FALSE;
    ::CloseHandle(token);
    return ok && elevation.TokenIsElevated != 0;
}

bool is_windows_x64() {
    // IsWow64Process2 在 Win10 1709+ 才有；更老的系统上 x64 是唯一能跑这个 exe 的形态。
    using IsWow64Process2Fn = BOOL(WINAPI*)(HANDLE, USHORT*, USHORT*);
    const HMODULE kernel = ::GetModuleHandleW(L"kernel32.dll");
    if (kernel == nullptr) {
        return false;
    }
    const auto function =
        reinterpret_cast<IsWow64Process2Fn>(::GetProcAddress(kernel, "IsWow64Process2"));
    if (function == nullptr) {
        return true;
    }

    USHORT process_machine = 0;
    USHORT native_machine = 0;
    if (!function(::GetCurrentProcess(), &process_machine, &native_machine)) {
        return false;
    }
    return native_machine == IMAGE_FILE_MACHINE_AMD64 || native_machine == IMAGE_FILE_MACHINE_ARM64;
}

bool is_windows_10_or_later() {
    return ::IsWindows10OrGreater() != FALSE;
}

std::wstring format_win32_error(DWORD error) {
    return format_message(error);
}

std::wstring format_hresult(HRESULT hr) {
    wchar_t buffer[32]{};
    ::swprintf(buffer, 32, L"0x%08X", static_cast<unsigned int>(hr));
    return buffer;
}

std::wstring format_filetime_iso(const FILETIME& time) {
    FILETIME local_time{};
    SYSTEMTIME system_time{};
    if (!::FileTimeToLocalFileTime(&time, &local_time) ||
        !::FileTimeToSystemTime(&local_time, &system_time)) {
        return {};
    }
    wchar_t buffer[64]{};
    ::swprintf(buffer, 64, L"%04d-%02d-%02dT%02d:%02d:%02d", system_time.wYear, system_time.wMonth,
               system_time.wDay, system_time.wHour, system_time.wMinute, system_time.wSecond);
    return buffer;
}

std::wstring trim(std::wstring_view text) {
    size_t begin = 0;
    size_t end = text.size();
    while (begin < end && (text[begin] == L' ' || text[begin] == L'\t' || text[begin] == L'\r' ||
                           text[begin] == L'\n')) {
        ++begin;
    }
    while (end > begin && (text[end - 1] == L' ' || text[end - 1] == L'\t' || text[end - 1] == L'\r' ||
                           text[end - 1] == L'\n')) {
        --end;
    }
    return std::wstring(text.substr(begin, end - begin));
}

bool equals_ignore_case(std::wstring_view left, std::wstring_view right) {
    if (left.size() != right.size()) {
        return false;
    }
    return ::_wcsnicmp(left.data(), right.data(), left.size()) == 0;
}

bool starts_with_ignore_case(std::wstring_view text, std::wstring_view prefix) {
    return text.size() >= prefix.size() &&
           ::_wcsnicmp(text.data(), prefix.data(), prefix.size()) == 0;
}

std::string to_utf8(std::wstring_view text) {
    if (text.empty()) {
        return {};
    }
    const int size = ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                           nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return {};
    }
    std::string result(static_cast<size_t>(size), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size,
                          nullptr, nullptr);
    return result;
}

std::wstring to_utf16(std::string_view text) {
    if (text.empty()) {
        return {};
    }
    const int size = ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                           nullptr, 0);
    if (size <= 0) {
        return {};
    }
    std::wstring result(static_cast<size_t>(size), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size);
    return result;
}

std::wstring quote_argument(std::wstring_view argument) {
    // 与 CommandLineToArgvW 兼容的转义：反斜杠只在引号前翻倍，引号前还要补一个。
    const bool needs_quotes =
        argument.empty() ||
        std::any_of(argument.begin(), argument.end(),
                    [](wchar_t ch) { return ch == L' ' || ch == L'\t' || ch == L'\n' || ch == L'"'; });
    if (!needs_quotes) {
        return std::wstring(argument);
    }

    std::wstring result;
    result.push_back(L'"');
    size_t backslashes = 0;
    for (const wchar_t ch : argument) {
        if (ch == L'\\') {
            ++backslashes;
            continue;
        }
        if (ch == L'"') {
            result.append(backslashes * 2 + 1, L'\\');
            result.push_back(L'"');
        } else {
            result.append(backslashes, L'\\');
            result.push_back(ch);
        }
        backslashes = 0;
    }
    result.append(backslashes * 2, L'\\');
    result.push_back(L'"');
    return result;
}

std::wstring join_arguments(const std::vector<std::wstring>& arguments) {
    std::wstring command_line;
    for (const std::wstring& argument : arguments) {
        if (!command_line.empty()) {
            command_line.push_back(L' ');
        }
        command_line += quote_argument(argument);
    }
    return command_line;
}

bool write_file_bytes(const std::filesystem::path& path, const void* data, size_t size,
                      std::wstring& error) {
    const HANDLE file = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                      FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = L"CreateFileW(" + path.wstring() + L"): " + format_win32_error(::GetLastError());
        return false;
    }

    size_t written = 0;
    const auto* bytes = static_cast<const BYTE*>(data);
    while (written < size) {
        DWORD chunk = 0;
        const DWORD remaining = static_cast<DWORD>(std::min<size_t>(size - written, 1u << 20));
        if (!::WriteFile(file, bytes + written, remaining, &chunk, nullptr) || chunk == 0) {
            error = L"WriteFile(" + path.wstring() + L"): " + format_win32_error(::GetLastError());
            ::CloseHandle(file);
            return false;
        }
        written += chunk;
    }
    ::FlushFileBuffers(file);
    ::CloseHandle(file);
    return true;
}

bool read_file_header(const std::filesystem::path& path, void* buffer, size_t size, size_t& read,
                      std::wstring& error) {
    read = 0;
    const HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = L"CreateFileW(" + path.wstring() + L"): " + format_win32_error(::GetLastError());
        return false;
    }

    DWORD got = 0;
    const bool ok = ::ReadFile(file, buffer, static_cast<DWORD>(size), &got, nullptr) != FALSE;
    ::CloseHandle(file);
    if (!ok) {
        error = L"ReadFile(" + path.wstring() + L"): " + format_win32_error(::GetLastError());
        return false;
    }
    read = got;
    return true;
}

}  // namespace msixpkg::setup
