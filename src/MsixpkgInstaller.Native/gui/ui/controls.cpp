#include "controls.h"

#include "host.h"
#include "theme.h"

#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

namespace msixpkg::ui {
namespace {

constexpr float kPadding = 8.0f;
constexpr float kScrollbarWidth = 9.0f;
constexpr float kCellPadding = 8.0f;

Color tone_color(const Theme& theme, LabelTone tone) {
    switch (tone) {
        case LabelTone::Secondary: return theme.text_secondary;
        case LabelTone::Disabled: return theme.text_disabled;
        case LabelTone::Accent: return theme.accent;
        case LabelTone::Warning: return theme.warning;
        case LabelTone::Primary: return theme.text;
    }

    return theme.text;
}

void paint_field(Canvas& canvas, const RectF& bounds, const Theme& theme, float radius) {
    canvas.fill_round_rect(bounds, radius, theme.control);
    canvas.stroke_round_rect(bounds, radius, theme.control_border, 1.0f);
}

TextStyle without_wrap(TextStyle style) {
    style.wrap = false;
    style.ellipsis = false;
    return style;
}

}  // namespace

// ============================================================================
// 滚动条几何（TextView / Table 共用）
// ============================================================================

ScrollBarMetrics scrollbar_metrics(const RectF& area, float content_height, float viewport_height, float scroll,
                                   float width) {
    ScrollBarMetrics metrics;
    if (area.height <= 0.0f || viewport_height <= 0.0f || content_height <= viewport_height + 0.5f) {
        return metrics;
    }

    metrics.visible = true;
    metrics.track = {area.right() - width, area.y, width, area.height};

    const float max_scroll = std::max(0.0f, content_height - viewport_height);
    const float thumb_height =
        std::min(area.height, std::max(24.0f, area.height * viewport_height / std::max(1.0f, content_height)));
    const float travel = std::max(0.0f, area.height - thumb_height);
    const float position = max_scroll > 0.0f ? std::clamp(scroll / max_scroll, 0.0f, 1.0f) : 0.0f;

    metrics.thumb = {metrics.track.x + 2.0f, area.y + position * travel, width - 4.0f, thumb_height};
    return metrics;
}

float scroll_for_thumb_y(const RectF& area, float content_height, float viewport_height, float thumb_y,
                         float thumb_height) {
    const float max_scroll = std::max(0.0f, content_height - viewport_height);
    const float travel = std::max(0.0f, area.height - thumb_height);
    if (travel <= 0.0f || max_scroll <= 0.0f) {
        return 0.0f;
    }

    const float position = std::clamp((thumb_y - area.y) / travel, 0.0f, 1.0f);
    return position * max_scroll;
}

// ============================================================================
// Label
// ============================================================================

Label::Label(std::wstring value, TextStyle style) : style_(std::move(style)) {
    text = std::move(value);
    tab_stop = false;
}

void Label::set_style(const TextStyle& style) {
    style_ = style;
    invalidate();
}

void Label::set_tone(LabelTone tone) {
    tone_ = tone;
    invalidate();
}

void Label::set_text(std::wstring value) {
    if (text == value) {
        return;
    }

    text = std::move(value);
    invalidate();
}

void Label::set_align(TextAlign align) {
    style_.align = align;
    invalidate();
}

void Label::set_valign(TextVAlign valign) {
    style_.valign = valign;
    invalidate();
}

void Label::set_ellipsis(bool ellipsis) {
    style_.ellipsis = ellipsis;
    invalidate();
}

void Label::paint(Canvas& canvas) {
    if (!visible) {
        return;
    }

    const Theme& current = theme();
    const Color color = enabled ? tone_color(current, tone_) : current.text_disabled;
    canvas.draw_text(text, bounds, style_, color);
}

Widget* Label::hit_test(float x, float y) {
    (void)x;
    (void)y;
    return nullptr;  // 静态文本不参与命中，点击落到下面的 Panel
}

// ============================================================================
// Button
// ============================================================================

Button::Button(std::wstring caption, std::function<void()> on_click) : on_click_(std::move(on_click)) {
    text = std::move(caption);
    tab_stop = true;
}

void Button::set_on_click(std::function<void()> callback) {
    on_click_ = std::move(callback);
}

void Button::set_primary(bool primary) {
    primary_ = primary;
    invalidate();
}

void Button::set_default(bool is_default) {
    default_ = is_default;
}

void Button::set_icon(IconId icon) {
    icon_ = icon;
    invalidate();
}

void Button::click() {
    if (!enabled) {
        return;
    }

    if (on_click_) {
        on_click_();
    }
}

void Button::paint(Canvas& canvas) {
    const Theme& current = theme();
    const bool active = pressed && hovered;

    Color background;
    Color foreground;
    if (!enabled) {
        background = current.control.mix(current.card, 0.45f);
        foreground = current.text_disabled;
    } else if (primary_) {
        background = current.accent;
        if (active) {
            background = background.mix(Color::black(), 0.12f);
        } else if (hovered) {
            background = background.mix(Color::white(), 0.10f);
        }

        foreground = current.on_accent;
    } else {
        background = active ? current.control_pressed : (hovered ? current.control_hover : current.control);
        foreground = current.text;
    }

    canvas.fill_round_rect(bounds, current.radius_control, background);
    if (!primary_) {
        canvas.stroke_round_rect(bounds, current.radius_control, current.control_border, 1.0f);
    } else {
        canvas.stroke_round_rect(bounds, current.radius_control, current.accent.mix(Color::white(), 0.20f), 1.0f);
    }

    RectF text_rect{bounds.x + 8.0f, bounds.y, std::max(0.0f, bounds.width - 16.0f), bounds.height};
    if (icon_ != IconId::None) {
        const RectF icon_rect{bounds.x + 10.0f, bounds.y + (bounds.height - 16.0f) * 0.5f, 16.0f, 16.0f};
        canvas.draw_icon(icon_, icon_rect, foreground);
        text_rect.x += 20.0f;
        text_rect.width = std::max(0.0f, text_rect.width - 20.0f);
    }

    TextStyle style = current.button;
    style.align = TextAlign::Center;
    style.valign = TextVAlign::Center;
    style.ellipsis = true;
    canvas.draw_text(text, text_rect, style, foreground);
    paint_focus_ring(canvas, current.radius_control);
}

bool Button::on_mouse(const MouseEvent& event) {
    switch (event.type) {
        case MouseEvent::Type::Down:
            if (!enabled || !event.left_button) {
                return false;
            }

            pressed = true;
            invalidate();
            return true;

        case MouseEvent::Type::Up: {
            const bool was_pressed = pressed;
            pressed = false;
            invalidate();
            if (was_pressed && enabled && bounds.contains(event.x, event.y)) {
                click();
            }

            return true;
        }

        default:
            return false;
    }
}

bool Button::on_key(const KeyEvent& event) {
    if (event.vk != VK_SPACE) {
        return false;
    }

    if (event.is_down) {
        if (enabled) {
            pressed = true;
            invalidate();
        }

        return true;
    }

    pressed = false;
    invalidate();
    if (enabled) {
        click();
    }

    return true;
}

// ============================================================================
// Checkbox
// ============================================================================

Checkbox::Checkbox(std::wstring label) {
    text = std::move(label);
    tab_stop = true;
}

void Checkbox::set_checked(bool checked) {
    if (checked_ == checked) {
        return;
    }

    checked_ = checked;
    invalidate();
    if (host != nullptr) {
        host->notify_accessible(this, EVENT_OBJECT_STATECHANGE);
    }
}

void Checkbox::set_on_changed(std::function<void(bool)> callback) {
    on_changed_ = std::move(callback);
}

void Checkbox::toggle() {
    if (!enabled) {
        return;
    }

    checked_ = !checked_;
    invalidate();
    if (host != nullptr) {
        host->notify_accessible(this, EVENT_OBJECT_STATECHANGE);
    }

    if (on_changed_) {
        on_changed_(checked_);
    }
}

void Checkbox::paint(Canvas& canvas) {
    const Theme& current = theme();
    constexpr float kBox = 18.0f;

    const RectF box_rect{bounds.x, bounds.y + (bounds.height - kBox) * 0.5f, kBox, kBox};
    if (checked_) {
        const Color fill = enabled ? current.accent : current.text_disabled;
        canvas.fill_round_rect(box_rect, 4.0f, fill);
        canvas.draw_icon(IconId::Check, box_rect.inset(3.0f), current.on_accent);
    } else {
        canvas.fill_round_rect(box_rect, 4.0f, hovered ? current.control_hover : current.control);
        canvas.stroke_round_rect(box_rect, 4.0f, current.control_border, 1.0f);
    }

    RectF text_rect{bounds.x + kBox + 10.0f, bounds.y, bounds.width - kBox - 10.0f, bounds.height};
    TextStyle style = current.body;
    style.valign = TextVAlign::Center;
    style.ellipsis = true;
    canvas.draw_text(text, text_rect, style, enabled ? current.text : current.text_disabled);
    paint_focus_ring(canvas, 4.0f);
}

bool Checkbox::on_mouse(const MouseEvent& event) {
    switch (event.type) {
        case MouseEvent::Type::Down:
            if (!enabled || !event.left_button) {
                return false;
            }

            pressed = true;
            invalidate();
            return true;

        case MouseEvent::Type::Up: {
            const bool was_pressed = pressed;
            pressed = false;
            invalidate();
            if (was_pressed && enabled && bounds.contains(event.x, event.y)) {
                toggle();
            }

            return true;
        }

        default:
            return false;
    }
}

bool Checkbox::on_key(const KeyEvent& event) {
    if (event.vk != VK_SPACE) {
        return false;
    }

    if (event.is_down) {
        if (enabled) {
            pressed = true;
            invalidate();
        }

        return true;
    }

    pressed = false;
    invalidate();
    toggle();
    return true;
}

uint32_t Checkbox::accessible_state() const {
    uint32_t state = Widget::accessible_state();
    if (checked_) {
        state |= kStateChecked;
    }

    return state;
}

// ============================================================================
// ProgressBar
// ============================================================================

ProgressBar::ProgressBar() {
    // 与现有 ControlSpec 的 WS_TABSTOP 行为一致：进度条也进焦点链，UIA 暴露百分比。
    tab_stop = true;
}

void ProgressBar::set_value(int percent) {
    const int clamped = std::clamp(percent, 0, 100);
    if (clamped == value_) {
        return;
    }

    value_ = clamped;
    invalidate();
    if (host != nullptr) {
        host->notify_accessible(this, EVENT_OBJECT_VALUECHANGE);
    }
}

void ProgressBar::set_marquee(bool marquee) {
    marquee_ = marquee;
    phase_ = 0.0f;
    invalidate();
}

void ProgressBar::paint(Canvas& canvas) {
    const Theme& current = theme();
    const float radius = bounds.height * 0.5f;
    canvas.fill_round_rect(bounds, radius, current.progress_track);
    // 高对比度下 track 是系统窗口色，和卡片同色；一条边框保证它可见。
    canvas.stroke_round_rect(bounds, radius, current.control_border, 1.0f);

    if (marquee_) {
        const float segment = std::max(bounds.height, bounds.width * 0.30f);
        const float travel = bounds.width + segment;
        const float left = bounds.x - segment + std::fmod(phase_, 1.0f) * travel;
        const float start = std::max(bounds.x, left);
        const float end = std::min(bounds.right(), left + segment);
        if (end > start) {
            canvas.push_clip(bounds.inset(1.0f));
            canvas.fill_round_rect({start, bounds.y + 1.0f, end - start, bounds.height - 2.0f},
                                   std::max(1.0f, radius - 1.0f), current.progress_fill);
            canvas.pop_clip();
        }

        return;
    }

    const float width = bounds.width * static_cast<float>(value_) / 100.0f;
    if (value_ > 0 && width > 0.0f) {
        const float fill_width = std::min(bounds.width, std::max(bounds.height, width));
        canvas.fill_round_rect({bounds.x, bounds.y, fill_width, bounds.height}, radius, current.progress_fill);
    }
}

void ProgressBar::tick(float dt_seconds) {
    if (!marquee_) {
        return;
    }

    phase_ = std::fmod(phase_ + dt_seconds * 0.6f, 1.0f);
    invalidate();
}

std::wstring ProgressBar::accessible_value() const {
    return std::to_wstring(value_) + L"%";
}

// ============================================================================
// TextView
// ============================================================================

TextView::TextView(std::wstring value, TextStyle style) : style_(std::move(style)) {
    text = std::move(value);
    tab_stop = true;
}

void TextView::set_style(const TextStyle& style) {
    style_ = style;
    lines_valid_ = false;
    invalidate();
}

void TextView::set_text(std::wstring value) {
    if (text == value) {
        return;
    }

    text = std::move(value);
    lines_valid_ = false;
    select_all_ = false;
    scroll_ = 0.0f;
    invalidate();
    if (host != nullptr) {
        host->notify_accessible(this, EVENT_OBJECT_VALUECHANGE);
    }
}

void TextView::set_read_only(bool read_only) {
    read_only_ = read_only;
    invalidate();
}

void TextView::set_show_scrollbar(bool show) {
    show_scrollbar_ = show;
    invalidate();
}

uint32_t TextView::accessible_state() const {
    return Widget::accessible_state() | kStateReadOnly;
}

RectF TextView::content_rect() const {
    const float scrollbar = scrollbar_visible_ ? kScrollbarWidth : 0.0f;
    return {bounds.x + kPadding, bounds.y + kPadding, std::max(0.0f, bounds.width - kPadding * 2.0f - scrollbar),
            std::max(0.0f, bounds.height - kPadding * 2.0f)};
}

float TextView::max_scroll() const {
    return std::max(0.0f, content_height_ - std::max(0.0f, bounds.height - kPadding * 2.0f));
}

void TextView::relayout(Canvas& canvas) {
    // 行高至少给字号 1.35 倍：GDI+ 的 DrawString 在矩形高度小于字体行高时
    // 会整行裁掉（不是压扁），所以这里的余量是功能性需求，不是审美。
    line_height_ = std::max(canvas.line_height(style_), style_.size * 1.35f);
    line_height_ = std::max(line_height_, 1.0f);
    const float viewport = std::max(0.0f, bounds.height - kPadding * 2.0f);
    const float full_width = std::max(1.0f, bounds.width - kPadding * 2.0f);

    const auto measure = [&canvas, this](std::wstring_view value) {
        return canvas.measure_text_width(value, style_);
    };

    lines_ = wrap_text(text, full_width, measure);
    if (static_cast<float>(lines_.size()) * line_height_ > viewport && full_width > kScrollbarWidth + 16.0f) {
        lines_ = wrap_text(text, full_width - kScrollbarWidth, measure);
    }

    content_height_ = static_cast<float>(lines_.size()) * line_height_;
    scrollbar_visible_ = show_scrollbar_ && content_height_ > viewport + 0.5f;
    scroll_ = std::clamp(scroll_, 0.0f, max_scroll());
    layout_width_ = bounds.width;
    layout_height_ = bounds.height;
    lines_valid_ = true;
}

void TextView::paint(Canvas& canvas) {
    const Theme& current = theme();
    paint_field(canvas, bounds, current, current.radius_control);

    if (!lines_valid_ || layout_width_ != bounds.width || layout_height_ != bounds.height) {
        relayout(canvas);
    }

    const RectF content = content_rect();
    if (content.empty()) {
        return;
    }

    canvas.push_clip(content);
    if (select_all_) {
        canvas.fill_rect(content, current.selection);
    }

    const TextStyle line_style = without_wrap(style_);
    const int first = static_cast<int>(std::floor(scroll_ / line_height_));
    const int visible_lines = static_cast<int>(std::ceil(content.height / line_height_)) + 1;
    const int last = std::min(static_cast<int>(lines_.size()), first + visible_lines);

    for (int index = std::max(0, first); index < last; ++index) {
        const float y = content.y + static_cast<float>(index) * line_height_ - scroll_;
        // 只画完整落在可视区里的行：半截汉字比留一条空白更像 bug。
        if (y + line_height_ > content.bottom() + 0.5f) {
            break;
        }

        canvas.draw_text(lines_[static_cast<size_t>(index)].text, {content.x, y, content.width, line_height_},
                         line_style, current.text);
    }

    canvas.pop_clip();

    const ScrollBarMetrics metrics = scrollbar_metrics(bounds.inset(2.0f), content_height_,
                                                       bounds.height - kPadding * 2.0f, scroll_);
    if (metrics.visible) {
        canvas.fill_round_rect(metrics.thumb, metrics.thumb.width * 0.5f, current.scrollbar_thumb);
    }

    paint_focus_ring(canvas, current.radius_control);
}

void TextView::scroll_to(float value) {
    const float clamped = std::clamp(value, 0.0f, max_scroll());
    if (std::abs(clamped - scroll_) < 0.01f) {
        return;
    }

    scroll_ = clamped;
    invalidate();
}

void TextView::scroll_by(float delta) {
    scroll_to(scroll_ + delta);
}

bool TextView::on_mouse(const MouseEvent& event) {
    const RectF area = bounds.inset(2.0f);
    const float viewport = bounds.height - kPadding * 2.0f;

    switch (event.type) {
        case MouseEvent::Type::Wheel:
            if (event.wheel_delta == 0) {
                return false;
            }

            scroll_by(-static_cast<float>(event.wheel_delta) / 120.0f * line_height_ * 3.0f);
            return true;

        case MouseEvent::Type::Down: {
            if (!event.left_button) {
                return false;
            }

            const ScrollBarMetrics metrics = scrollbar_metrics(area, content_height_, viewport, scroll_);
            if (metrics.visible && metrics.thumb.contains(event.x, event.y)) {
                dragging_thumb_ = true;
                drag_grab_ = event.y - metrics.thumb.y;
                return true;
            }

            select_all_ = false;
            invalidate();
            return false;
        }

        case MouseEvent::Type::Move:
            if (!dragging_thumb_) {
                return false;
            }

            {
                const ScrollBarMetrics metrics = scrollbar_metrics(area, content_height_, viewport, scroll_);
                scroll_to(scroll_for_thumb_y(area, content_height_, viewport, event.y - drag_grab_,
                                             metrics.thumb.height));
            }
            return true;

        case MouseEvent::Type::Up:
            dragging_thumb_ = false;
            return false;

        default:
            return false;
    }
}

bool TextView::on_key(const KeyEvent& event) {
    if (!event.is_down) {
        return false;
    }

    if (event.ctrl && event.vk == 'A') {
        select_all_ = true;
        invalidate();
        return true;
    }

    if (event.ctrl && event.vk == 'C') {
        copy_all();
        return true;
    }

    const float page = std::max(line_height_, bounds.height - kPadding * 2.0f - line_height_);
    switch (event.vk) {
        case VK_UP:
            scroll_by(-line_height_);
            return true;
        case VK_DOWN:
            scroll_by(line_height_);
            return true;
        case VK_PRIOR:
            scroll_by(-page);
            return true;
        case VK_NEXT:
            scroll_by(page);
            return true;
        case VK_HOME:
            scroll_to(0.0f);
            return true;
        case VK_END:
            scroll_to(max_scroll());
            return true;
        default:
            return false;
    }
}

void TextView::copy_all() {
    if (host == nullptr || host->hwnd() == nullptr) {
        return;
    }

    std::wstring data;
    data.reserve(text.size() + 16);
    for (const wchar_t ch : text) {
        if (ch == L'\n') {
            data += L"\r\n";
        } else {
            data.push_back(ch);
        }
    }

    if (::OpenClipboard(host->hwnd()) == FALSE) {
        return;
    }

    ::EmptyClipboard();
    const size_t bytes = (data.size() + 1) * sizeof(wchar_t);
    if (HGLOBAL memory = ::GlobalAlloc(GMEM_MOVEABLE, bytes); memory != nullptr) {
        if (void* target = ::GlobalLock(memory); target != nullptr) {
            std::memcpy(target, data.c_str(), bytes);
            ::GlobalUnlock(memory);
            ::SetClipboardData(CF_UNICODETEXT, memory);
        } else {
            ::GlobalFree(memory);
        }
    }

    ::CloseClipboard();
}

// ============================================================================
// Table
// ============================================================================

Table::Table() {
    tab_stop = true;
}

void Table::set_columns(std::vector<Column> columns) {
    columns_ = std::move(columns);
    invalidate();
}

void Table::set_rows(std::vector<std::vector<std::wstring>> rows) {
    rows_ = std::move(rows);
    selected_row_ = -1;
    hover_row_ = -1;
    scroll_ = 0.0f;
    invalidate();
}

void Table::set_on_selection_changed(std::function<void(int)> callback) {
    on_selection_changed_ = std::move(callback);
}

void Table::set_selected_row(int index, bool notify) {
    const int count = static_cast<int>(rows_.size());
    const int clamped = (index < 0 || index >= count) ? -1 : index;
    if (clamped == selected_row_) {
        return;
    }

    selected_row_ = clamped;
    ensure_visible(selected_row_);
    invalidate();

    if (notify && host != nullptr) {
        host->notify_list_selection(this, selected_row_);
    }

    if (notify && on_selection_changed_) {
        on_selection_changed_(selected_row_);
    }
}

std::wstring Table::row_text(int index) const {
    if (index < 0 || index >= static_cast<int>(rows_.size())) {
        return {};
    }

    std::wstring joined;
    const auto& row = rows_[static_cast<size_t>(index)];
    for (size_t column = 0; column < row.size(); ++column) {
        if (column > 0) {
            joined += L", ";
        }

        joined += row[column];
    }

    return joined;
}

uint32_t Table::accessible_state() const {
    return Widget::accessible_state();
}

std::wstring Table::accessible_value() const {
    return row_text(selected_row_);
}

RectF Table::row_view_bounds() const {
    return {bounds.x + 1.0f, bounds.y + 1.0f + header_height(), std::max(0.0f, bounds.width - 2.0f),
            std::max(0.0f, bounds.height - 2.0f - header_height())};
}

RectF Table::row_bounds(int index) const {
    const RectF view = row_view_bounds();
    return {view.x, view.y + static_cast<float>(index) * row_height() - scroll_, view.width, row_height()};
}

int Table::row_at(float x, float y) const {
    const RectF view = row_view_bounds();
    if (x < view.x || x >= view.right() || y < view.y || y >= view.bottom()) {
        return -1;
    }

    const int index = static_cast<int>((y - view.y + scroll_) / row_height());
    if (index < 0 || index >= static_cast<int>(rows_.size())) {
        return -1;
    }

    return index;
}

float Table::max_scroll() const {
    return std::max(0.0f, static_cast<float>(rows_.size()) * row_height() - row_view_bounds().height);
}

void Table::scroll_by(float delta) {
    const float clamped = std::clamp(scroll_ + delta, 0.0f, max_scroll());
    if (std::abs(clamped - scroll_) < 0.01f) {
        return;
    }

    scroll_ = clamped;
    invalidate();
}

void Table::ensure_visible(int index) {
    if (index < 0) {
        return;
    }

    const RectF view = row_view_bounds();
    const RectF row = row_bounds(index);
    if (row.y < view.y) {
        scroll_ = std::max(0.0f, static_cast<float>(index) * row_height());
    } else if (row.bottom() > view.bottom()) {
        scroll_ = std::max(0.0f, static_cast<float>(index + 1) * row_height() - view.height);
    }
}

void Table::paint(Canvas& canvas) {
    const Theme& current = theme();
    paint_field(canvas, bounds, current, current.radius_control);

    const RectF content{bounds.x + 1.0f, bounds.y + 1.0f, std::max(0.0f, bounds.width - 2.0f),
                        std::max(0.0f, bounds.height - 2.0f)};

    // 表头
    if (content.width > 0.0f) {
        canvas.push_clip({content.x, content.y, content.width, header_height()});
        canvas.fill_rect({content.x, content.y, content.width, header_height()}, current.table_header);

        TextStyle header = current.caption;
        header.weight = 600;
        header.valign = TextVAlign::Center;
        header.ellipsis = true;

        float x = content.x;
        for (const Column& column : columns_) {
            if (x >= content.right()) {
                break;
            }

            const float width = std::min(column.width, content.right() - x);
            canvas.draw_text(column.title, {x + kCellPadding, content.y, std::max(0.0f, width - kCellPadding * 2.0f),
                                            header_height()},
                             header, current.text_secondary);
            x += column.width;
        }

        canvas.pop_clip();
    }

    const RectF view = row_view_bounds();
    const ScrollBarMetrics metrics =
        scrollbar_metrics(content, static_cast<float>(rows_.size()) * row_height(), view.height, scroll_);
    const float usable_width = std::max(0.0f, view.width - (metrics.visible ? kScrollbarWidth : 0.0f));

    if (!view.empty()) {
        canvas.push_clip(view);
        for (int index = 0; index < static_cast<int>(rows_.size()); ++index) {
            const RectF row = row_bounds(index);
            if (row.bottom() <= view.y || row.y >= view.bottom()) {
                continue;
            }

            if (index == selected_row_) {
                canvas.fill_rect(row, current.selection);
            } else if (index == hover_row_) {
                canvas.fill_rect(row, current.table_hover);
            }

            TextStyle cell = current.body;
            cell.size = 13.0f;
            cell.valign = TextVAlign::Center;
            cell.ellipsis = true;

            const auto& values = rows_[static_cast<size_t>(index)];
            float x = row.x;
            for (size_t column = 0; column < columns_.size() && column < values.size(); ++column) {
                const float width = columns_[column].width;
                if (x >= row.x + usable_width) {
                    break;
                }

                const float visible_width = std::min(width, row.x + usable_width - x);
                canvas.draw_text(values[column],
                                 {x + kCellPadding, row.y, std::max(0.0f, visible_width - kCellPadding * 2.0f),
                                  row.height},
                                 cell, current.text);
                x += width;
            }

            canvas.draw_line(row.x, row.bottom() - 0.5f, row.x + usable_width, row.bottom() - 0.5f,
                             current.card_border.with_alpha(0.5f), 1.0f);
        }

        canvas.pop_clip();
    }

    if (metrics.visible) {
        canvas.fill_round_rect(metrics.thumb, metrics.thumb.width * 0.5f, current.scrollbar_thumb);
    }

    paint_focus_ring(canvas, current.radius_control);
}

bool Table::on_mouse(const MouseEvent& event) {
    const RectF view = row_view_bounds();
    const ScrollBarMetrics metrics =
        scrollbar_metrics({bounds.x + 1.0f, bounds.y + 1.0f, bounds.width - 2.0f, bounds.height - 2.0f},
                          static_cast<float>(rows_.size()) * row_height(), view.height, scroll_);

    switch (event.type) {
        case MouseEvent::Type::Wheel:
            if (event.wheel_delta == 0) {
                return false;
            }

            scroll_by(-static_cast<float>(event.wheel_delta) / 120.0f * row_height() * 2.0f);
            return true;

        case MouseEvent::Type::Down: {
            if (!event.left_button) {
                return false;
            }

            if (metrics.visible && metrics.thumb.contains(event.x, event.y)) {
                dragging_thumb_ = true;
                drag_grab_ = event.y - metrics.thumb.y;
                return true;
            }

            const int row = row_at(event.x, event.y);
            if (row >= 0) {
                set_selected_row(row);
                return true;
            }

            return false;
        }

        case MouseEvent::Type::Move: {
            if (dragging_thumb_) {
                const float target = scroll_for_thumb_y({bounds.x + 1.0f, bounds.y + 1.0f, bounds.width - 2.0f,
                                                         bounds.height - 2.0f},
                                                        static_cast<float>(rows_.size()) * row_height(), view.height,
                                                        event.y - drag_grab_, metrics.thumb.height);
                const float clamped = std::clamp(target, 0.0f, max_scroll());
                if (std::abs(clamped - scroll_) >= 0.01f) {
                    scroll_ = clamped;
                    invalidate();
                }

                return true;
            }

            const int row = row_at(event.x, event.y);
            if (row != hover_row_) {
                hover_row_ = row;
                invalidate();
            }

            return false;
        }

        case MouseEvent::Type::Up:
            dragging_thumb_ = false;
            return false;

        case MouseEvent::Type::Leave:
            if (hover_row_ != -1) {
                hover_row_ = -1;
                invalidate();
            }

            return false;
    }

    return false;
}

bool Table::on_key(const KeyEvent& event) {
    if (!event.is_down) {
        return false;
    }

    const int count = static_cast<int>(rows_.size());
    if (count == 0) {
        return false;
    }

    const int current =
        selected_row_ < 0 ? (event.vk == VK_END ? count - 1 : 0) : selected_row_;
    const int page = std::max(1, static_cast<int>(row_view_bounds().height / row_height()));

    switch (event.vk) {
        case VK_UP:
            set_selected_row(std::max(0, current - 1));
            return true;
        case VK_DOWN:
            set_selected_row(std::min(count - 1, selected_row_ < 0 ? 0 : current + 1));
            return true;
        case VK_HOME:
            set_selected_row(0);
            return true;
        case VK_END:
            set_selected_row(count - 1);
            return true;
        case VK_PRIOR:
            set_selected_row(std::max(0, current - page));
            return true;
        case VK_NEXT:
            set_selected_row(std::min(count - 1, current + page));
            return true;
        default:
            return false;
    }
}

}  // namespace msixpkg::ui
