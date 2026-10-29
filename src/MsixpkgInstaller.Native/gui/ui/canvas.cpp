#include "canvas.h"

#include <algorithm>
#include <cmath>

namespace msixpkg::ui {
namespace {

Gdiplus::RectF to_gdiplus(const RectF& rect) {
    return Gdiplus::RectF(rect.x, rect.y, rect.width, rect.height);
}

RectF intersect_rect(const RectF& left, const RectF& right) {
    const float x = std::max(left.x, right.x);
    const float y = std::max(left.y, right.y);
    const float r = std::min(left.right(), right.right());
    const float b = std::min(left.bottom(), right.bottom());
    if (r <= x || b <= y) {
        return {};
    }

    return {x, y, r - x, b - y};
}

float clamp01(float value) {
    return std::clamp(value, 0.0f, 1.0f);
}

BYTE to_byte(float value) {
    return static_cast<BYTE>(clamp01(value) * 255.0f + 0.5f);
}

void add_round_rect(Gdiplus::GraphicsPath& path, const RectF& rect, float radius) {
    const float limit = std::min(rect.width, rect.height) * 0.5f;
    const float r = std::clamp(radius, 0.0f, std::max(0.0f, limit));
    if (r < 0.5f) {
        path.AddRectangle(to_gdiplus(rect));
        return;
    }

    const Gdiplus::REAL d = r * 2.0f;
    const Gdiplus::REAL x = rect.x;
    const Gdiplus::REAL y = rect.y;
    const Gdiplus::REAL w = rect.width;
    const Gdiplus::REAL h = rect.height;

    path.AddArc(x, y, d, d, 180.0f, 90.0f);
    path.AddArc(x + w - d, y, d, d, 270.0f, 90.0f);
    path.AddArc(x + w - d, y + h - d, d, d, 0.0f, 90.0f);
    path.AddArc(x, y + h - d, d, d, 90.0f, 90.0f);
    path.CloseFigure();
}

std::wstring font_key(const TextStyle& style) {
    const int size_tenths = static_cast<int>(style.size * 10.0f + 0.5f);
    return style.family + L"|" + std::to_wstring(size_tenths) + L"|" + std::to_wstring(style.weight) +
           (style.italic ? L"|i" : L"|");
}

}  // namespace

// ============================================================================
// Color
// ============================================================================

Color Color::from_argb(uint32_t argb) {
    Color color;
    color.a = static_cast<float>((argb >> 24) & 0xFFu) / 255.0f;
    color.r = static_cast<float>((argb >> 16) & 0xFFu) / 255.0f;
    color.g = static_cast<float>((argb >> 8) & 0xFFu) / 255.0f;
    color.b = static_cast<float>(argb & 0xFFu) / 255.0f;
    return color;
}

Color Color::rgb(uint8_t r, uint8_t g, uint8_t b) {
    return rgba(r, g, b, 1.0f);
}

Color Color::rgba(uint8_t r, uint8_t g, uint8_t b, float alpha) {
    return {static_cast<float>(r) / 255.0f, static_cast<float>(g) / 255.0f,
            static_cast<float>(b) / 255.0f, clamp01(alpha)};
}

Color Color::from_colorref(COLORREF value) {
    return rgb(GetRValue(value), GetGValue(value), GetBValue(value));
}

Color Color::white() {
    return rgb(255, 255, 255);
}

Color Color::black() {
    return rgb(0, 0, 0);
}

Color Color::transparent() {
    return {0.0f, 0.0f, 0.0f, 0.0f};
}

Color Color::mix(const Color& other, float t) const {
    const float k = clamp01(t);
    return {r + (other.r - r) * k, g + (other.g - g) * k, b + (other.b - b) * k, a + (other.a - a) * k};
}

Gdiplus::Color Color::to_gdiplus() const {
    return Gdiplus::Color(to_byte(a), to_byte(r), to_byte(g), to_byte(b));
}

// ============================================================================
// Canvas
// ============================================================================

Canvas::Canvas(Gdiplus::Bitmap& bitmap, float dpi, int width_px, int height_px)
    : graphics_(std::make_unique<Gdiplus::Graphics>(&bitmap)),
      dpi_(dpi > 0.0f ? dpi : 96.0f),
      width_px_(width_px),
      height_px_(height_px) {
    graphics_->SetPageUnit(Gdiplus::UnitPixel);
    graphics_->SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    graphics_->SetPixelOffsetMode(Gdiplus::PixelOffsetModeNone);
    // 文字默认由 DirectWrite 灰度 AA 绘制（单一后端，位置一致）；这里的 GDI+
    // 文字只在 DWrite 初始化失败时兜底。
    graphics_->SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
    graphics_->SetCompositingMode(Gdiplus::CompositingModeSourceOver);
    graphics_->SetCompositingQuality(Gdiplus::CompositingQualityHighQuality);
    graphics_->SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBilinear);
    set_dpi(dpi_);
}

