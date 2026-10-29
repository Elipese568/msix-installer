#include "gui_widgets.h"

#include "host.h"
#include "theme.h"

#include <algorithm>
#include <cmath>

namespace msixpkg::gui {
namespace {

wren::Color blend(const wren::Color& base, const wren::Color& tint, float amount) {
    return base.mix(tint, amount);
}

wren::Color warning_border(const wren::Theme& theme) {
    return theme.appearance == wren::Appearance::HighContrast ? theme.warning
                                                              : theme.warning.with_alpha(0.42f);
}

}  // namespace

// ============================================================================
// Card
// ============================================================================

void Card::set_tone(Tone tone) {
    if (tone_ == tone) {
        return;
    }

    tone_ = tone;
    invalidate();
}

void Card::paint(wren::Canvas& canvas) {
    const wren::Theme& current = theme();
    wren::Color surface = current.card;
    wren::Color border = current.card_border;

    if (tone_ == Tone::Warning) {
        surface = blend(current.card, current.warning, 0.10f);
        border = warning_border(current);
    } else if (tone_ == Tone::Accent) {
        surface = blend(current.card, current.accent, 0.10f);
        border = current.accent.with_alpha(0.35f);
    }

    canvas.fill_round_rect(bounds, current.radius_card, surface);
    canvas.stroke_round_rect(bounds, current.radius_card, border, 1.0f);
    wren::Panel::paint(canvas);
}

// ============================================================================
// Divider
// ============================================================================

void Divider::paint(wren::Canvas& canvas) {
    canvas.fill_rect(bounds, theme().card_border);
}

// ============================================================================
// StepRail
// ============================================================================

void StepRail::set_labels(std::vector<std::wstring> labels) {
    labels_ = std::move(labels);
    invalidate();
}

void StepRail::set_active(int index) {
    if (active_ == index) {
        return;
    }

    active_ = index;
    invalidate();
}

void StepRail::paint(wren::Canvas& canvas) {
    if (labels_.empty() || bounds.width <= 0.0f) {
        return;
    }

    const wren::Theme& current = theme();
    const int count = static_cast<int>(labels_.size());
    const float segment = bounds.width / static_cast<float>(count);
    const float center_y = bounds.y + 8.0f;
    const float radius = 5.0f;

    const auto center_x = [&](int index) {
        return bounds.x + segment * (static_cast<float>(index) + 0.5f);
    };

    // 连接线先画，圆点会盖在上面。
    canvas.draw_line(center_x(0), center_y, center_x(count - 1), center_y, current.control_border, 1.0f);
    for (int index = 0; index < active_ && index + 1 < count; ++index) {
        canvas.draw_line(center_x(index), center_y, center_x(index + 1), center_y, current.accent, 2.0f);
    }

    for (int index = 0; index < count; ++index) {
        const float x = center_x(index);
        const bool done = index < active_;
        const bool active = index == active_;

        const wren::RectF dot{x - radius, center_y - radius, radius * 2.0f, radius * 2.0f};
        if (done) {
            canvas.fill_round_rect(dot, radius, current.accent);
            canvas.draw_icon(wren::IconId::Check, dot.inset(3.0f), current.on_accent);
        } else if (active) {
            const wren::RectF halo{x - radius - 3.0f, center_y - radius - 3.0f, (radius + 3.0f) * 2.0f,
                                   (radius + 3.0f) * 2.0f};
            canvas.stroke_round_rect(halo, radius + 3.0f, current.accent.with_alpha(0.40f), 1.0f);
            canvas.fill_round_rect(dot, radius, current.accent);
        } else {
            canvas.fill_round_rect(dot, radius, current.control);
            canvas.stroke_round_rect(dot, radius, current.control_border, 1.0f);
        }

        wren::TextStyle label = current.caption;
        label.size = 12.0f;
        label.align = wren::TextAlign::Center;
        label.valign = wren::TextVAlign::Top;
        label.ellipsis = true;

        const wren::Color color = active ? current.text : (done ? current.text_secondary : current.text_disabled);
        canvas.draw_text(labels_[static_cast<size_t>(index)],
                         {x - segment * 0.5f + 6.0f, bounds.y + 17.0f, std::max(0.0f, segment - 12.0f), 17.0f},
                         label, color);
    }
}

// ============================================================================
// PhaseTimeline
// ============================================================================

void PhaseTimeline::set_items(std::vector<std::wstring> items) {
    items_ = std::move(items);
    invalidate();
}

void PhaseTimeline::set_state(int active_index, bool failed) {
    if (active_ == active_index && failed_ == failed) {
        return;
    }

    active_ = active_index;
    failed_ = failed;
    invalidate();
    if (host != nullptr) {
        host->notify_accessible(this, EVENT_OBJECT_NAMECHANGE);
    }
}

void PhaseTimeline::set_state_labels(std::wstring done, std::wstring active, std::wstring pending,
                                     std::wstring failed) {
    done_ = std::move(done);
    active_text_ = std::move(active);
    pending_ = std::move(pending);
    failed_text_ = std::move(failed);
    invalidate();
}

void PhaseTimeline::paint(wren::Canvas& canvas) {
    if (items_.empty()) {
        return;
    }

    const wren::Theme& current = theme();
    const int count = static_cast<int>(items_.size());
    const int rows = (count + 1) / 2;
    const float gap = 18.0f;
    const float column = std::max(40.0f, (bounds.width - gap) * 0.5f);
    const float row_height = std::min(34.0f, bounds.height / static_cast<float>(std::max(1, rows)));
    constexpr float kMark = 16.0f;

    // 每一列后面一条竖线，让「进度感」不依赖颜色。
    for (int col = 0; col < 2; ++col) {
        const float x = bounds.x + static_cast<float>(col) * (column + gap) + kMark * 0.5f;
        if (x <= bounds.right()) {
            canvas.draw_line(x, bounds.y + 8.0f, x, bounds.y + row_height * static_cast<float>(rows - 1) + 8.0f,
                             current.control_border, 1.0f);
        }
    }

    for (int index = 0; index < count; ++index) {
        const int col = index / rows;
        const int row = index % rows;
        const float x = bounds.x + static_cast<float>(col) * (column + gap);
        const float y = bounds.y + static_cast<float>(row) * row_height;
        const bool done = index < active_ || (failed_ && index < active_);
        const bool active = index == active_ && !failed_;
        const bool failed = index == active_ && failed_;

        const wren::RectF mark{x, y + (row_height - kMark) * 0.5f, kMark, kMark};
        if (failed) {
            canvas.fill_round_rect(mark, kMark * 0.5f, current.warning);
            canvas.draw_icon(wren::IconId::Close, mark.inset(3.0f), current.on_accent);
        } else if (active) {
            canvas.fill_round_rect(mark, kMark * 0.5f, current.accent);
            canvas.draw_icon(wren::IconId::ChevronRight, mark.inset(4.0f), current.on_accent);
        } else if (done) {
            canvas.fill_round_rect(mark, kMark * 0.5f, current.accent);
            canvas.draw_icon(wren::IconId::Check, mark.inset(3.0f), current.on_accent);
        } else {
            canvas.fill_round_rect(mark, kMark * 0.5f, current.control);
            canvas.stroke_round_rect(mark, kMark * 0.5f, current.control_border, 1.0f);
        }

        wren::TextStyle label = current.body;
        label.size = 13.0f;
        label.valign = wren::TextVAlign::Center;
        label.ellipsis = true;
        if (active) {
            label.weight = 600;
        }

        const wren::Color color = failed ? current.warning
                                        : (active ? current.accent : (done ? current.text_secondary : current.text_disabled));
        canvas.draw_text(items_[static_cast<size_t>(index)],
                         {x + kMark + 10.0f, y, std::max(0.0f, column - kMark - 10.0f), row_height}, label, color);
    }
}

std::wstring PhaseTimeline::state_text(int index) const {
    if (index < 0) {
        return pending_;
    }

    if (failed_ && index == active_) {
        return failed_text_;
    }

    if (index == active_) {
        return active_text_;
    }

    return index < active_ ? done_ : pending_;
}

std::wstring PhaseTimeline::accessible_name() const {
    std::wstring summary;
    for (size_t index = 0; index < items_.size(); ++index) {
        if (index != 0) {
            summary += L"；";
        }

        summary += state_text(static_cast<int>(index));
        summary += L" ";
        summary += items_[index];
    }

    return summary;
}

// ============================================================================
// StatusMark
// ============================================================================

void StatusMark::set_kind(Kind kind) {
    if (kind_ == kind) {
        return;
    }

    kind_ = kind;
    invalidate();
}

void StatusMark::paint(wren::Canvas& canvas) {
    const wren::Theme& current = theme();
    const float size = std::min(bounds.width, bounds.height);
    if (size <= 0.0f) {
        return;
    }

    const wren::RectF circle{bounds.x + (bounds.width - size) * 0.5f, bounds.y + (bounds.height - size) * 0.5f, size,
                             size};

    wren::Color fill = current.control;
    wren::Color border = current.control_border;
    wren::IconId icon = wren::IconId::Info;
    wren::Color icon_color = current.text_secondary;

    switch (kind_) {
        case Kind::Success:
            fill = current.accent;
            border = current.accent.with_alpha(0.35f);
            icon = wren::IconId::Check;
            icon_color = current.on_accent;
            break;
        case Kind::Failure:
            fill = blend(current.control, current.warning, 0.18f);
            border = warning_border(current);
            icon = wren::IconId::Close;
            icon_color = current.warning;
            break;
        case Kind::Info:
            break;
    }

    canvas.fill_round_rect(circle, size * 0.5f, fill);
    canvas.stroke_round_rect(circle, size * 0.5f, border, 1.0f);
    canvas.draw_icon(icon, circle.inset(size * 0.28f), icon_color);
}

// ============================================================================
// PackageTile
// ============================================================================

void PackageTile::paint(wren::Canvas& canvas) {
    const wren::Theme& current = theme();
    canvas.fill_round_rect(bounds, current.radius_card, blend(current.card, current.accent, 0.16f));
    canvas.stroke_round_rect(bounds, current.radius_card, current.accent.with_alpha(0.30f), 1.0f);

    wren::RectF box = bounds.inset(std::min(bounds.width, bounds.height) * 0.26f);
    canvas.stroke_round_rect(box, 2.0f, current.accent, 1.5f);
    const float flap = box.y + box.height * 0.36f;
    canvas.draw_line(box.x, flap, box.right(), flap, current.accent, 1.5f);
    canvas.draw_line(box.x + box.width * 0.5f, flap, box.x + box.width * 0.5f, box.bottom(), current.accent, 1.5f);
}

// ============================================================================
// Banner
// ============================================================================

void Banner::set_kind(Kind kind) {
    if (kind_ == kind) {
        return;
    }

    kind_ = kind;
    invalidate();
}

void Banner::set_message(std::wstring message) {
    if (text == message) {
        return;
    }

    text = std::move(message);
    invalidate();
    if (host != nullptr) {
        host->notify_accessible(this, EVENT_OBJECT_NAMECHANGE);
    }
}

void Banner::paint(wren::Canvas& canvas) {
    const wren::Theme& current = theme();
    const bool warning = kind_ == Kind::Warning;
    const wren::Color surface = warning ? blend(current.card, current.warning, 0.12f) : current.card;
    const wren::Color border = warning ? warning_border(current) : current.card_border;

    canvas.fill_round_rect(bounds, current.radius_card, surface);
    canvas.stroke_round_rect(bounds, current.radius_card, border, 1.0f);

    const wren::RectF icon{bounds.x + 14.0f, bounds.y + (bounds.height - 20.0f) * 0.5f, 20.0f, 20.0f};
    canvas.draw_icon(warning ? wren::IconId::Warning : wren::IconId::Info, icon,
                     warning ? current.warning : current.text_secondary);

    wren::TextStyle style = current.body;
    style.size = 13.0f;
    style.wrap = true;
    style.valign = wren::TextVAlign::Top;
    canvas.draw_text(text, {bounds.x + 46.0f, bounds.y + 8.0f, std::max(0.0f, bounds.width - 60.0f),
                            std::max(0.0f, bounds.height - 12.0f)},
                     style, current.text);
}

}  // namespace msixpkg::gui
