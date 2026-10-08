#include "log.h"

#include <Windows.h>

#include <cstdio>
#include <fstream>
#include <string>

#include "internal.h"

namespace msixpkg {
namespace {

/// 本地时间 + 毫秒，便于和用户口述的「几点几分失败」对上。
std::string timestamp() {
    SYSTEMTIME now{};
    ::GetLocalTime(&now);
    char buffer[40] = {};
    std::snprintf(
        buffer,
        sizeof(buffer),
        "%04u-%02u-%02u %02u:%02u:%02u.%03u",
        static_cast<unsigned>(now.wYear),
        static_cast<unsigned>(now.wMonth),
        static_cast<unsigned>(now.wDay),
        static_cast<unsigned>(now.wHour),
        static_cast<unsigned>(now.wMinute),
        static_cast<unsigned>(now.wSecond),
        static_cast<unsigned>(now.wMilliseconds));
    return buffer;
}

std::string file_timestamp() {
    SYSTEMTIME now{};
    ::GetLocalTime(&now);
    char buffer[32] = {};
    std::snprintf(
        buffer,
        sizeof(buffer),
        "%04u%02u%02u-%02u%02u%02u-%03u",
        static_cast<unsigned>(now.wYear),
        static_cast<unsigned>(now.wMonth),
        static_cast<unsigned>(now.wDay),
        static_cast<unsigned>(now.wHour),
        static_cast<unsigned>(now.wMinute),
        static_cast<unsigned>(now.wSecond),
        static_cast<unsigned>(now.wMilliseconds));
    return buffer;
}

}  // namespace

Log Log::open(const DataRoots& roots, std::string_view prefix) {
    Log log;
    const std::filesystem::path directory = roots.logs;
    const std::filesystem::path path =
        directory / (std::string(prefix) + "-" + file_timestamp() + "-" + new_install_id().substr(0, 8) + ".log");

    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) {
        return log;
    }

    std::ofstream stream(path, std::ios::binary | std::ios::app);
    if (!stream) {
        return log;
    }

    stream << "# msixpkg installer log\n";
    stream << "# started " << utc_now() << "\n";
    stream.close();

    log.path_ = path;
    return log;
}

Log Log::at(const std::filesystem::path& path) {
    Log log;
    if (path.empty()) {
        return log;
    }

    const std::filesystem::path directory = path.parent_path();
    std::error_code error;
    if (!directory.empty()) {
        std::filesystem::create_directories(directory, error);
    }

    std::ofstream stream(path, std::ios::binary | std::ios::app);
    if (!stream) {
        return log;
    }

    stream.close();
    log.path_ = path;
    return log;
}

void Log::line(std::string_view text) const {
    if (path_.empty()) {
        return;
    }

    const std::lock_guard<std::mutex> guard(*mutex_);
    std::ofstream stream(path_, std::ios::binary | std::ios::app);
    if (!stream) {
        return;
    }

    stream << '[' << timestamp() << "] " << text << '\n';
}

void Log::step(std::string_view phase, std::string_view detail) const {
    line(std::string("phase ") + std::string(phase) + ": " + std::string(detail));
}

void Log::fingerprint(std::string_view label, std::string_view value) const {
    line(std::string("  ") + std::string(label) + " = " + std::string(value));
}

void Log::failure(const Error& error) const {
    line("FAILED " + error.to_string());
}

std::string Log::path_utf8() const {
    return narrow_utf8(path_.wstring());
}

}  // namespace msixpkg