Canvas::~Canvas() = default;

void Canvas::set_dpi(float dpi) {
    dpi_ = dpi > 0.0f ? dpi : 96.0f;
    // 所有绘制都按 DIP 给坐标，这里一次性放大到像素。文字也走这条变换，
    // 所以字体按 DIP 尺寸创建即可，不需要两套坐标。
    graphics_->ResetTransform();
    const Gdiplus::REAL scale = dpi_ / 96.0f;
    graphics_->ScaleTransform(scale, scale, Gdiplus::MatrixOrderAppend);
}

Gdiplus::Graphics& Canvas::graphics() {
    return *graphics_;
}

Gdiplus::Font* Canvas::font_for(const TextStyle& style) {
    const std::wstring key = font_key(style);
    const auto found = fonts_.find(key);
    if (found != fonts_.end()) {
        return found->second.get();
    }

    const INT gdi_style = (style.weight >= 600 ? Gdiplus::FontStyleBold : Gdiplus::FontStyleRegular) |
                          (style.italic ? Gdiplus::FontStyleItalic : 0);
    auto font = std::make_unique<Gdiplus::Font>(style.family.c_str(), style.size, gdi_style, Gdiplus::UnitPixel);
    if (font->GetLastStatus() != Gdiplus::Ok) {
        font = std::make_unique<Gdiplus::Font>(L"Segoe UI", style.size, gdi_style, Gdiplus::UnitPixel);
    }

    Gdiplus::Font* raw = font.get();
    fonts_.emplace(key, std::move(font));
    return raw;
}

Gdiplus::StringFormat* Canvas::draw_format(const TextStyle& style) {
    const int alignment = static_cast<int>(style.align);
    const int valign = static_cast<int>(style.valign);
    const int key = 1000 + alignment * 100 + valign * 10 + (style.wrap ? 1 : 0) + (style.ellipsis ? 2 : 0);
    const auto found = formats_.find(key);
    if (found != formats_.end()) {
        return found->second.get();
    }

    auto format = std::make_unique<Gdiplus::StringFormat>(Gdiplus::StringFormatFlagsNoFitBlackBox);
    if (!style.wrap) {
        format->SetFormatFlags(format->GetFormatFlags() | Gdiplus::StringFormatFlagsNoWrap);
    }

    switch (style.align) {
        case TextAlign::Center: format->SetAlignment(Gdiplus::StringAlignmentCenter); break;
        case TextAlign::Right: format->SetAlignment(Gdiplus::StringAlignmentFar); break;
        case TextAlign::Left: format->SetAlignment(Gdiplus::StringAlignmentNear); break;
    }

    switch (style.valign) {
        case TextVAlign::Center: format->SetLineAlignment(Gdiplus::StringAlignmentCenter); break;
        case TextVAlign::Bottom: format->SetLineAlignment(Gdiplus::StringAlignmentFar); break;
        case TextVAlign::Top: format->SetLineAlignment(Gdiplus::StringAlignmentNear); break;
    }

    format->SetTrimming(style.ellipsis ? Gdiplus::StringTrimmingEllipsisCharacter : Gdiplus::StringTrimmingNone);
    // 字符串表里的 '&' 是普通字符：不把它当助记符。
    format->SetHotkeyPrefix(Gdiplus::HotkeyPrefixNone);

    Gdiplus::StringFormat* raw = format.get();
    formats_.emplace(key, std::move(format));
    return raw;
}

