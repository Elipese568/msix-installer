#pragma once

// 自绘 UI 框架的绘图后端（docs/self-drawn-ui-framework.md §3）。
//
// 这里是全框架**唯一**接触 GDI+ 的地方：上层只看到 RectF / Color / TextStyle 和
// Canvas 的绘制动词。将来换 D2D 时只需要另一个 Canvas 实现，Widget 不用动。
//
// 坐标一律是 DIP（与现有 ControlSpec 的 dp 坐标同一套）；Canvas 内部按 dpi/96
// 做一次 ScaleTransform，所以调用方不需要自己乘。像素单位只出现在 Frame 上。

#include <Windows.h>
// WIN32_LEAN_AND_MEAN 会把 OLE 头排除掉，而 GDI+ 的 Image/Metafile 声明用到
// IStream/PROPID —— 必须在 <gdiplus.h> 之前显式补上。
#include <objidl.h>

#include <gdiplus.h>
#include <d2d1_1.h>
#include <wrl/client.h>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace msixpkg::ui {

struct RectF {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;

    float right() const { return x + width; }
    float bottom() const { return y + height; }
    bool empty() const { return width <= 0.0f || height <= 0.0f; }
    bool contains(float px, float py) const {
        return px >= x && py >= y && px < right() && py < bottom();
    }

    RectF inset(float amount) const {
        return {x + amount, y + amount, width - amount * 2.0f, height - amount * 2.0f};
    }

    RectF offset(float dx, float dy) const { return {x + dx, y + dy, width, height}; }
};

/// 0..1 的浮点颜色。GDI+ 只认 ARGB 字节，这里保留浮点是给主题插值/淡入淡出用的。
struct Color {
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    float a = 1.0f;

    static Color from_argb(uint32_t argb);
    static Color rgb(uint8_t r, uint8_t g, uint8_t b);
    static Color rgba(uint8_t r, uint8_t g, uint8_t b, float alpha);
    static Color from_colorref(COLORREF value);
    static Color white();
    static Color black();
    static Color transparent();

    Color with_alpha(float alpha) const { return {r, g, b, alpha}; }
    Color mix(const Color& other, float t) const;

    Gdiplus::Color to_gdiplus() const;
};

enum class IconId {
    None,
    Check,
    ChevronRight,
    Info,
    Warning,
    Shield,
    Close,
    Download,
};

enum class TextAlign { Left, Center, Right };
enum class TextVAlign { Top, Center, Bottom };

struct TextStyle {
    std::wstring family = L"Microsoft YaHei UI";
    float size = 14.0f;  // DIP
    int weight = 400;    // 400 / 600
    bool wrap = false;
    bool ellipsis = false;
    bool italic = false;
    TextAlign align = TextAlign::Left;
    TextVAlign valign = TextVAlign::Top;
};

struct SizeF {
    float width = 0.0f;
    float height = 0.0f;
};

/// 一条待回放的文字。Widget 绘制阶段只记录，整棵树画完后由 DirectWrite 以
/// 灰度 AA 统一绘制（透明/不透明表面都能写正确的 alpha）；裁剪矩形随命令带走。
struct TextCommand {
    std::wstring text;
    RectF rect;
    TextStyle style;
    Color color;
    RectF clip;
};

class TextMetrics {
public:
    virtual ~TextMetrics() = default;
    /// 单行宽度（DIP）。换行文本由上层按 token 累加。
    virtual float measure_width(std::wstring_view text, const TextStyle& style, float dpi) = 0;
    /// 一行的高度（DIP，含行距）。
    virtual float line_height(const TextStyle& style, float dpi) = 0;
};

class Canvas {
public:
    Canvas(Gdiplus::Bitmap& bitmap, float dpi, int width_px, int height_px);
    /// D2D 硬件后端：形状直接画进 presenter 的 D2D 设备上下文（交换链 back buffer）。
    /// target 由调用方拥有，Canvas 只借用；文字仍由 DirectWriteText 画到同一个 target。
    Canvas(ID2D1DeviceContext* target, float dpi, int width_px, int height_px);
    ~Canvas();

    Canvas(const Canvas&) = delete;
    Canvas& operator=(const Canvas&) = delete;

    bool hardware() const { return d2d_ != nullptr; }

    void set_dpi(float dpi);
    float dpi() const { return dpi_; }
    /// 文字度量后端。没设时退回 GDI+ 度量（DWrite 初始化失败时用）。
    void set_text_metrics(TextMetrics* metrics) { metrics_ = metrics; }

