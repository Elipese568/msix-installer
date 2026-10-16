#pragma once

// 界面文案的运行时取用（data source 是 res/strings.rc 的两张字符串表）。
//
// 为什么不直接在各处调 LoadStringW：
//  1. LoadStringW **没有 langid 参数**，它按 FindResource 的语言回落链选表，而那条链
//     只命中完全相同的 langid（用户语言是 zh-TW / en-GB 时不会回落到 zh-CN / en-US 表，
//     界面上就会出现空白按钮）；
//  2. 「取不到文案」必须在开发期大声暴露，而不是让用户看到空白标签。
//
// 所以这里自己按 RT_STRING 的分块结构（每块 16 条）用 FindResourceExW 精确取表，
// 并显式回落到 en-US —— 回落链由我们决定，不由系统猜。

#include <Windows.h>

#include <cstdint>
#include <string>
#include <string_view>

#include "resource.h"

namespace msixpkg::gui {

/// 一张选定的语言表 + 从它取文案。
class Strings {
public:
    /// 按「用户首选 UI 语言 → 简体中文 → 美式英语」的顺序挑一张存在的表。
    /// 必须在任何界面创建之前调用一次。
    void load(void* instance);

    [[nodiscard]] std::wstring_view get(int id) const;
    /// 直接拿 std::wstring（需要拼接时用）。
    [[nodiscard]] std::wstring text(int id) const { return std::wstring(get(id)); }

    /// 实际选中的 langid（诊断 / 截图核对用）。
    [[nodiscard]] WORD language() const noexcept { return language_; }
    /// 选中表里缺项的条数（0 = 完整）。开发期用它做自检。
    [[nodiscard]] size_t missing_count() const noexcept { return missing_; }

private:
    void* instance_ = nullptr;
    WORD language_ = 0;
    size_t missing_ = 0;
    std::wstring slots_[MSIXPKG_STRING_SLOT_COUNT];
};

/// 用选定语言（拿不到就按当前线程语言）格式化系统消息，例如 FormatMessage 的错误文本。
std::wstring system_message(DWORD code);

/// 内存大小：自动选 B / KB / MB / GB。
std::wstring format_size(uint64_t bytes);
/// 十六进制指纹的分组显示：`ab12 cd34 …`（便于人工比对）。
std::wstring format_fingerprint(std::wstring_view hex);
/// `printf` 风格的窄字符串拼接（只用于内部诊断文本，界面文案一律来自字符串表）。
std::wstring format(const wchar_t* pattern, ...);

}  // namespace msixpkg::gui
