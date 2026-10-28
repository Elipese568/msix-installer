#include "material.h"

#include <Windows.h>
#include <dwmapi.h>

namespace msixpkg::ui {
namespace {

// DWM 属性号写死而不是用 SDK 枚举：这些值从 Windows 10 到 11 一直稳定，
// 用返回值判断支持与否即可，不依赖编译时 SDK 版本。
constexpr DWORD kDwmwaUseImmersiveDarkMode = 20;
constexpr DWORD kDwmwaUseImmersiveDarkModeBefore20H1 = 19;
constexpr DWORD kDwmwaWindowCornerPreference = 33;
constexpr DWORD kDwmwaSystemBackdropType = 38;
constexpr DWORD kDwmwaRedirectionBitmapAlpha = 39;
constexpr DWORD kDwmsbtMainWindow = 2;
constexpr DWORD kDwmwcpRound = 2;

void apply_common_window_attributes(HWND hwnd, bool dark) {
    apply_dark_title_bar(hwnd, dark);

    const DWORD corner = kDwmwcpRound;
    ::DwmSetWindowAttribute(hwnd, kDwmwaWindowCornerPreference, &corner, sizeof(corner));
}
bool extend_frame(HWND hwnd) {
    MARGINS margins{-1, -1, -1, -1};
    return SUCCEEDED(::DwmExtendFrameIntoClientArea(hwnd, &margins));
}

class MicaMaterial final : public Material {
public:
    MaterialKind kind() const override { return MaterialKind::Mica; }
    Color tint() const override { return Color::rgba(242, 242, 242, 0.0f); }

    bool apply(HWND hwnd, bool dark) override {
        if (hwnd == nullptr) {
            return false;
        }

        apply_common_window_attributes(hwnd, dark);

        const DWORD backdrop = kDwmsbtMainWindow;
        if (FAILED(::DwmSetWindowAttribute(hwnd, kDwmwaSystemBackdropType, &backdrop, sizeof(backdrop)))) {
            return false;
        }

        // 这一条是 Win11 26100+：没有它，DWM 把重定向位图当完全不透明，材质永远被盖住。
        const BOOL alpha = TRUE;
        if (FAILED(::DwmSetWindowAttribute(hwnd, kDwmwaRedirectionBitmapAlpha, &alpha, sizeof(alpha)))) {
            return false;
        }

        return extend_frame(hwnd);
    }
};

// ---- Acrylic：未文档化 API，全部隔离在这个类里 ----
//
// 结构体自己定义，不依赖 SDK 头；GetProcAddress 动态取，Win10 上没有也不会崩。
struct AccentPolicy {
    DWORD state;
    DWORD flags;
    DWORD gradient_color;  // 0xAABBGGRR
    DWORD animation_id;
};

struct WindowCompositionAttributeData {
    DWORD attribute;
    void* data;
    SIZE_T size;
};

using SetWindowCompositionAttributeFn = BOOL(WINAPI*)(HWND, WindowCompositionAttributeData*);

constexpr DWORD kAccentEnableAcrylicBlurBehind = 4;
constexpr DWORD kAccentEnableBlurBehind = 3;
constexpr DWORD kWcaAccentPolicy = 19;

SetWindowCompositionAttributeFn load_set_window_composition_attribute() {
    static const auto function = reinterpret_cast<SetWindowCompositionAttributeFn>(
        ::GetProcAddress(::GetModuleHandleW(L"user32.dll"), "SetWindowCompositionAttribute"));
    return function;
}

bool set_accent(HWND hwnd, DWORD state, DWORD gradient_color) {
    const auto set_attribute = load_set_window_composition_attribute();
    if (set_attribute == nullptr) {
        return false;
    }

    AccentPolicy policy{};
    policy.state = state;
    policy.flags = 2;
    policy.gradient_color = gradient_color;
    policy.animation_id = 0;

    WindowCompositionAttributeData data{};
    data.attribute = kWcaAccentPolicy;
    data.data = &policy;
    data.size = sizeof(policy);
    return set_attribute(hwnd, &data) != FALSE;
}

class AcrylicMaterial final : public Material {
public:
    MaterialKind kind() const override { return MaterialKind::Acrylic; }
    Color tint() const override { return Color::rgba(243, 243, 243, 0.0f); }

    bool apply(HWND hwnd, bool dark) override {
        if (hwnd == nullptr) {
            return false;
        }

        apply_common_window_attributes(hwnd, dark);

        if (!extend_frame(hwnd)) {
            return false;
        }

        // 0xAABBGGRR：深色给一层暗色 tint，浅色给亮色。alpha 太高会把后面的
        // 内容压死，0x99 是亚克力比较常见的取法。
        const DWORD tint = dark ? 0x99202020u : 0x99F3F3F3u;
        if (set_accent(hwnd, kAccentEnableAcrylicBlurBehind, tint)) {
            return true;
        }

        // Win10 1803 之前的旧形态：只有模糊没有 tint/noise，仍比纯色强。
        return set_accent(hwnd, kAccentEnableBlurBehind, tint);
    }
};

class SolidMaterial final : public Material {
public:
    MaterialKind kind() const override { return MaterialKind::Solid; }
    Color tint() const override { return Color::rgb(0x20, 0x20, 0x20); }
    bool apply(HWND hwnd, bool dark) override {
        if (hwnd == nullptr) {
            return false;
        }

        // Solid 也是正式材质：标题栏/圆角照常跟随主题，只是没有透明背景。
        apply_common_window_attributes(hwnd, dark);

        // 关掉可能残留的材质属性（从 Mica 切回 Solid 时）。
        const BOOL alpha = FALSE;
        ::DwmSetWindowAttribute(hwnd, kDwmwaRedirectionBitmapAlpha, &alpha, sizeof(alpha));

        MARGINS margins{0, 0, 0, 0};
        ::DwmExtendFrameIntoClientArea(hwnd, &margins);
        return true;
    }
};

}  // namespace

void apply_dark_title_bar(HWND hwnd, bool dark) {
    if (hwnd == nullptr) {
        return;
    }

    const BOOL value = dark ? TRUE : FALSE;
    ::DwmSetWindowAttribute(hwnd, kDwmwaUseImmersiveDarkMode, &value, sizeof(value));
    // 老属性号也写一次：部分 26H2 预览版同时认这两个。
    ::DwmSetWindowAttribute(hwnd, kDwmwaUseImmersiveDarkModeBefore20H1, &value, sizeof(value));

    // 已经上屏的窗口需要一次非客户区重算，标题栏才会立刻换成新亮暗。
    ::SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    ::DwmFlush();
}

std::unique_ptr<Material> make_material(MaterialPreference preference) {    switch (preference) {
        case MaterialPreference::Mica:
            return std::make_unique<MicaMaterial>();
        case MaterialPreference::Acrylic:
            return std::make_unique<AcrylicMaterial>();
        case MaterialPreference::Solid:
        case MaterialPreference::Auto:
            return std::make_unique<SolidMaterial>();
    }

    return std::make_unique<SolidMaterial>();
}

const wchar_t* material_name(MaterialKind kind) {
    switch (kind) {
        case MaterialKind::Mica: return L"Mica";
        case MaterialKind::Acrylic: return L"Acrylic";
        case MaterialKind::Solid: return L"Solid";
    }

    return L"Solid";
}

}  // namespace msixpkg::ui
