#include "text_layout.h"

#include <algorithm>

namespace msixpkg::ui {
namespace {

bool is_space(wchar_t c) {
    return c == L' ' || c == L'\t' || c == L'\r' || c == 0x3000;  // 含 CR 与全角空格
}

bool is_cjk_range(wchar_t c) {
    return (c >= 0x1100 && c <= 0x11FF) ||    // 谚文字母
           (c >= 0x2E80 && c <= 0x303F) ||    // CJK 部首、标点
           (c >= 0x3040 && c <= 0x30FF) ||    // 假名
           (c >= 0x3400 && c <= 0x4DBF) ||    // 扩展 A
           (c >= 0x4E00 && c <= 0x9FFF) ||    // 基本区
           (c >= 0xA000 && c <= 0xA4CF) ||    // 彝文
           (c >= 0xAC00 && c <= 0xD7AF) ||    // 谚文音节
           (c >= 0xF900 && c <= 0xFAFF) ||    // 兼容表意
           (c >= 0xFE30 && c <= 0xFE4F) ||    // 兼容标点
           (c >= 0xFF00 && c <= 0xFF60) ||    // 全角字符
           (c >= 0xFFE0 && c <= 0xFFE6);
}

void push_line(std::vector<WrappedLine>& lines, std::wstring& line, float& width) {
    while (!line.empty() && is_space(line.back())) {
        line.pop_back();
    }

    WrappedLine wrapped;
    wrapped.text = std::move(line);
    wrapped.width = width;
    lines.push_back(std::move(wrapped));
    line.clear();
    width = 0.0f;
}

}  // namespace

bool is_breakable_cjk(wchar_t c) {
    return is_cjk_range(c);
}

std::vector<WrappedLine> wrap_text(std::wstring_view text, float max_width, const MeasureWidth& measure) {
    std::vector<WrappedLine> lines;
    if (text.empty() || max_width <= 1.0f || !measure) {
        return lines;
    }

    std::wstring line;
    float line_width = 0.0f;

    std::size_t index = 0;
    while (index <= text.size()) {
        // 先切出一个段落（显式 '\n' 之间）。
        std::size_t end = text.find(L'\n', index);
        if (end == std::wstring_view::npos) {
            end = text.size();
        }

        const std::wstring_view paragraph = text.substr(index, end - index);

        // 段落内再切成「一个 CJK 字 / 一个拉丁词 / 一个空格」这样的原子 token。
        std::size_t cursor = 0;
        while (cursor < paragraph.size()) {
            const wchar_t c = paragraph[cursor];
            if (is_space(c)) {
                if (!line.empty() && !is_space(line.back())) {
                    const std::wstring space(1, L' ');
                    line += space;
                    line_width += measure(space);
                }

                ++cursor;
                continue;
            }

            std::wstring token;
            if (is_cjk_range(c)) {
                token.assign(1, c);
                ++cursor;
            } else {
                while (cursor < paragraph.size() && !is_space(paragraph[cursor]) && !is_cjk_range(paragraph[cursor])) {
                    token.push_back(paragraph[cursor]);
                    ++cursor;
                }
            }

            const float token_width = measure(token);

            // 单个 token 就超过一行：按字符硬拆（长 URL、无空格路径）。
            if (token_width > max_width && token.size() > 1) {
                for (const wchar_t ch : token) {
                    const std::wstring piece(1, ch);
                    const float piece_width = measure(piece);
                    if (line_width + piece_width > max_width && !line.empty()) {
                        push_line(lines, line, line_width);
                    }

                    line += piece;
                    line_width += piece_width;
                }

                continue;
            }

            if (line_width + token_width > max_width && !line.empty()) {
                push_line(lines, line, line_width);
            }

            line += token;
            line_width += token_width;
        }

        // 段落结束：即使空段落也保留一行，保证 '\n\n' 不丢空行。
        push_line(lines, line, line_width);

        if (end == text.size()) {
            break;
        }

        index = end + 1;
    }

    return lines;
}

}  // namespace msixpkg::ui
