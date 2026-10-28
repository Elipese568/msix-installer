#pragma once

// 主题 token（docs/self-drawn-ui-framework.md §5.5）。
//
// Theme 只放颜色/字号/圆角，不放控件逻辑。Widget 从 Host::theme() 拿当前主题；
// 材质是否可见（Mica/Acrylic vs Solid）会改变卡片的透明度 —— 这也是这里
// 唯一一处「主题依赖材质」的地方：Solid 下卡片必须不透明，否则会发灰。

#include "canvas.h"

namespace msixpkg::ui {

enum class Appearance {
    Auto = 0,
    Light,
    Dark,
    HighContrast,
};

struct Theme {
    Appearance appearance = Appearance::Light;
    bool material_visible = false;

    Color text;
    Color text_secondary;
    Color text_disabled;
    Color accent;
    Color on_accent;
    Color card;
    Color card_border;
    Color control;
    Color control_border;
    Color control_hover;
    Color control_pressed;
    Color window_solid;
    Color focus_ring;
    Color selection;
    Color table_header;
    Color table_hover;
    Color progress_track;
    Color progress_fill;
    Color scrollbar_track;
    Color scrollbar_thumb;
    Color warning;
    Color warning_surface;

    float radius_card = 12.0f;
    float radius_control = 6.0f;

    TextStyle title;
    TextStyle heading;
    TextStyle body;
    TextStyle caption;
    TextStyle button;
    TextStyle mono;
};

/// appearance 为 Auto 时按系统设置解析。material_visible=false 时返回全不透明配色。
Theme make_theme(Appearance appearance, bool material_visible);

/// 读 AppsUseLightTheme / 高对比度，返回 Light/Dark/HighContrast（不会返回 Auto）。
Appearance detect_appearance();

bool high_contrast_enabled();
bool transparency_enabled();
bool remote_session();

/// 系统强调色（注册表 AccentColor，失败回退 #0078D4）。
Color system_accent();

}  // namespace msixpkg::ui
