#pragma once

// 窗口背景材质（docs/self-drawn-ui-framework.md §4）。
//
// 三种材质只负责「让窗口后面透出什么」：
//   Mica    —— Win11 26100+，属性 38 + 39 + 扩展边框；
//   Acrylic —— Win10/旧 Win11，SetWindowCompositionAttribute（未文档化，隔离在本文件）；
//   Solid   —— 正式支持的兜底，不是失败态。
//
// Widget 不感知材质：它只画半透明/实色表面，底下透出什么由 Material 决定。

#include "canvas.h"

#include <memory>

namespace msixpkg::ui {

enum class MaterialKind { Solid, Acrylic, Mica };
enum class MaterialPreference { Auto, Solid, Acrylic, Mica };

class Material {
public:
    virtual ~Material() = default;
    virtual MaterialKind kind() const = 0;
    /// 成功返回 true；任何一步失败都必须返回 false（Host 会依次回退）。
    virtual bool apply(HWND hwnd, bool dark) = 0;
    virtual Color tint() const = 0;
};

std::unique_ptr<Material> make_material(MaterialPreference preference);

/// 切换标题栏亮暗。Windows 11 用属性 20，旧版本用 19；两个都失败就忽略。
void apply_dark_title_bar(HWND hwnd, bool dark);

const wchar_t* material_name(MaterialKind kind);

}  // namespace msixpkg::ui
