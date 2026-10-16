#pragma once

// 发布者钉扎策略（docs/security-model.md §1）。
//
// 三个来源合并成「允许的发布者 SHA-256 指纹」集合：
//
//  1. **管理员预先部署的机器级白名单**
//     `HKLM\SOFTWARE\MsixpkgInstaller` 的值 `AllowedPublishers`
//     （REG_MULTI_SZ，一个指纹一项；也接受用 `;` `,` 或换行分隔的 REG_SZ），
//     以及同名**子键** `...\AllowedPublishers`（值名随意，值数据是指纹）——
//     两种约定都支持，因为白名单是运维手工建的，不该逼他们猜我们喜欢哪种。
//  2. **本次调用显式给的 `--pin`**（由调用方合并，不在本模块里）。
//  3. **用户之前明确勾选过「信任此发布者（将记住）」的 TOFU 记录**
//     —— 落在 `<数据根>\publishers.json`，与状态库同一处、同样原子写。
//
// 没有第 3 条的话，「将记住」就是一句空话：用户每次安装都会被再问一遍，
// 而 UI 上的复选框却在暗示「以后不用再问了」。

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "log.h"
#include "state.h"

namespace msixpkg {

/// 机器级白名单的注册表位置（security-model.md §1 指定的路径）。
inline constexpr wchar_t kPublisherPolicyKey[] = L"SOFTWARE\\MsixpkgInstaller";
inline constexpr wchar_t kPublisherPolicyValue[] = L"AllowedPublishers";

struct PublisherPolicy {
    /// 管理员部署的机器级白名单（小写 64 位十六进制）。
    std::vector<std::string> machine_pinned;
    /// 用户通过「信任此发布者（将记住）」确认过的发布者。
    std::vector<std::string> remembered;
    /// 读取过程中的问题（注册表读不到、记录文件损坏…），由调用方写进日志。
    std::vector<std::string> warnings;

    /// 合并去重后的完整白名单。
    [[nodiscard]] std::vector<std::string> allowed() const;
    [[nodiscard]] bool is_pinned(std::string_view sha256) const;
};

/// 读取策略。**永不抛**：读不到就是空白名单（默认拒绝是安全方向）。
PublisherPolicy load_publisher_policy(const DataRoots& roots);

/// TOFU 记录文件：`<数据根>\publishers.json`。
std::filesystem::path remembered_publishers_path(const DataRoots& roots);

/// 记住一个发布者（原子写 + 调用方负责写审计日志）。
/// 返回 false 表示没能落盘（磁盘/权限问题）—— 调用方只记警告，不因此让安装失败。
bool remember_publisher(
    const DataRoots& roots,
    std::string_view sha256,
    std::string_view subject);

/// 规范化一个指纹：去空白、转小写；不是 64 位十六进制时返回空串。
std::string normalize_thumbprint(std::string_view thumbprint);

}  // namespace msixpkg
