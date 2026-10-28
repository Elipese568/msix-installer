// 框架里不依赖 HWND 的纯逻辑测试：换行、滚动条几何、颜色、命中测试。
//
// 这些是截图回归之前最便宜的一道闸：换行算法错了会表现为「行数对不上、滚动范围
// 不对」，肉眼很难从一张 PNG 上看出来。运行方式见 gui/ui/README.md。

#include "canvas.h"
#include "controls.h"
#include "text_layout.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void check(bool condition, const char* name) {
    std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", name);
    if (!condition) {
        ++g_failures;
    }
}

bool close_enough(float left, float right) {
    return std::fabs(left - right) < 0.01f;
}

// 每个字符宽 8 DIP（CJK 也按 1 个字符算），便于断言。
float measure_eight(std::wstring_view text) {
    return static_cast<float>(text.size()) * 8.0f;
}

void test_wrap() {
    using namespace msixpkg::ui;

    {
        const auto lines = wrap_text(L"hello world foo", 90.0f, measure_eight);
        check(lines.size() >= 2, "latin text wraps at word boundary");
        check(lines[0].text == L"hello world", "latin first line keeps whole words");
    }

    {
        const auto lines = wrap_text(L"中文中文中文", 32.0f, measure_eight);
        check(lines.size() == 2, "cjk breaks per character");
        check(lines[0].text == L"中文中文", "cjk first line has four chars");
    }

    {
        const auto lines = wrap_text(L"abcdefghij", 40.0f, measure_eight);
        check(lines.size() == 2, "over-long word is hard-split");
        check(lines[0].text == L"abcde", "over-long word splits at width");
    }

    {
        const auto lines = wrap_text(L"a\nb", 100.0f, measure_eight);
        check(lines.size() == 2, "explicit newline forces a line");
        check(lines[1].text == L"b", "content after newline is preserved");
    }

    {
        const auto lines = wrap_text(L"a\n\nb", 100.0f, measure_eight);
        check(lines.size() == 3, "empty paragraph keeps its blank line");
        check(lines[1].text.empty(), "middle line is empty");
    }

    {
        const auto lines = wrap_text(L"tail   ", 100.0f, measure_eight);
        check(lines.size() == 1 && lines[0].text == L"tail", "trailing spaces are trimmed");
    }

    {
        const auto lines = wrap_text(L"", 100.0f, measure_eight);
        check(lines.empty(), "empty text produces no lines");
    }

    {
        const auto lines = wrap_text(L"\r\nSecond", 120.0f, measure_eight);
        check(lines.size() == 2, "crlf is treated as one line break");
        check(lines[1].text == L"Second", "crlf does not leak CR into the line");
    }
}

void test_scrollbar() {
    using namespace msixpkg::ui;

    const RectF area{0.0f, 0.0f, 100.0f, 100.0f};

    {
        const auto metrics = scrollbar_metrics(area, 400.0f, 100.0f, 0.0f);
        check(metrics.visible, "scrollbar visible when content overflows");
        check(close_enough(metrics.thumb.height, 25.0f), "thumb height is proportional");
        check(close_enough(metrics.thumb.y, 0.0f), "thumb starts at top");
    }

    {
        const auto metrics = scrollbar_metrics(area, 200.0f, 100.0f, 50.0f);
        check(close_enough(metrics.thumb.height, 50.0f), "half content -> half thumb");
        check(close_enough(metrics.thumb.y, 25.0f), "half scroll -> half travel");
    }

    {
        const auto metrics = scrollbar_metrics(area, 100.0f, 100.0f, 0.0f);
        check(!metrics.visible, "no scrollbar when content fits");
    }

    {
        const float scroll = scroll_for_thumb_y(area, 200.0f, 100.0f, 50.0f, 50.0f);
        check(close_enough(scroll, 100.0f), "drag to bottom maps to max scroll");
        check(close_enough(scroll_for_thumb_y(area, 200.0f, 100.0f, 0.0f, 50.0f), 0.0f), "drag at top maps to zero");
        check(close_enough(scroll_for_thumb_y(area, 200.0f, 100.0f, 500.0f, 50.0f), 100.0f), "drag past bottom clamps");
    }
}

void test_geometry_and_color() {
    using namespace msixpkg::ui;

    const RectF rect{10.0f, 20.0f, 100.0f, 40.0f};
    check(rect.contains(10.0f, 20.0f), "rect contains top-left");
    check(rect.contains(109.9f, 59.9f), "rect contains inside bottom-right");
    check(!rect.contains(110.0f, 60.0f), "rect right/bottom are exclusive");
    check(close_enough(rect.inset(5.0f).width, 90.0f), "inset shrinks width");
    check(close_enough(rect.right(), 110.0f), "right edge");

    const Color color = Color::from_argb(0x80FF8000u);
    check(close_enough(color.a, 128.0f / 255.0f), "argb alpha decoded");
    check(close_enough(color.r, 1.0f) && close_enough(color.g, 128.0f / 255.0f) && close_enough(color.b, 0.0f), "argb channels decoded");
    check(close_enough(color.mix(Color::black(), 0.5f).r, 0.5f), "color mix halfway");
}

}  // namespace

int wmain() {
    test_wrap();
    test_scrollbar();
    test_geometry_and_color();

    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL PASS" : "FAILURES", g_failures,
                g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
