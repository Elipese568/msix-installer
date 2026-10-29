#pragma once

// 安装器自有的 Wren 控件扩展（不属于 Wren 框架本身）。
//
// Wren 故意只提供 Label / Button / Checkbox / ProgressBar / TextView / Table；
// 卡片、步骤条、阶段时间线、状态徽记、警告横幅这类「这个产品才有的」视觉组件，
// 按 wren-ui 技能的约定由使用方自己写 Widget。这里全部只画形状、取 Theme token，
// 不硬编码颜色，也不假设材质是否可见。

#include "widget.h"

#include <string>
#include <vector>

namespace msixpkg::gui {

/// 圆角卡片。继承 Panel 是因为它要装子控件：先画半透明表面，再交给 Panel 画子节点。
class Card : public wren::Panel {
public:
    enum class Tone { Surface, Warning, Accent };

    void set_tone(Tone tone);
    void paint(wren::Canvas& canvas) override;

private:
    Tone tone_ = Tone::Surface;
};

/// 一条 1px 分隔线。纯装饰，不进 MSAA。
class Divider : public wren::Widget {
public:
    void paint(wren::Canvas& canvas) override;
    wren::Role accessible_role() const override { return wren::Role::Panel; }
    wren::Widget* hit_test(float, float) override { return nullptr; }
};

/// 顶部四步指示条（确认 → 信任 → 安装 → 结果）。纯装饰；当前页由 active 决定。
class StepRail : public wren::Widget {
public:
    void set_labels(std::vector<std::wstring> labels);
    void set_active(int index);
    void paint(wren::Canvas& canvas) override;
    wren::Role accessible_role() const override { return wren::Role::Panel; }
    wren::Widget* hit_test(float, float) override { return nullptr; }

private:
    std::vector<std::wstring> labels_;
    int active_ = 0;
};

/// 安装阶段时间线：两列 × 四行，按「已完成 / 进行中 / 待执行 / 失败」着色。
///
/// 它没有子控件，所以不进焦点链；但读屏器需要知道「走到哪一步了」，
/// 因此角色是 Text 并用一段本地化摘要作为 accessible_name。
class PhaseTimeline : public wren::Widget {
public:
    void set_items(std::vector<std::wstring> items);
    /// active_index 为 -1 表示还没开始；failed 为真时 active 那一项画成失败。
    void set_state(int active_index, bool failed);
    void set_state_labels(std::wstring done, std::wstring active, std::wstring pending,
                          std::wstring failed);
    void paint(wren::Canvas& canvas) override;
    wren::Role accessible_role() const override { return wren::Role::Text; }
    std::wstring accessible_name() const override;
    wren::Widget* hit_test(float, float) override { return nullptr; }

private:
    std::wstring state_text(int index) const;

    std::vector<std::wstring> items_;
    int active_ = -1;
    bool failed_ = false;
    std::wstring done_{L"done"};
    std::wstring active_text_{L"running"};
    std::wstring pending_{L"pending"};
    std::wstring failed_text_{L"failed"};
};

/// 结果页的圆形状态徽记（对勾 / 叉 / 感叹号）。纯装饰。
class StatusMark : public wren::Widget {
public:
    enum class Kind { Success, Failure, Info };

    void set_kind(Kind kind);
    void paint(wren::Canvas& canvas) override;
    wren::Role accessible_role() const override { return wren::Role::Panel; }
    wren::Widget* hit_test(float, float) override { return nullptr; }

private:
    Kind kind_ = Kind::Info;
};

/// 摘要页左上角的包图标底座（自绘一个盒子）。纯装饰。
class PackageTile : public wren::Widget {
public:
    void paint(wren::Canvas& canvas) override;
    wren::Role accessible_role() const override { return wren::Role::Panel; }
    wren::Widget* hit_test(float, float) override { return nullptr; }
};

/// 警告 / 信息横幅：图标 + 自动换行文字。文字本身是 MSAA 的静态文本。
class Banner : public wren::Widget {
public:
    enum class Kind { Info, Warning };

    void set_kind(Kind kind);
    void set_message(std::wstring message);
    void paint(wren::Canvas& canvas) override;
    wren::Role accessible_role() const override { return wren::Role::Label; }
    std::wstring accessible_name() const override { return text; }
    wren::Widget* hit_test(float, float) override { return nullptr; }

private:
    Kind kind_ = Kind::Info;
};

}  // namespace msixpkg::gui
