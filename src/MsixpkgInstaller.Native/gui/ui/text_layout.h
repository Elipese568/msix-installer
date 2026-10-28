#pragma once

// 只读文本的换行算法。TextView / Table / 多行 Label 共用。
//
// 为什么不用 GDI+ 自己的 DrawString 换行：TextView 要按「行」滚动、要算总行数、
// 要让 Home/End/PageUp/PageDown 落在真实的行上。让 GDI+ 在绘制时自由换行就没法
// 拿到行模型了，所以先在 DIP 空间算好行，再逐行绘制 —— 绘制与测量用的是同一份
// 宽度，不会出现「算 10 行、画 11 行」。

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace msixpkg::ui {

struct WrappedLine {
    std::wstring text;
    float width = 0.0f;
};

using MeasureWidth = std::function<float(std::wstring_view)>;

/// 按 max_width（DIP）贪心换行。显式 '\n' 强制断行；CJK 逐字可断，
/// 拉丁文按词断（超长单词再逐字拆）。返回的每行已经去掉行尾空格。
std::vector<WrappedLine> wrap_text(std::wstring_view text, float max_width, const MeasureWidth& measure);

/// 是否为可以逐字断行的 CJK 字符（含全角标点、假名、谚文）。
bool is_breakable_cjk(wchar_t c);

}  // namespace msixpkg::ui
