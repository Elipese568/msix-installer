#include "theme.h"

#include <algorithm>

namespace msixpkg::ui {
namespace {

bool read_dword(HKEY root, const wchar_t* path, const wchar_t* name, DWORD& value) {
    DWORD size = sizeof(value);
    const LSTATUS status = ::RegGetValueW(root, path, name, RRF_RT_REG_DWORD, nullptr, &value, &size);
    return status == ERROR_SUCCESS;
}

float brightness(const Color& color) {
    return 0.299f * color.r + 0.587f * color.g + 0.114f * color.b;
}

Color system_color(int index) {
    return Color::from_colorref(::GetSysColor(index));
}

TextStyle make_style(float size, int weight, bool wrap = false, bool ellipsis = false) {
    TextStyle style;
    style.size = size;
    style.weight = weight;
    style.wrap = wrap;
    style.ellipsis = ellipsis;
    return style;
}

}  // namespace

bool high_contrast_enabled() {
    HIGHCONTRASTW contrast{};
    contrast.cbSize = sizeof(contrast);
    if (::SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0) == FALSE) {
        return false;
    }

    return (contrast.dwFlags & HCF_HIGHCONTRASTON) != 0;
}

bool transparency_enabled() {
    DWORD value = 1;
    read_dword(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
               L"EnableTransparency", value);
    return value != 0;
}

bool remote_session() {
    return ::GetSystemMetrics(SM_REMOTESESSION) != 0;
}

Appearance detect_appearance() {
    if (high_contrast_enabled()) {
        return Appearance::HighContrast;
    }

    DWORD light = 1;
    read_dword(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
               L"AppsUseLightTheme", light);
    return light == 0 ? Appearance::Dark : Appearance::Light;
}

