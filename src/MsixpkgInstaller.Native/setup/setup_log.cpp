#include "setup_log.h"

#include "setup_util.h"

#include <cstdio>
#include <system_error>
#include <vector>

namespace msixpkg::setup {
namespace {

std::wstring now_text() {
    SYSTEMTIME local{};
    ::GetLocalTime(&local);
    wchar_t buffer[32]{};
    ::swprintf(buffer, 32, L"%04d-%02d-%02d %02d:%02d:%02d", local.wYear, local.wMonth, local.wDay,
               local.wHour, local.wMinute, local.wSecond);
    return buffer;
}

}  // namespace

Log::~Log() = default;

void Log::open(const Options& options, std::wstring mode) {
    std::lock_guard<std::mutex> guard(mutex_);
    mode_ = std::move(mode);

    std::vector<std::filesystem::path> candidates;
    if (!options.log_dir.empty()) {
        candidates.emplace_back(options.log_dir);
    } else {
        const std::wstring from_environment = get_env(L"MSIXPKG_SETUP_LOG_DIR");
        if (!from_environment.empty()) {
            candidates.emplace_back(from_environment);
        }
    }
    const std::filesystem::path local = local_app_data();
    if (!local.empty()) {
        candidates.push_back(local / L"MsixpkgInstaller");
    }
    const std::filesystem::path temp = temp_directory();
    if (!temp.empty()) {
        candidates.push_back(temp / L"MsixpkgInstaller");
    }

    attempted_ = true;
    for (const std::filesystem::path& directory : candidates) {
        std::wstring error;
        if (try_open(directory, error)) {
            return;
        }
    }
}

bool Log::try_open(const std::filesystem::path& directory, std::wstring& error) {
    std::error_code code;
    std::filesystem::create_directories(directory, code);
    if (code) {
        error = L"create_directories(" + directory.wstring() + L"): " + to_utf16(code.message());
        return false;
    }

    const std::filesystem::path candidate = directory / L"setup.log";
    const HANDLE file = ::CreateFileW(candidate.c_str(), FILE_APPEND_DATA,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                                      FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = L"CreateFileW(" + candidate.wstring() + L"): " + format_win32_error(::GetLastError());
        return false;
    }
    ::SetFilePointer(file, 0, nullptr, FILE_END);
    ::CloseHandle(file);

    directory_ = directory;
    path_ = candidate;
    return true;
}

void Log::line(const std::wstring& text) {
    std::lock_guard<std::mutex> guard(mutex_);
    if (path_.empty()) {
        return;
    }
    append_line(text);
}

void Log::append_line(const std::wstring& text) {
    const std::wstring line = L"[" + mode_ + L"] " + text + L"\r\n";

    const HANDLE file = ::CreateFileW(path_.c_str(), FILE_APPEND_DATA,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                                      FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }
    ::SetFilePointer(file, 0, nullptr, FILE_END);

    // 用 UTF-16LE 原样写：日志里有中文，ANSI 在别的代码页机器上会变乱码。
    DWORD written = 0;
    ::WriteFile(file, line.data(), static_cast<DWORD>(line.size() * sizeof(wchar_t)), &written, nullptr);
    ::FlushFileBuffers(file);
    ::CloseHandle(file);
}

}  // namespace msixpkg::setup
