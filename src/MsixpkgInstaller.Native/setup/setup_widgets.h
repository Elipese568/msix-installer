#pragma once

// setup 自己的一个小控件：Wren 没有 RadioButton，而「为所有用户 / 仅为我」需要
// 一个可聚焦、可读屏的单选卡片。只画形状，颜色全部取 Theme token。

#include "widget.h"

#include <functional>
#include <string>

namespace msixpkg::setup {

class ChoiceCard : public wren::Widget {
public:
    ChoiceCard(std::wstring title, std::wstring detail, std::function<void()> on_select);

    void set_selected(bool selected);
    bool selected() const { return selected_; }

    void paint(wren::Canvas& canvas) override;
    bool on_mouse(const wren::MouseEvent& event) override;
    bool on_key(const wren::KeyEvent& event) override;
    wren::Role accessible_role() const override { return wren::Role::Checkbox; }
    uint32_t accessible_state() const override;
    std::wstring accessible_default_action() const override { return L"选择"; }
    void do_default_action() override { select(); }

private:
    void select();

    std::wstring detail_;
    std::function<void()> on_select_;
    bool selected_ = false;
};

}  // namespace msixpkg::setup
