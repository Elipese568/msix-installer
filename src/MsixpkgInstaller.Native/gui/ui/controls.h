#pragma once

// 控件集（docs/self-drawn-ui-framework.md §5.2）。
//
// 全部是 Widget：没有子 HWND。Label/Button/Checkbox/ProgressBar 是简单叶子；
// TextView/Table 自己维护滚动模型，共用同一套自绘滚动条几何（ScrollBarMetrics）。

#include "text_layout.h"
#include "widget.h"

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace msixpkg::ui {

// ---- 自绘滚动条：TextView / Table 共用 ----
struct ScrollBarMetrics {
    RectF track;
    RectF thumb;
    bool visible = false;
};

ScrollBarMetrics scrollbar_metrics(const RectF& area, float content_height, float viewport_height, float scroll,
                                   float width = 9.0f);

/// 拖动时把鼠标位置换算回滚动偏移（用于 TextView / Table）。
float scroll_for_thumb_y(const RectF& area, float content_height, float viewport_height, float thumb_y,
                         float thumb_height);

enum class LabelTone { Primary, Secondary, Disabled, Accent, Warning };

class Label : public Widget {
public:
    explicit Label(std::wstring value = {}, TextStyle style = {});
    void set_style(const TextStyle& style);
    const TextStyle& style() const { return style_; }
    void set_tone(LabelTone tone);
    LabelTone tone() const { return tone_; }
    void set_text(std::wstring value);
    void set_align(TextAlign align);
    void set_valign(TextVAlign valign);
    void set_ellipsis(bool ellipsis);

    void paint(Canvas& canvas) override;
    Widget* hit_test(float x, float y) override;
    Role accessible_role() const override { return Role::Label; }

private:
    TextStyle style_;
    LabelTone tone_ = LabelTone::Primary;
};

class Button : public Widget {
public:
    explicit Button(std::wstring caption = {}, std::function<void()> on_click = {});
    void set_on_click(std::function<void()> callback);
    void set_primary(bool primary);
    bool is_primary() const { return primary_; }
    void set_default(bool is_default);
    bool is_default_button() const override { return default_; }
    void set_icon(IconId icon);
    void click();

    void paint(Canvas& canvas) override;
    bool on_mouse(const MouseEvent& event) override;
    bool on_key(const KeyEvent& event) override;
    Role accessible_role() const override { return Role::Button; }
    std::wstring accessible_default_action() const override { return L"按下"; }
    void do_default_action() override { click(); }

private:
    std::function<void()> on_click_;
    bool primary_ = false;
    bool default_ = false;
    IconId icon_ = IconId::None;
};

class Checkbox : public Widget {
public:
    explicit Checkbox(std::wstring label = {});
    void set_checked(bool checked);
    bool checked() const { return checked_; }
    void set_on_changed(std::function<void(bool)> callback);
    void toggle();

    void paint(Canvas& canvas) override;
    bool on_mouse(const MouseEvent& event) override;
    bool on_key(const KeyEvent& event) override;
    uint32_t accessible_state() const override;
    Role accessible_role() const override { return Role::Checkbox; }
    std::wstring accessible_default_action() const override { return L"切换"; }
    void do_default_action() override { toggle(); }

private:
    bool checked_ = false;
    std::function<void(bool)> on_changed_;
};

class ProgressBar : public Widget {
public:
    ProgressBar();

    void set_value(int percent);
    /// 亚百分比进度：高刷新率下进度条按像素平滑移动，无障碍仍按整数报告。
    void set_value_f(float percent);
    int value() const { return value_; }
    float value_f() const { return value_f_; }
    void set_marquee(bool marquee);
    bool marquee() const { return marquee_; }

    void paint(Canvas& canvas) override;
    void tick(float dt_seconds) override;
    Role accessible_role() const override { return Role::Progress; }
    std::wstring accessible_value() const override;

private:
    int value_ = 0;
    float value_f_ = 0.0f;
    bool marquee_ = false;
    float phase_ = 0.0f;
};

class TextView : public Widget {
public:
    explicit TextView(std::wstring value = {}, TextStyle style = {});
    void set_style(const TextStyle& style);
    void set_text(std::wstring value);
    void set_read_only(bool read_only);
    bool read_only() const { return read_only_; }
    void set_show_scrollbar(bool show);
    void copy_all();

    void paint(Canvas& canvas) override;
    bool on_mouse(const MouseEvent& event) override;
    bool on_key(const KeyEvent& event) override;
    uint32_t accessible_state() const override;
    Role accessible_role() const override { return Role::Text; }
    std::wstring accessible_value() const override { return text; }

private:
    void relayout(Canvas& canvas);
    void scroll_by(float delta);
    void scroll_to(float value);
    RectF content_rect() const;
    float max_scroll() const;

    TextStyle style_;
    bool read_only_ = true;
    bool show_scrollbar_ = true;
    bool scrollbar_visible_ = false;
    bool dragging_thumb_ = false;
    float drag_grab_ = 0.0f;
    float scroll_ = 0.0f;
    float line_height_ = 0.0f;
    float content_height_ = 0.0f;
    float layout_width_ = 0.0f;
    float layout_height_ = 0.0f;
    bool lines_valid_ = false;
    bool select_all_ = false;
    std::vector<WrappedLine> lines_;
};

class Table : public Widget {
public:
    struct Column {
        std::wstring title;
        float width = 100.0f;
    };

    Table();

    void set_columns(std::vector<Column> columns);
    void set_rows(std::vector<std::vector<std::wstring>> rows);
    const std::vector<std::vector<std::wstring>>& rows() const { return rows_; }
    int row_count() const { return static_cast<int>(rows_.size()); }

    int selected_row() const { return selected_row_; }
    void set_selected_row(int index, bool notify = true);
    void set_on_selection_changed(std::function<void(int)> callback);

    void paint(Canvas& canvas) override;
    bool on_mouse(const MouseEvent& event) override;
    bool on_key(const KeyEvent& event) override;
    uint32_t accessible_state() const override;
    std::wstring accessible_value() const override;
    Role accessible_role() const override { return Role::Table; }
    int accessible_row_count() const override { return static_cast<int>(rows_.size()); }

    std::wstring row_text(int index) const;
    RectF row_view_bounds() const;
    RectF row_bounds(int index) const;
    int row_at(float x, float y) const;
    float header_height() const { return 28.0f; }
    float row_height() const { return 26.0f; }

private:
    void ensure_visible(int index);
    void scroll_by(float delta);
    float max_scroll() const;

    std::vector<Column> columns_;
    std::vector<std::vector<std::wstring>> rows_;
    int selected_row_ = -1;
    int hover_row_ = -1;
    float scroll_ = 0.0f;
    bool dragging_thumb_ = false;
    float drag_grab_ = 0.0f;
    std::function<void(int)> on_selection_changed_;
};

}  // namespace msixpkg::ui
