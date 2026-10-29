#include "dwrite_text.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace msixpkg::ui {
namespace {

DWRITE_TEXT_ALIGNMENT to_alignment(TextAlign align) {
    switch (align) {
        case TextAlign::Center: return DWRITE_TEXT_ALIGNMENT_CENTER;
        case TextAlign::Right: return DWRITE_TEXT_ALIGNMENT_TRAILING;
        case TextAlign::Left: return DWRITE_TEXT_ALIGNMENT_LEADING;
    }

    return DWRITE_TEXT_ALIGNMENT_LEADING;
}

DWRITE_PARAGRAPH_ALIGNMENT to_paragraph(TextVAlign valign) {
    switch (valign) {
        case TextVAlign::Center: return DWRITE_PARAGRAPH_ALIGNMENT_CENTER;
        case TextVAlign::Bottom: return DWRITE_PARAGRAPH_ALIGNMENT_FAR;
        case TextVAlign::Top: return DWRITE_PARAGRAPH_ALIGNMENT_NEAR;
    }

    return DWRITE_PARAGRAPH_ALIGNMENT_NEAR;
}

D2D1_COLOR_F to_color(const Color& color) {
    return D2D1::ColorF(color.r, color.g, color.b, color.a);
}

D2D1_RECT_F to_rect(const RectF& rect) {
    return D2D1::RectF(rect.x, rect.y, rect.right(), rect.bottom());
}

}  // namespace

DirectWriteText::DirectWriteText() = default;

DirectWriteText::~DirectWriteText() = default;

bool DirectWriteText::initialize() {
    if (d2d_factory_ == nullptr) {
        if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                                     d2d_factory_.ReleaseAndGetAddressOf()))) {
            return false;
        }
    }

    if (dwrite_factory_ == nullptr) {
        Microsoft::WRL::ComPtr<IUnknown> unknown;
        if (FAILED(::DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                         unknown.ReleaseAndGetAddressOf()))) {
            return false;
        }

        if (FAILED(unknown.As(&dwrite_factory_))) {
            return false;
        }
    }

    return d2d_factory_ != nullptr && dwrite_factory_ != nullptr;
}

bool DirectWriteText::bind(HDC memory_dc, int width_px, int height_px) {
    if (memory_dc != nullptr && memory_dc == bound_dc_ && width_px == width_px_ &&
        height_px == height_px_ && target_ != nullptr) {
        return true;
    }

    reset();
    if (!initialize() || memory_dc == nullptr || width_px <= 0 || height_px <= 0) {
        return false;
    }

    const D2D1_RENDER_TARGET_PROPERTIES properties = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));

    if (FAILED(d2d_factory_->CreateDCRenderTarget(&properties, target_.ReleaseAndGetAddressOf()))) {
        reset();
        return false;
    }

    RECT rect{0, 0, width_px, height_px};
    if (FAILED(target_->BindDC(memory_dc, &rect))) {
        reset();
        return false;
    }

    if (FAILED(target_->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 1.0f),
                                         brush_.ReleaseAndGetAddressOf()))) {
        reset();
        return false;
    }

    target_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    target_->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

    bound_dc_ = memory_dc;
    width_px_ = width_px;
    height_px_ = height_px;
    return true;
}

void DirectWriteText::reset() {
    target_.Reset();
    brush_.Reset();
    bound_dc_ = nullptr;
    width_px_ = 0;
    height_px_ = 0;
    drawing_ = false;
}

std::wstring DirectWriteText::key_for(const TextStyle& style) {
    return style.family + L"|" + std::to_wstring(static_cast<int>(style.size * 10.0f + 0.5f)) + L"|" +
           std::to_wstring(style.weight) + (style.italic ? L"|i|" : L"|n|") +
           std::to_wstring(static_cast<int>(style.align)) + L"|" +
           std::to_wstring(static_cast<int>(style.valign)) + (style.wrap ? L"|w" : L"|nw") +
           (style.ellipsis ? L"|e" : L"|ne");
}

