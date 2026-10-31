#include "setup_widgets.h"

#include "host.h"
#include "theme.h"

#include <algorithm>

namespace msixpkg::setup {

ChoiceCard::ChoiceCard(std::wstring title, std::wstring detail, std::function<void()> on_select)
    : detail_(std::move(detail)), on_select_(std::move(on_select)) {
    text = std::move(title);
    tab_stop = true;
}

void ChoiceCard::set_selected(bool selected) {
    if (selected_ == selected) {
        return;
    }
    selected_ = selected;
    invalidate();
    if (host != nullptr) {
        host->notify_accessible(this, EVENT_OBJECT_STATECHANGE);
    }
}

void ChoiceCard::select() {
    if (!enabled) {
        return;
    }
    set_selected(true);
    if (on_select_) {
        on_select_();
    }
}

void ChoiceCard::paint(wren::Canvas& canvas) {
    const wren::Theme& current = theme();
    const wren::RectF box = bounds;
    const wren::Color surface = current.card;
    const wren::Color border = selected_ ? current.accent : current.card_border;

    canvas.fill_round_rect(box, current.radius_card, surface);
    canvas.stroke_round_rect(box, current.radius_card, border, selected_ ? 2.0f : 1.0f);

    const float dot_diameter = 16.0f;
    const wren::RectF dot{box.x + 18.0f, box.y + (box.height - dot_diameter) * 0.5f, dot_diameter,
                          dot_diameter};
    if (selected_) {
        canvas.fill_round_rect(dot, dot_diameter * 0.5f, current.accent);
        const wren::RectF inner = dot.inset(5.0f);
        canvas.fill_round_rect(inner, inner.width * 0.5f, current.on_accent);
    } else {
        canvas.stroke_round_rect(dot, dot_diameter * 0.5f, current.control_border, 1.0f);
    }

    wren::TextStyle title_style = current.body;
    title_style.weight = 600;
    title_style.size = 14.0f;
    title_style.ellipsis = true;
    canvas.draw_text(text, {box.x + 46.0f, box.y + 12.0f, std::max(0.0f, box.width - 60.0f), 20.0f},
                     title_style, current.text);

    wren::TextStyle detail_style = current.caption;
    detail_style.size = 12.0f;
    detail_style.wrap = true;
    canvas.draw_text(detail_, {box.x + 46.0f, box.y + 34.0f, std::max(0.0f, box.width - 60.0f),
                               std::max(0.0f, box.height - 38.0f)},
                     detail_style, current.text_secondary);

    if (focused && focus_visible && enabled) {
        paint_focus_ring(canvas, current.radius_card);
    }
}

bool ChoiceCard::on_mouse(const wren::MouseEvent& event) {
    if (!enabled) {
        return false;
    }
    if (event.type == wren::MouseEvent::Type::Down) {
        select();
        return true;
    }
    if (event.type == wren::MouseEvent::Type::Up) {
        return true;
    }
    return false;
}

bool ChoiceCard::on_key(const wren::KeyEvent& event) {
    if (!enabled || !event.is_down) {
        return false;
    }
    if (event.vk == VK_SPACE || event.vk == VK_RETURN) {
        select();
        return true;
    }
    return false;
}

uint32_t ChoiceCard::accessible_state() const {
    uint32_t state = wren::Widget::accessible_state();
    if (selected_) {
        state |= wren::kStateChecked;
    }
    return state;
}

}  // namespace msixpkg::setup
