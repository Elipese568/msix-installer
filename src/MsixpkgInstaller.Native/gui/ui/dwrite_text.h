#pragma once

// DirectWrite 灰度文字后端（全框架唯一文字路径）。
//
// 为什么不再用 GDI ClearType：ClearType 需要已知的不透明背景；在 Mica/Acrylic
// 的透明表面上只能靠 alpha 补丁，既出鬼影，GDI 与 GDI+ 的排版度量还不一致
// （同一份布局里两种基线）。DWrite 的灰度 AA 写正确的预乘 alpha，透明/不透明
// 表面都能用，且测量与绘制来自同一套 layout —— 位置只算一次。

#include "canvas.h"

#include <Windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <string>
#include <unordered_map>

namespace msixpkg::ui {

class DirectWriteText final : public TextMetrics {
public:
    DirectWriteText();
    ~DirectWriteText() override;

    DirectWriteText(const DirectWriteText&) = delete;
    DirectWriteText& operator=(const DirectWriteText&) = delete;

    bool initialize();
    /// 把 D2D DC 渲染目标绑到帧的 DIB。尺寸/DC 不变时是空操作。
    bool bind(HDC memory_dc, int width_px, int height_px);
    void reset();

    bool begin(float dpi);
    void draw(const TextCommand& command);
    void end();

    bool ready() const { return target_ != nullptr; }

    float measure_width(std::wstring_view text, const TextStyle& style, float dpi) override;
    float line_height(const TextStyle& style, float dpi) override;

private:
    struct FormatEntry {
        Microsoft::WRL::ComPtr<IDWriteTextFormat> format;
        Microsoft::WRL::ComPtr<IDWriteInlineObject> ellipsis;
    };

    FormatEntry* entry_for(const TextStyle& style);
    static std::wstring key_for(const TextStyle& style);

    Microsoft::WRL::ComPtr<ID2D1Factory> d2d_factory_;
    Microsoft::WRL::ComPtr<IDWriteFactory> dwrite_factory_;
    Microsoft::WRL::ComPtr<ID2D1DCRenderTarget> target_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush_;
    std::unordered_map<std::wstring, FormatEntry> formats_;
    std::unordered_map<std::wstring, float> line_heights_;
    HDC bound_dc_ = nullptr;
    int width_px_ = 0;
    int height_px_ = 0;
    bool drawing_ = false;
};

}  // namespace msixpkg::ui
