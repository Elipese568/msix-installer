#pragma once

// 安装日志：`<数据根>\logs\installer-<时间戳>.log`（docs/installer-design.md §7）。
//
// 要求：每步、每个指纹、每个 HRESULT 都要留痕 —— 否则用户看到「安装失败」时
// 我们既无法自证做过什么，也无法让 repair / 工单对上账。
//
// 实现上刻意「每行打开 → 追加 → 关闭」：安装器同时存在主进程与提权助手进程，
// 长驻句柄会让两边的日志互相截断；日志不是热路径，这点开销可以接受。

#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

#include "msixpkg/errors.h"
#include "state.h"

namespace msixpkg {

class Log {
public:
    Log() = default;

    /// 在 `<roots.logs>` 下新建一份日志。失败时返回未打开的 Log（不抛）——
    /// 「日志写不了」不该让安装本身失败。
    static Log open(const DataRoots& roots, std::string_view prefix = "installer");

    /// 打开一个**已存在**的日志文件继续追加（提权助手用它写进父进程的同一份日志）。
    static Log at(const std::filesystem::path& path);

    void line(std::string_view text) const;
    void step(std::string_view phase, std::string_view detail) const;
    void fingerprint(std::string_view label, std::string_view value) const;
    void failure(const Error& error) const;

    [[nodiscard]] bool is_open() const noexcept { return !path_.empty(); }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
    [[nodiscard]] std::string path_utf8() const;

private:
    std::filesystem::path path_;
    /// 用 shared_ptr 持有互斥量：mutex 不可移动，而 open() 需要按值返回 Log。
    std::shared_ptr<std::mutex> mutex_ = std::make_shared<std::mutex>();
};

}  // namespace msixpkg
