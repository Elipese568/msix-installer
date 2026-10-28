#include "widget.h"

#include "host.h"
#include "theme.h"

namespace msixpkg::ui {

uint32_t Widget::accessible_state() const {
    uint32_t state = 0;
    if (tab_stop) {
        state |= kStateFocusable;
    }

    if (focused) {
        state |= kStateFocused;
    }

    if (!enabled) {
        state |= kStateDisabled;
    }

    if (!visible) {
        state |= kStateInvisible;
    }

    return state;
}

Widget* Widget::hit_test(float x, float y) {
    if (!visible || !enabled) {
        return nullptr;
    }

    return bounds.contains(x, y) ? this : nullptr;
}

void Widget::invalidate() {
    if (host != nullptr) {
        host->invalidate(&bounds);
    }
}

void Widget::invalidate_with(const RectF& rect) {
    if (host != nullptr) {
        host->invalidate(&rect);
    }
}

void Widget::focus() {
    if (host != nullptr) {
        host->focus_widget(this);
    }
}

const Theme& Widget::theme() const {
    if (host != nullptr) {
        return host->theme();
    }

    static const Theme kFallback = make_theme(Appearance::Light, false);
    return kFallback;
}

void Widget::paint_focus_ring(Canvas& canvas, float radius) const {
    if (!focused || !focus_visible || !enabled) {
        return;
    }

    // 1px 实线 + 2px 内缩：只靠颜色区分焦点在浅色/深色/HC 下都不够稳。
    const Theme& current = theme();
    canvas.stroke_round_rect(bounds.inset(1.5f), radius, current.focus_ring, 1.0f);
}

// ============================================================================
// Panel
// ============================================================================

Widget* Panel::add(std::unique_ptr<Widget> child) {
    if (child == nullptr) {
        return nullptr;
    }

    Widget* raw = child.get();
    raw->parent = this;
    if (host != nullptr) {
        attach_host(*raw, host);
    }

    children_.push_back(std::move(child));
    return raw;
}

void Panel::paint(Canvas& canvas) {
    if (!visible) {
        return;
    }

    canvas.push_clip(bounds);
    for (auto& child : children_) {
        if (child->visible) {
            child->paint(canvas);
        }
    }

    canvas.pop_clip();
}

Widget* Panel::hit_test(float x, float y) {
    if (!visible) {
        return nullptr;
    }

    // 后添加的在上层：命中测试要反着找。
    for (auto it = children_.rbegin(); it != children_.rend(); ++it) {
        if (!(*it)->visible) {
            continue;
        }

        if (Widget* hit = (*it)->hit_test(x, y)) {
            return hit;
        }
    }

    return (enabled && bounds.contains(x, y)) ? this : nullptr;
}

void Panel::tick(float dt_seconds) {
    if (!visible) {
        return;
    }

    for (auto& child : children_) {
        child->tick(dt_seconds);
    }
}

void Panel::on_theme_changed() {
    for (auto& child : children_) {
        child->on_theme_changed();
    }
}

void attach_host(Widget& root, Host* host) {
    root.host = host;
    if (const auto* children = root.child_widgets()) {
        for (const auto& child : *children) {
            attach_host(*child, host);
        }
    }
}

}  // namespace msixpkg::ui