Gdiplus::StringFormat* Canvas::measure_format(bool wrap) {
    const int key = wrap ? -2 : -1;
    const auto found = formats_.find(key);
    if (found != formats_.end()) {
        return found->second.get();
    }

    auto format = std::make_unique<Gdiplus::StringFormat>(Gdiplus::StringFormatFlagsNoFitBlackBox);
    if (!wrap) {
        format->SetFormatFlags(format->GetFormatFlags() | Gdiplus::StringFormatFlagsNoWrap);
    }

    format->SetHotkeyPrefix(Gdiplus::HotkeyPrefixNone);
    Gdiplus::StringFormat* raw = format.get();
    formats_.emplace(key, std::move(format));
    return raw;
}

float Canvas::line_ratio(const TextStyle& style) {
    const std::wstring& key = style.family;
    static std::unordered_map<std::wstring, float> ratios;
    const auto cached = ratios.find(key);
    if (cached != ratios.end()) {
        return cached->second;
    }

    float ratio = 1.4f;
    Gdiplus::FontFamily family(style.family.c_str());
    if (family.GetLastStatus() == Gdiplus::Ok) {
        const INT em = family.GetEmHeight(Gdiplus::FontStyleRegular);
        // GetLineSpacing 已经含 ascent + descent + line gap，比只加 ascent/descent
        // 更接近 GDI+ 实际排版的基线间距。
        const INT spacing = family.GetLineSpacing(Gdiplus::FontStyleRegular);
        if (em > 0 && spacing > 0) {
            ratio = static_cast<float>(spacing) / static_cast<float>(em);
        }
    }

    ratios.emplace(key, ratio);
    return ratio;
}

void Canvas::fill_rect(const RectF& rect, const Color& color) {
    if (rect.empty() || color.a <= 0.0f) {
        return;
    }

    Gdiplus::SolidBrush brush(color.to_gdiplus());
    graphics_->FillRectangle(&brush, to_gdiplus(rect));
}

void Canvas::fill_round_rect(const RectF& rect, float radius, const Color& color) {
    if (rect.empty() || color.a <= 0.0f) {
        return;
    }

    Gdiplus::GraphicsPath path;
    add_round_rect(path, rect, radius);
    Gdiplus::SolidBrush brush(color.to_gdiplus());
    graphics_->FillPath(&brush, &path);
}

void Canvas::stroke_round_rect(const RectF& rect, float radius, const Color& color, float width) {
    if (rect.empty() || color.a <= 0.0f || width <= 0.0f) {
        return;
    }

    // 描边压在矩形内侧：否则圆角卡片会多出半像素、相邻卡片看起来错位。
    const RectF inner = rect.inset(width * 0.5f);
    if (inner.empty()) {
        return;
    }

    Gdiplus::GraphicsPath path;
    add_round_rect(path, inner, std::max(0.0f, radius - width * 0.5f));
    Gdiplus::Pen pen(color.to_gdiplus(), width);
    pen.SetAlignment(Gdiplus::PenAlignmentCenter);
    graphics_->DrawPath(&pen, &path);
}

