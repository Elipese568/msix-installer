#pragma once

// 单 HWND 的宿主（docs/self-drawn-ui-framework.md §2.3 / §3）。
//
// Host 拥有窗口、帧、材质、呈现器、焦点和 MSAA 根对象。它只处理窗口级消息：
// WM_PAINT、鼠标/键盘、WM_DPICHANGED、WM_GETOBJECT、WM_TIMER。业务逻辑
// （按钮点了做什么）通过 Widget 的回调交给调用方，Host 一概不知道。

#include "automation.h"
#include "canvas.h"
#include "gdi_text.h"
#include "material.h"
#include "theme.h"
#include "widget.h"

#include <Windows.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace msixpkg::ui {

class Presenter;

struct HostOptions {
    std::wstring title = L"Msixpkg";
    std::wstring window_class = L"MsixpkgSelfDrawnHost";
    int width_dip = 720;
    int height_dip = 480;
    Appearance appearance = Appearance::Auto;
    MaterialPreference material = MaterialPreference::Auto;
    /// Esc 的默认语义。为空时 Host 直接关窗。
    std::function<void()> on_escape;
};

class Host {
public:
    Host();
    ~Host();

    Host(const Host&) = delete;
    Host& operator=(const Host&) = delete;

    bool create(HINSTANCE instance, std::unique_ptr<Widget> root, HostOptions options = {});
    int run();
    void quit(int exit_code = 0);
    void close();

    HWND hwnd() const { return hwnd_; }
    Widget* root() const { return root_.get(); }

    const Theme& theme() const { return theme_; }
    Appearance appearance() const { return appearance_; }
    void set_appearance(Appearance appearance);

    MaterialKind material_kind() const { return material_kind_; }
    MaterialPreference material_preference() const { return options_.material; }
    void set_material(MaterialPreference preference);
    const wchar_t* presenter_name() const;

    float dpi() const { return dpi_; }
    float pixels(float dip) const { return dip * dpi_ / 96.0f; }
    float dip(float px) const { return px * 96.0f / dpi_; }
    int client_width_px() const { return client_width_px_; }
    int client_height_px() const { return client_height_px_; }

    void invalidate(const RectF* dirty = nullptr);
    void set_tick_handler(std::function<void()> handler);
    void set_animating(bool animating, int interval_ms = 33);
    void layout_changed();

    Widget* focused_widget() const { return focused_; }
    void focus_widget(Widget* widget);
    bool focus_next(bool backwards);

    // ---- MSAA（automation.cpp 用） ----
    struct AccessibleEntry {
        Widget* widget = nullptr;
        int row = -1;  // -1 = widget 本身；>=0 = Table 的第 row 行
    };
    std::vector<AccessibleEntry> accessible_entries() const;
    int child_id_of(const Widget* widget) const;
    void notify_accessible(const Widget* widget, LONG event_id);
    void notify_list_selection(const Widget* table, int row);
    RectF to_screen(const RectF& client_dip) const;

    /// 诊断用：WM_GETOBJECT(OBJID_CLIENT) 被成功响应的次数。
    int accessibility_request_count() const { return accessibility_requests_; }

private:
    static LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
    LRESULT handle(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
    LRESULT on_create();
    LRESULT on_paint();
    LRESULT on_get_object(WPARAM wparam, LPARAM lparam);

    bool register_window_class();
    void refresh_material();
    void apply_window_mode(bool layered);
    void update_client_size();
    void ensure_frame();
    void render(HDC dc);
    void render_now();
    float scale() const { return dpi_ / 96.0f; }

    bool to_dip(WPARAM lparam, float& x, float& y) const;
    Widget* hit_test(float x, float y) const;
    void update_mouse(float x, float y);
    void set_hover(Widget* widget);
    void collect_focusable(Widget& widget, std::vector<Widget*>& out) const;
    Widget* default_button() const;
    void collect_default(Widget& widget, Widget*& found) const;
    bool handle_key(WPARAM vk, bool is_down);

    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    HostOptions options_{};
    bool appearance_auto_ = true;
    Appearance appearance_ = Appearance::Light;
    Theme theme_ = make_theme(Appearance::Light, false);
    MaterialKind material_kind_ = MaterialKind::Solid;
    std::unique_ptr<Material> material_;
    std::unique_ptr<Presenter> presenter_;
    std::unique_ptr<Widget> root_;

    std::unique_ptr<Frame> frame_;
    int client_width_px_ = 0;
    int client_height_px_ = 0;
    float dpi_ = 96.0f;
    bool window_active_ = false;
    bool layered_ = false;
    bool refreshing_material_ = false;
    bool ole_initialized_ = false;
    bool animating_ = false;
    ULONGLONG last_tick_ms_ = 0;

    Widget* hovered_ = nullptr;
    Widget* captured_ = nullptr;
    Widget* focused_ = nullptr;

    std::function<void()> tick_handler_;
    AccessiblePtr accessibility_;
    GdiTextRenderer gdi_text_;
    ULONG_PTR gdiplus_token_ = 0;
    bool gdiplus_ready_ = false;
    int exit_code_ = 0;
    int accessibility_requests_ = 0;
};

}  // namespace msixpkg::ui
