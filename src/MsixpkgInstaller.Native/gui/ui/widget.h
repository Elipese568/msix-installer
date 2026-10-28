#pragma once

// Widget 基类与面板（docs/self-drawn-ui-framework.md §2.3 / §5）。
//
// 树是**单 HWND** 的：没有子窗口，所以「布局」就是给每个 Widget 一个绝对
// 矩形；「焦点」就是 Host 维护的一个 Widget*。Panel 只做三件事：容纳子节点、
// 把各自矩形画出来、把命中测试分派给子节点。

#include "canvas.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace msixpkg::ui {

class Host;
struct Theme;

// 位掩码与 docs/self-drawn-ui-framework.md §2.3 一致，后两位是本实现补的。
inline constexpr uint32_t kStateFocusable = 1u << 0;
inline constexpr uint32_t kStateFocused = 1u << 1;
inline constexpr uint32_t kStateDisabled = 1u << 2;
inline constexpr uint32_t kStateChecked = 1u << 3;
inline constexpr uint32_t kStateSelected = 1u << 4;
inline constexpr uint32_t kStateReadOnly = 1u << 5;
inline constexpr uint32_t kStateInvisible = 1u << 6;
inline constexpr uint32_t kStateOffscreen = 1u << 7;

enum class Role {
    Panel,
    Label,
    Button,
    Checkbox,
    Progress,
    Text,
    Table,
    Row,
    ScrollBar,
};

struct MouseEvent {
    enum class Type { Move, Down, Up, Wheel, Leave };

    Type type = Type::Move;
    float x = 0.0f;  // 根坐标（DIP）
    float y = 0.0f;
    int wheel_delta = 0;
    bool left_button = false;
};

struct KeyEvent {
    UINT vk = 0;
    bool ctrl = false;
    bool shift = false;
    bool alt = false;
    bool is_down = true;
};

class Widget {
public:
    virtual ~Widget() = default;

    RectF bounds;
    int control_id = 0;

    Widget* parent = nullptr;
    Host* host = nullptr;

    bool visible = true;
    bool enabled = true;
    bool tab_stop = false;
    bool focused = false;
    bool focus_visible = true;
    bool hovered = false;
    bool pressed = false;

    std::wstring text;

    virtual void paint(Canvas& canvas) = 0;

    /// 返回命中的最深节点；不可见/禁用/越界返回 nullptr。Label 之类不可交互的
    /// 控件应该返回 nullptr，让点击落到下面的 Panel 上。
    virtual Widget* hit_test(float x, float y);

    virtual bool on_mouse(const MouseEvent& event) { (void)event; return false; }
    virtual bool on_key(const KeyEvent& event) { (void)event; return false; }
    virtual void tick(float dt_seconds) { (void)dt_seconds; }
    virtual void on_focus() {}
    virtual void on_blur() {}
    virtual void on_theme_changed() {}

    virtual std::wstring accessible_name() const { return text; }
    virtual Role accessible_role() const = 0;
    virtual std::wstring accessible_value() const { return {}; }
    virtual std::wstring accessible_default_action() const { return {}; }
    virtual void do_default_action() {}
    virtual uint32_t accessible_state() const;
    /// Table 会把每一行也暴露成 MSAA 子对象（docs §6.2）。
    virtual int accessible_row_count() const { return 0; }
    virtual bool is_default_button() const { return false; }

    virtual const std::vector<std::unique_ptr<Widget>>* child_widgets() const { return nullptr; }

    bool accepts_focus() const { return tab_stop && visible && enabled; }
    void invalidate();
    void invalidate_with(const RectF& rect);
    void focus();
    const Theme& theme() const;

protected:
    void paint_focus_ring(Canvas& canvas, float radius = 6.0f) const;
};

class Panel : public Widget {
public:
    Panel() = default;

    Widget* add(std::unique_ptr<Widget> child);
    template <typename T, typename... Args>
    T* emplace(Args&&... args) {
        auto child = std::make_unique<T>(std::forward<Args>(args)...);
        T* raw = child.get();
        add(std::move(child));
        return raw;
    }

    const std::vector<std::unique_ptr<Widget>>& children() const { return children_; }
    void paint(Canvas& canvas) override;
    Widget* hit_test(float x, float y) override;
    void tick(float dt_seconds) override;
    void on_theme_changed() override;
    Role accessible_role() const override { return Role::Panel; }
    const std::vector<std::unique_ptr<Widget>>* child_widgets() const override { return &children_; }

private:
    std::vector<std::unique_ptr<Widget>> children_;
};

/// 把 root 及其所有后代挂到 host 上（invalidate/focus/主题分发都靠这个指针）。
void attach_host(Widget& root, Host* host);

}  // namespace msixpkg::ui