void Canvas::draw_line(float x1, float y1, float x2, float y2, const Color& color, float width) {
    if (color.a <= 0.0f || width <= 0.0f) {
        return;
    }

    Gdiplus::Pen pen(color.to_gdiplus(), width);
    pen.SetStartCap(Gdiplus::LineCapRound);
    pen.SetEndCap(Gdiplus::LineCapRound);
    graphics_->DrawLine(&pen, x1, y1, x2, y2);
}

void Canvas::draw_text(std::wstring_view text, const RectF& rect, const TextStyle& style, const Color& color) {
    if (text.empty() || rect.empty() || color.a <= 0.0f) {
        return;
    }

    if (defer_text_) {
        TextCommand command;
        command.text.assign(text);
        command.rect = rect;
        command.style = style;
        command.color = color;
        command.clip = current_clip();
        text_commands_.push_back(std::move(command));
        return;
    }

    draw_text_immediate(text, rect, style, color);
}

void Canvas::draw_text_immediate(std::wstring_view text, const RectF& rect, const TextStyle& style,
                                 const Color& color, const RectF& clip) {
    if (text.empty() || rect.empty() || color.a <= 0.0f) {
        return;
    }

    Gdiplus::Font* font = font_for(style);
    Gdiplus::StringFormat* format = draw_format(style);
    Gdiplus::SolidBrush brush(color.to_gdiplus());

    if (clip.empty()) {
        graphics_->DrawString(text.data(), static_cast<INT>(text.size()), font, to_gdiplus(rect), format, &brush);
        return;
    }

    const Gdiplus::GraphicsState state = graphics_->Save();
    graphics_->SetClip(to_gdiplus(clip), Gdiplus::CombineModeIntersect);
    graphics_->DrawString(text.data(), static_cast<INT>(text.size()), font, to_gdiplus(rect), format, &brush);
    graphics_->Restore(state);
}

RectF Canvas::current_clip() const {
    if (!clip_rects_.empty()) {
        return clip_rects_.back();
    }

    const float scale = dpi_ / 96.0f;
    return {0.0f, 0.0f, static_cast<float>(width_px_) / scale, static_cast<float>(height_px_) / scale};
}

void Canvas::flush() {
    graphics_->Flush(Gdiplus::FlushIntentionSync);
}

