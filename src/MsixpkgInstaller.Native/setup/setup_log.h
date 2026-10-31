#pragma once

// 分发安装包的日志：UTF-16LE、追加、每行 `[mode] …`。
//
// 目录按可写性探测：--log-dir → %LOCALAPPDATA%\MsixpkgInstaller → %TEMP%\MsixpkgInstaller。
// 静默安装没有界面，这个文件就是唯一的现场记录，所以「写不进去」本身也要能解释：
// log.path() 为空时界面会显示「没有日志」而不是一个不存在的路径。

#include <Windows.h>

#include <filesystem>
#include <mutex>
#include <string>

#include "setup_options.h"

namespace msixpkg::setup {

class Log {
public:
    Log() = default;
    ~Log();

    Log(const Log&) = delete;
    Log& operator=(const Log&) = delete;

    /// mode 只是前缀（setup / uninstall），打开失败不视为安装失败。
    void open(const Options& options, std::wstring mode);
    void line(const std::wstring& text);

    /// 空表示一个候选目录都写不了。
    const std::filesystem::path& path() const { return path_; }
    const std::filesystem::path& directory() const { return directory_; }
    bool available() const { return !path_.empty(); }

private:
    bool try_open(const std::filesystem::path& directory, std::wstring& error);
    void append_line(const std::wstring& text);

    std::mutex mutex_;
    std::wstring mode_ = L"setup";
    std::filesystem::path directory_;
    std::filesystem::path path_;
    bool attempted_ = false;
};

}  // namespace msixpkg::setup