    void fill_rect(const RectF& rect, const Color& color);
    void fill_round_rect(const RectF& rect, float radius, const Color& color);
    void stroke_round_rect(const RectF& rect, float radius, const Color& color, float width = 1.0f);
    void draw_line(float x1, float y1, float x2, float y2, const Color& color, float width = 1.0f);
    void draw_text(std::wstring_view text, const RectF& rect, const TextStyle& style, const Color& color);
    void draw_icon(IconId icon, const RectF& rect, const Color& color);
    void draw_image(const Gdiplus::Image& image, const RectF& rect);

    void push_clip(const RectF& rect);
    void pop_clip();
    void reset_clip();

    /// 按给定宽度换行后的整体尺寸（DIP）。wrap=false 时宽度是整行宽度。
    SizeF measure_text(std::wstring_view text, float max_width, const TextStyle& style);
    /// 单行宽度；换行的文本请用 measure_text。
    float measure_text_width(std::wstring_view text, const TextStyle& style);
    /// 一行文字的行高（字体度量，不含行距）。
    float line_height(const TextStyle& style);

    // ---- 延迟文字 ----
    void set_defer_text(bool defer) { defer_text_ = defer; }
    bool defer_text() const { return defer_text_; }
    const std::vector<TextCommand>& text_commands() const { return text_commands_; }
    void clear_text_commands() { text_commands_.clear(); }
    /// 直接把文字用 GDI+ 画进帧（DWrite 失败时的兜底 / 分层回退用）。
    void draw_text_immediate(std::wstring_view text, const RectF& rect, const TextStyle& style, const Color& color,
                             const RectF& clip = {});
    void flush();

private:
    Gdiplus::Graphics& graphics();
    Gdiplus::Font* font_for(const TextStyle& style);
    Gdiplus::StringFormat* draw_format(const TextStyle& style);
    Gdiplus::StringFormat* measure_format(bool wrap);
    float line_ratio(const TextStyle& style);
    RectF current_clip() const;
    void draw_icon_d2d(IconId icon, const RectF& rect, const Color& color);

    std::unique_ptr<Gdiplus::Graphics> graphics_;
    ID2D1DeviceContext* d2d_ = nullptr;  // 非拥有
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> d2d_brush_;
    Microsoft::WRL::ComPtr<ID2D1StrokeStyle> d2d_stroke_;
    size_t d2d_clip_depth_ = 0;
    float dpi_ = 96.0f;
    TextMetrics* metrics_ = nullptr;
    int width_px_ = 0;
    int height_px_ = 0;
    bool defer_text_ = false;
    std::vector<TextCommand> text_commands_;
    std::unordered_map<std::wstring, std::unique_ptr<Gdiplus::Font>> fonts_;
    std::unordered_map<int, std::unique_ptr<Gdiplus::StringFormat>> formats_;
    std::vector<Gdiplus::GraphicsState> clips_;
    std::vector<RectF> clip_rects_;
};

/// 一帧：32bpp 预乘 alpha 的 DIB，既能给 GDI+ 画，也能给 UpdateLayeredWindow 用。
///
/// 用 DIB 而不是 Gdiplus::Bitmap 自持内存，是为了让 LayeredPresenter 能直接拿
/// 同一块像素去 UpdateLayeredWindow —— 两条 present 路径共享同一份帧数据。
class Frame {
public:
    Frame(int width_px, int height_px, float dpi);
    ~Frame();

    Frame(const Frame&) = delete;
    Frame& operator=(const Frame&) = delete;

    void resize(int width_px, int height_px, float dpi);

    Canvas& canvas() const { return *canvas_; }
    Gdiplus::Bitmap& bitmap() const { return *bitmap_; }
    int width_px() const { return width_px_; }
    int height_px() const { return height_px_; }
    float dpi() const { return dpi_; }

    HDC memory_dc() const { return memory_dc_; }
    HBITMAP dib() const { return dib_; }
    /// 预乘 alpha 的 BGRA 像素，自上而下，行距 width_px() * 4。
    /// GPU 呈现要把它整块拷进交换链的 staging 纹理。
    const void* bits() const { return bits_; }

    /// 整帧清成 alpha=0（SourceCopy）。Mica/Acrylic 下这就是「透出材质」。
    void clear_transparent();

private:
    void create(int width_px, int height_px, float dpi);
    void destroy();

    int width_px_ = 0;
    int height_px_ = 0;
    float dpi_ = 96.0f;
    HDC memory_dc_ = nullptr;
    HBITMAP dib_ = nullptr;
    HGDIOBJ old_bitmap_ = nullptr;
    void* bits_ = nullptr;
    std::unique_ptr<Gdiplus::Bitmap> bitmap_;
    std::unique_ptr<Canvas> canvas_;
};

}  // namespace msixpkg::ui
