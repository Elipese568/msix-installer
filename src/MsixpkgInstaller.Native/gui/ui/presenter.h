#pragma once

// 帧 → 窗口的呈现方式（docs/self-drawn-ui-framework.md §3.2 / §4.5）。
//
// GdiPresenter 是默认路径：普通窗口 + 重定向位图 + GDI+ SourceCopy。
// LayeredPresenter 是 Win10 Acrylic 不成立时的备选：WS_EX_LAYERED +
// UpdateLayeredWindow，逐像素 alpha 确定，但材质是否可见要真机验证。

#include "canvas.h"

#include <Windows.h>
#include <memory>

namespace msixpkg::ui {

class Presenter {
public:
    virtual ~Presenter() = default;
    virtual const wchar_t* name() const = 0;
    virtual void present(const Frame& frame, HDC dc) = 0;
};

std::unique_ptr<Presenter> make_gdi_presenter();
std::unique_ptr<Presenter> make_layered_presenter(HWND hwnd);

}  // namespace msixpkg::ui