void Canvas::draw_icon(IconId icon, const RectF& rect, const Color& color) {
    if (icon == IconId::None || rect.empty() || color.a <= 0.0f) {
        return;
    }

    const float s = std::min(rect.width, rect.height);
    const float ox = rect.x + (rect.width - s) * 0.5f;
    const float oy = rect.y + (rect.height - s) * 0.5f;
    const auto px = [&](float value) { return ox + value * s; };
    const auto py = [&](float value) { return oy + value * s; };

    Gdiplus::Pen pen(color.to_gdiplus(), std::max(1.2f, s * 0.12f));
    pen.SetStartCap(Gdiplus::LineCapRound);
    pen.SetEndCap(Gdiplus::LineCapRound);
    pen.SetLineJoin(Gdiplus::LineJoinRound);

    Gdiplus::SolidBrush brush(color.to_gdiplus());

    switch (icon) {
        case IconId::Check:
            graphics_->DrawLine(&pen, px(0.18f), py(0.53f), px(0.42f), py(0.77f));
            graphics_->DrawLine(&pen, px(0.42f), py(0.77f), px(0.82f), py(0.26f));
            break;

        case IconId::ChevronRight:
            graphics_->DrawLine(&pen, px(0.38f), py(0.22f), px(0.66f), py(0.5f));
            graphics_->DrawLine(&pen, px(0.66f), py(0.5f), px(0.38f), py(0.78f));
            break;

        case IconId::Close:
            graphics_->DrawLine(&pen, px(0.24f), py(0.24f), px(0.76f), py(0.76f));
            graphics_->DrawLine(&pen, px(0.76f), py(0.24f), px(0.24f), py(0.76f));
            break;

        case IconId::Info: {
            const float r = s * 0.4f;
            graphics_->DrawEllipse(&pen, ox + (s - r * 2.0f) * 0.5f, oy + (s - r * 2.0f) * 0.5f, r * 2.0f, r * 2.0f);
            graphics_->DrawLine(&pen, px(0.5f), py(0.44f), px(0.5f), py(0.68f));
            graphics_->FillEllipse(&brush, px(0.5f) - s * 0.04f, py(0.3f) - s * 0.04f, s * 0.08f, s * 0.08f);
            break;
        }

        case IconId::Warning: {
            Gdiplus::GraphicsPath path;
            path.AddLine(px(0.5f), py(0.1f), px(0.94f), py(0.86f));
            path.AddLine(px(0.94f), py(0.86f), px(0.06f), py(0.86f));
            path.CloseFigure();
            graphics_->DrawPath(&pen, &path);
            graphics_->DrawLine(&pen, px(0.5f), py(0.38f), px(0.5f), py(0.61f));
            graphics_->FillEllipse(&brush, px(0.5f) - s * 0.04f, py(0.7f) - s * 0.04f, s * 0.08f, s * 0.08f);
            break;
        }

        case IconId::Shield: {
            Gdiplus::GraphicsPath path;
            path.AddLine(px(0.5f), py(0.07f), px(0.87f), py(0.22f));
            path.AddLine(px(0.87f), py(0.22f), px(0.82f), py(0.6f));
            path.AddLine(px(0.82f), py(0.6f), px(0.5f), py(0.93f));
            path.AddLine(px(0.5f), py(0.93f), px(0.18f), py(0.6f));
            path.AddLine(px(0.18f), py(0.6f), px(0.13f), py(0.22f));
            path.CloseFigure();
            graphics_->DrawPath(&pen, &path);
            break;
        }

        case IconId::Download:
            graphics_->DrawLine(&pen, px(0.5f), py(0.14f), px(0.5f), py(0.64f));
            graphics_->DrawLine(&pen, px(0.3f), py(0.45f), px(0.5f), py(0.65f));
            graphics_->DrawLine(&pen, px(0.7f), py(0.45f), px(0.5f), py(0.65f));
            graphics_->DrawLine(&pen, px(0.2f), py(0.84f), px(0.8f), py(0.84f));
            break;

        case IconId::None:
            break;
    }
}

void Canvas::draw_image(const Gdiplus::Image& image, const RectF& rect) {
    if (rect.empty()) {
        return;
    }

    // GDI+ 的 DrawImage 只接收非 const Image*，但它不会修改图像；这里按 SDK 约定转换。
    graphics_->DrawImage(const_cast<Gdiplus::Image*>(&image), rect.x, rect.y, rect.width, rect.height);
}

void Canvas::push_clip(const RectF& rect) {
    const RectF clip = intersect_rect(current_clip(), rect);
    clip_rects_.push_back(clip);
    clips_.push_back(graphics_->Save());
    graphics_->SetClip(to_gdiplus(clip), Gdiplus::CombineModeIntersect);
}

void Canvas::pop_clip() {
    if (clips_.empty()) {
        return;
    }

    graphics_->Restore(clips_.back());
    clips_.pop_back();
    if (!clip_rects_.empty()) {
        clip_rects_.pop_back();
    }
}

void Canvas::reset_clip() {
    while (!clips_.empty()) {
        graphics_->Restore(clips_.back());
        clips_.pop_back();
    }

    clip_rects_.clear();
}

SizeF Canvas::measure_text(std::wstring_view text, float max_width, const TextStyle& style) {
    if (text.empty()) {
        return {0.0f, line_height(style)};
    }

    Gdiplus::Font* font = font_for(style);
    Gdiplus::StringFormat* format = measure_format(style.wrap);
    const float width = max_width > 0.0f ? max_width : 1000000.0f;
    Gdiplus::RectF layout(0.0f, 0.0f, width, 1000000.0f);
    Gdiplus::RectF bounds;
    graphics_->MeasureString(text.data(), static_cast<INT>(text.size()), font, layout, format, &bounds);
    return {bounds.Width, bounds.Height};
}