DirectWriteText::FormatEntry* DirectWriteText::entry_for(const TextStyle& style) {
    const std::wstring key = key_for(style);
    const auto found = formats_.find(key);
    if (found != formats_.end()) {
        return &found->second;
    }

    if (!initialize()) {
        return nullptr;
    }

    const DWRITE_FONT_WEIGHT weight =
        style.weight >= 600 ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL;
    const DWRITE_FONT_STYLE font_style = style.italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL;

    FormatEntry entry;
    HRESULT status = dwrite_factory_->CreateTextFormat(
        style.family.c_str(), nullptr, weight, font_style, DWRITE_FONT_STRETCH_NORMAL, style.size, L"",
        entry.format.ReleaseAndGetAddressOf());

    if (FAILED(status)) {
        status = dwrite_factory_->CreateTextFormat(L"Segoe UI", nullptr, weight, font_style,
                                                   DWRITE_FONT_STRETCH_NORMAL, style.size, L"",
                                                   entry.format.ReleaseAndGetAddressOf());
    }

    if (FAILED(status) || entry.format == nullptr) {
        return nullptr;
    }

    entry.format->SetTextAlignment(to_alignment(style.align));
    entry.format->SetParagraphAlignment(to_paragraph(style.valign));
    entry.format->SetWordWrapping(style.wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);

    if (style.ellipsis) {
        dwrite_factory_->CreateEllipsisTrimmingSign(entry.format.Get(), entry.ellipsis.ReleaseAndGetAddressOf());
    }

    const auto inserted = formats_.emplace(key, std::move(entry));
    return &inserted.first->second;
}

bool DirectWriteText::begin(float dpi) {
    if (target_ == nullptr || drawing_) {
        return false;
    }

    target_->BeginDraw();

    const float scale = dpi > 0.0f ? dpi / 96.0f : 1.0f;
    target_->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale));
    target_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    target_->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    drawing_ = true;
    return true;
}

void DirectWriteText::draw(const TextCommand& command) {
    if (!drawing_ || dwrite_factory_ == nullptr || target_ == nullptr || command.text.empty() ||
        command.rect.empty()) {
        return;
    }

    FormatEntry* entry = entry_for(command.style);
    if (entry == nullptr || entry->format == nullptr) {
        return;
    }

    Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
    if (FAILED(dwrite_factory_->CreateTextLayout(command.text.c_str(),
                                                 static_cast<UINT32>(command.text.size()), entry->format.Get(),
                                                 command.rect.width, command.rect.height,
                                                 layout.ReleaseAndGetAddressOf()))) {
        return;
    }

    if (command.style.ellipsis && entry->ellipsis != nullptr) {
        const DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        layout->SetTrimming(&trimming, entry->ellipsis.Get());
    }

    brush_->SetColor(to_color(command.color));

    const bool clipped = !command.clip.empty();
    if (clipped) {
        target_->PushAxisAlignedClip(to_rect(command.clip), D2D1_ANTIALIAS_MODE_ALIASED);
    }

    target_->DrawTextLayout(D2D1::Point2F(command.rect.x, command.rect.y), layout.Get(), brush_.Get(),
                            D2D1_DRAW_TEXT_OPTIONS_NONE);

    if (clipped) {
        target_->PopAxisAlignedClip();
    }
}

void DirectWriteText::end() {
    if (!drawing_ || target_ == nullptr) {
        drawing_ = false;
        return;
    }

    const HRESULT status = target_->EndDraw();
    drawing_ = false;
    if (status == D2DERR_RECREATE_TARGET) {
        reset();
    }
}

float DirectWriteText::measure_width(std::wstring_view text, const TextStyle& style, float dpi) {
    (void)dpi;

    if (text.empty() || !initialize()) {
        return 0.0f;
    }

    FormatEntry* entry = entry_for(style);
    if (entry == nullptr || entry->format == nullptr) {
        return 0.0f;
    }

    Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
    if (FAILED(dwrite_factory_->CreateTextLayout(text.data(), static_cast<UINT32>(text.size()),
                                                 entry->format.Get(), 100000.0f, 100000.0f,
                                                 layout.ReleaseAndGetAddressOf()))) {
        return 0.0f;
    }

    layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

    DWRITE_TEXT_METRICS metrics{};
    if (FAILED(layout->GetMetrics(&metrics))) {
        return 0.0f;
    }

    return metrics.widthIncludingTrailingWhitespace;
}

float DirectWriteText::line_height(const TextStyle& style, float dpi) {
    (void)dpi;

    TextStyle probe = style;
    probe.align = TextAlign::Left;
    probe.valign = TextVAlign::Top;
    probe.wrap = false;
    probe.ellipsis = false;

    const std::wstring key = key_for(probe);
    const auto found = line_heights_.find(key);
    if (found != line_heights_.end()) {
        return found->second;
    }

    float value = std::max(1.0f, style.size * 1.35f);
    if (initialize()) {
        if (FormatEntry* entry = entry_for(probe); entry != nullptr && entry->format != nullptr) {
            Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
            if (SUCCEEDED(dwrite_factory_->CreateTextLayout(L"Ag", 2, entry->format.Get(), 100000.0f,
                                                            100000.0f, layout.ReleaseAndGetAddressOf()))) {
                DWRITE_TEXT_METRICS metrics{};
                if (SUCCEEDED(layout->GetMetrics(&metrics)) && metrics.height > 0.0f) {
                    value = metrics.height;
                }
            }
        }
    }

    line_heights_.emplace(key, value);
    return value;
}

}  // namespace msixpkg::ui
