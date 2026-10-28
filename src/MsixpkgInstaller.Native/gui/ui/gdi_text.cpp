#include "gdi_text.h"

#include <algorithm>
#include <cmath>

namespace msixpkg::ui {
namespace {

RECT to_pixels(const RectF& rect, float scale) {
    return {static_cast<LONG>(std::lround(rect.x * scale)), static_cast<LONG>(std::lround(rect.y * scale)),
            static_cast<LONG>(std::lround(rect.right() * scale)),
            static_cast<LONG>(std::lround(rect.bottom() * scale))};
}

BYTE channel(float value) {
    return static_cast<BYTE>(std::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f);
}

}  // namespace

GdiTextRenderer::~GdiTextRenderer() {
    clear();
}

void GdiTextRenderer::clear() {
    for (const auto& [key, font] : fonts_) {
        (void)key;
        if (font != nullptr) {
            ::DeleteObject(font);
        }
    }

    fonts_.clear();
}

HFONT GdiTextRenderer::font_for(const TextStyle& style, float dpi) {
    const int pixel_height = std::max(1, static_cast<int>(std::lround(style.size * dpi / 96.0f)));
    const std::wstring key = style.family + L"|" + std::to_wstring(pixel_height) + L"|" +
                             std::to_wstring(style.weight) + (style.italic ? L"|i" : L"|");
    const auto found = fonts_.find(key);
    if (found != fonts_.end()) {
        return found->second;
    }

    // 负高度 = 字符高度；GDI 的 cell height 与 GDI+ 的 UnitPixel em size 对
    // YaHei/Consolas 这类字体基本一致，所以两者视觉大小对得上。
    const HFONT font = ::CreateFontW(
        -pixel_height, 0, 0, 0, style.weight, style.italic ? TRUE : FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
        style.family.c_str());
    if (font == nullptr) {
        return nullptr;
    }

    fonts_.emplace(key, font);
    return font;
}

void GdiTextRenderer::draw(HDC dc, const TextCommand& command, float dpi) {
    if (dc == nullptr || command.text.empty() || command.rect.empty()) {
        return;
    }

    const float scale = dpi / 96.0f;
    const HFONT font = font_for(command.style, dpi);
    if (font == nullptr) {
        return;
    }

    RECT rect = to_pixels(command.rect, scale);

    const int saved = ::SaveDC(dc);
    if (!command.clip.empty()) {
        const RECT clip = to_pixels(command.clip, scale);
        ::IntersectClipRect(dc, clip.left, clip.top, clip.right, clip.bottom);
    }

    ::SetBkMode(dc, TRANSPARENT);
    ::SetTextColor(dc, RGB(channel(command.color.r), channel(command.color.g), channel(command.color.b)));

    UINT flags = DT_NOPREFIX;
    if (!command.style.wrap) {
        flags |= DT_SINGLELINE;
    }

    if (command.style.ellipsis) {
        flags |= DT_END_ELLIPSIS;
    }

    switch (command.style.align) {
        case TextAlign::Center: flags |= DT_CENTER; break;
        case TextAlign::Right: flags |= DT_RIGHT; break;
        case TextAlign::Left: break;
    }

    if (!command.style.wrap) {
        switch (command.style.valign) {
            case TextVAlign::Center: flags |= DT_VCENTER; break;
            case TextVAlign::Bottom: flags |= DT_BOTTOM; break;
            case TextVAlign::Top: break;
        }
    }

    const HGDIOBJ old = ::SelectObject(dc, font);
    ::DrawTextW(dc, command.text.c_str(), static_cast<int>(command.text.size()), &rect, flags);
    ::SelectObject(dc, old);
    ::RestoreDC(dc, saved);
}

}  // namespace msixpkg::ui