float Canvas::measure_text_width(std::wstring_view text, const TextStyle& style) {
    if (text.empty()) {
        return 0.0f;
    }

    if (metrics_ != nullptr) {
        return metrics_->measure_width(text, style, dpi_);
    }

    Gdiplus::Font* font = font_for(style);
    Gdiplus::StringFormat* format = measure_format(false);
    Gdiplus::RectF layout(0.0f, 0.0f, 1000000.0f, 1000000.0f);
    Gdiplus::RectF bounds;
    graphics_->MeasureString(text.data(), static_cast<INT>(text.size()), font, layout, format, &bounds);
    return bounds.Width;
}

float Canvas::line_height(const TextStyle& style) {
    if (metrics_ != nullptr) {
        return metrics_->line_height(style, dpi_);
    }

    return style.size * line_ratio(style);
}

// ============================================================================
// Frame
// ============================================================================

Frame::Frame(int width_px, int height_px, float dpi) {
    create(width_px, height_px, dpi);
}

Frame::~Frame() {
    destroy();
}

void Frame::create(int width_px, int height_px, float dpi) {
    width_px_ = std::max(1, width_px);
    height_px_ = std::max(1, height_px);
    dpi_ = dpi > 0.0f ? dpi : 96.0f;

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width_px_;
    info.bmiHeader.biHeight = -height_px_;  // 负高度 = 自上而下
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    dib_ = ::CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (dib_ == nullptr) {
        return;
    }

    bits_ = bits;
    memory_dc_ = ::CreateCompatibleDC(nullptr);
    if (memory_dc_ == nullptr) {
        destroy();
        return;
    }

    old_bitmap_ = ::SelectObject(memory_dc_, dib_);

    // GDI+ 直接画进 DIB 的像素：GDI+ 负责预乘 alpha，DWM 与
    // UpdateLayeredWindow 都按 AC_SRC_ALPHA 解释这份数据。
    bitmap_ = std::make_unique<Gdiplus::Bitmap>(width_px_, height_px_, width_px_ * 4, PixelFormat32bppPARGB,
                                                static_cast<BYTE*>(bits_));
    if (bitmap_->GetLastStatus() != Gdiplus::Ok) {
        destroy();
        return;
    }

    canvas_ = std::make_unique<Canvas>(*bitmap_, dpi_, width_px_, height_px_);
}

void Frame::destroy() {
    canvas_.reset();
    bitmap_.reset();

    if (memory_dc_ != nullptr) {
        if (old_bitmap_ != nullptr) {
            ::SelectObject(memory_dc_, old_bitmap_);
            old_bitmap_ = nullptr;
        }

        ::DeleteDC(memory_dc_);
        memory_dc_ = nullptr;
    }

    if (dib_ != nullptr) {
        ::DeleteObject(dib_);
        dib_ = nullptr;
        bits_ = nullptr;
    }
}

void Frame::resize(int width_px, int height_px, float dpi) {
    if (width_px == width_px_ && height_px == height_px_ && std::abs(dpi - dpi_) < 0.01f) {
        return;
    }

    destroy();
    create(width_px, height_px, dpi);
}

void Frame::clear_transparent() {
    if (bitmap_ == nullptr) {
        return;
    }

    Gdiplus::Graphics graphics(bitmap_.get());
    graphics.SetCompositingMode(Gdiplus::CompositingModeSourceCopy);
    graphics.SetCompositingQuality(Gdiplus::CompositingQualityHighSpeed);
    Gdiplus::SolidBrush transparent(Gdiplus::Color(0, 0, 0, 0));
    graphics.FillRectangle(&transparent, 0, 0, width_px_, height_px_);
}

}  // namespace msixpkg::ui
