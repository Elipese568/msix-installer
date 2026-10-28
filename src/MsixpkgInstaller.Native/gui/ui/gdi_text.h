#pragma once

// GDI ClearType 文字回放。
//
// GDI+ 在预乘 alpha 位图上只能给出灰度抗锯齿，而且 ClearType 会出彩色重影。
// 但 GDI 的 DrawTextW 写到「已经是不透明表面」的 DIB 上时，alpha 保持 255、
// RGB 由 ClearType 子像素合成 —— 这正是标准 Win32 控件的观感。
//
// 所以 Host 把不透明表面上的文字延迟到整帧画完后再用这里回放；透明/材质表面
// 仍走 GDI+ 灰度 AA（ClearType 在透明背景上没有正确的背景色可混）。

#include "canvas.h"

#include <Windows.h>

#include <string>
#include <unordered_map>

namespace msixpkg::ui {

class GdiTextRenderer {
public:
    GdiTextRenderer() = default;
    ~GdiTextRenderer();

    GdiTextRenderer(const GdiTextRenderer&) = delete;
    GdiTextRenderer& operator=(const GdiTextRenderer&) = delete;

    void draw(HDC dc, const TextCommand& command, float dpi);
    void clear();

private:
    HFONT font_for(const TextStyle& style, float dpi);

    std::unordered_map<std::wstring, HFONT> fonts_;
};

}  // namespace msixpkg::ui