Color system_accent() {
    if (high_contrast_enabled()) {
        return system_color(COLOR_HIGHLIGHT);
    }

    DWORD value = 0;
    if (read_dword(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\DWM", L"AccentColor", value)) {
        // AccentColor 的字节序是 0xAABBGGRR。
        const auto red = static_cast<uint8_t>(value & 0xFFu);
        const auto green = static_cast<uint8_t>((value >> 8) & 0xFFu);
        const auto blue = static_cast<uint8_t>((value >> 16) & 0xFFu);
        if (red != 0 || green != 0 || blue != 0) {
            return Color::rgb(red, green, blue);
        }
    }

    return Color::rgb(0x00, 0x78, 0xD4);  // Windows 默认蓝
}

Theme make_theme(Appearance appearance, bool material_visible) {
    if (appearance == Appearance::Auto) {
        appearance = detect_appearance();
    }

    Theme theme;
    theme.appearance = appearance;
    theme.material_visible = material_visible && appearance != Appearance::HighContrast;

    theme.title = make_style(22.0f, 600, false, true);
    theme.heading = make_style(16.0f, 600);
    theme.body = make_style(14.0f, 400);
    theme.caption = make_style(12.0f, 400);
    theme.button = make_style(14.0f, 400);
    theme.mono = make_style(12.0f, 400);
    theme.mono.family = L"Consolas";

    if (appearance == Appearance::HighContrast) {
        // 高对比度不启用材质：系统色可能白底，深色写死的文字会白底白字。
        theme.text = system_color(COLOR_WINDOWTEXT);
        theme.text_secondary = system_color(COLOR_WINDOWTEXT);
        theme.text_disabled = system_color(COLOR_GRAYTEXT);
        theme.accent = system_color(COLOR_HIGHLIGHT);
        theme.on_accent = system_color(COLOR_HIGHLIGHTTEXT);
        theme.card = system_color(COLOR_WINDOW);
        theme.card_border = system_color(COLOR_WINDOWTEXT);
        theme.control = system_color(COLOR_WINDOW);
        theme.control_border = system_color(COLOR_WINDOWTEXT);
        theme.control_hover = system_color(COLOR_HIGHLIGHT);
        theme.control_pressed = system_color(COLOR_HIGHLIGHT);
        theme.window_solid = system_color(COLOR_WINDOW);
        theme.focus_ring = system_color(COLOR_HIGHLIGHT);
        theme.selection = system_color(COLOR_HIGHLIGHT);
        theme.table_header = system_color(COLOR_WINDOW);
        theme.table_hover = system_color(COLOR_HIGHLIGHT);
        theme.progress_track = system_color(COLOR_WINDOW);
        theme.progress_fill = system_color(COLOR_HIGHLIGHT);
        theme.scrollbar_track = system_color(COLOR_WINDOW);
        theme.scrollbar_thumb = system_color(COLOR_WINDOWTEXT);
        theme.warning = system_color(COLOR_WINDOWTEXT);
        theme.warning_surface = system_color(COLOR_WINDOW);
        theme.radius_card = 4.0f;
        theme.radius_control = 2.0f;
        return theme;
    }

    const bool dark = appearance == Appearance::Dark;
    Color accent = system_accent();
    if (dark) {
        // 深色主题下系统强调色常常太暗（贴在深底上看不清），提亮一点。
        accent = accent.mix(Color::white(), 0.16f);
    }

    theme.accent = accent;
    theme.on_accent = brightness(accent) > 0.55f ? Color::black() : Color::white();
    theme.focus_ring = accent;
    theme.selection = dark ? Color::rgba(76, 194, 255, 0.26f) : Color::rgba(0, 120, 212, 0.16f);
    theme.warning = dark ? Color::rgb(0xFF, 0x99, 0xA4) : Color::rgb(0xC4, 0x2B, 0x1C);
    theme.warning_surface = dark ? Color::rgba(255, 153, 164, 0.10f) : Color::rgba(196, 43, 28, 0.08f);

    if (dark) {
        theme.text = Color::rgb(0xF2, 0xF2, 0xF2);
        theme.text_secondary = Color::rgb(0xC2, 0xC2, 0xC2);
        theme.text_disabled = Color::rgb(0x80, 0x80, 0x80);
        theme.window_solid = Color::rgb(0x20, 0x20, 0x20);
        theme.progress_fill = accent;
    } else {
        theme.text = Color::rgb(0x1B, 0x1B, 0x1B);
        theme.text_secondary = Color::rgb(0x5C, 0x5C, 0x5C);
        theme.text_disabled = Color::rgb(0x9B, 0x9B, 0x9B);
        theme.window_solid = Color::rgb(0xF3, 0xF3, 0xF3);
        theme.progress_fill = accent;
    }

    if (theme.material_visible) {
        // 半透明卡片：材质从底下透出来。文字由 DirectWrite 灰度 AA 绘制，
        // 在带 alpha 的表面上也能写正确的预乘 alpha，不再需要不透明底。
        theme.card = dark ? Color::rgba(38, 38, 38, 0.66f) : Color::rgba(255, 255, 255, 0.70f);
        theme.card_border = dark ? Color::rgba(255, 255, 255, 0.09f) : Color::rgba(0, 0, 0, 0.08f);
        theme.control = dark ? Color::rgba(255, 255, 255, 0.06f) : Color::rgba(255, 255, 255, 0.58f);
        theme.control_border = dark ? Color::rgba(255, 255, 255, 0.12f) : Color::rgba(0, 0, 0, 0.12f);
        theme.control_hover = dark ? Color::rgba(255, 255, 255, 0.12f) : Color::rgba(255, 255, 255, 0.82f);
        theme.control_pressed = dark ? Color::rgba(255, 255, 255, 0.18f) : Color::rgba(255, 255, 255, 0.40f);
        theme.table_header = dark ? Color::rgba(255, 255, 255, 0.05f) : Color::rgba(255, 255, 255, 0.45f);
        theme.table_hover = dark ? Color::rgba(255, 255, 255, 0.04f) : Color::rgba(0, 0, 0, 0.03f);
        theme.progress_track = dark ? Color::rgba(255, 255, 255, 0.10f) : Color::rgba(0, 0, 0, 0.08f);
        theme.scrollbar_track = Color::transparent();
        theme.scrollbar_thumb = dark ? Color::rgba(255, 255, 255, 0.28f) : Color::rgba(0, 0, 0, 0.26f);
    } else {
        theme.card = dark ? Color::rgb(0x2B, 0x2B, 0x2B) : Color::rgb(0xFC, 0xFC, 0xFC);
        theme.card_border = dark ? Color::rgb(0x3A, 0x3A, 0x3A) : Color::rgb(0xE2, 0xE2, 0xE2);
        theme.control = dark ? Color::rgb(0x2F, 0x2F, 0x2F) : Color::rgb(0xFF, 0xFF, 0xFF);
        theme.control_border = dark ? Color::rgb(0x45, 0x45, 0x45) : Color::rgb(0xD6, 0xD6, 0xD6);
        theme.control_hover = dark ? Color::rgb(0x38, 0x38, 0x38) : Color::rgb(0xF7, 0xF7, 0xF7);
        theme.control_pressed = dark ? Color::rgb(0x33, 0x33, 0x33) : Color::rgb(0xEA, 0xEA, 0xEA);
        theme.table_header = dark ? Color::rgb(0x2A, 0x2A, 0x2A) : Color::rgb(0xF0, 0xF0, 0xF0);
        theme.table_hover = dark ? Color::rgb(0x33, 0x33, 0x33) : Color::rgb(0xF2, 0xF2, 0xF2);
        theme.progress_track = dark ? Color::rgb(0x3A, 0x3A, 0x3A) : Color::rgb(0xE1, 0xE1, 0xE1);
        theme.scrollbar_track = Color::transparent();
        theme.scrollbar_thumb = dark ? Color::rgb(0x5A, 0x5A, 0x5A) : Color::rgb(0xC0, 0xC0, 0xC0);
    }

    return theme;
}

}  // namespace msixpkg::ui
