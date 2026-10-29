# 全自绘 GDI+ 安装器 UI 框架设计

> 状态：设计稿（2026-10-28）；框架核心 + demo 已实现
> （`feature/self-drawn-ui`，2026-10-29，代码与验证记录见
> [`src/MsixpkgInstaller.Native/gui/ui/README.md`](../src/MsixpkgInstaller.Native/gui/ui/README.md)）
> 范围：`src/MsixpkgInstaller.Native/gui`
> 目标：单 HWND、全自绘、GDI+、小型 C++ UI 框架；Win11 用 Mica，Win10/旧 Win11 用 Acrylic，失败回退 Solid。
> 关联：`docs/installer-design.md` §8（GUI 契约）、`docs/wpf-ui-feasibility.md` §10（D2D/Mica 评估）、`docs/handoff.md`（UIA 与提权注意事项）。

---

## 0. 结论

1. **做一个小型 retained-mode 框架，而不是通用 UI 库。** 固定 720×480 dp、五个页面、无文本输入、无多窗口；不需要 measure/arrange 约束系统、数据绑定、XAML、CSS、富文本或 IME 编辑器。
2. **渲染后端先用 GDI+。** 当前仓库已经有 GDI+ 写 alpha 的路径，`gdiplus` 已在链接里；对这个尺寸的安装器，GDI+ 的圆角、渐变、半透明、文字、图标足够，性能不是瓶颈。`Canvas` 保持抽象，后续可以换 D2D，但第一版不做双后端。
3. **把“怎么画”和“怎么呈现”分开。** `Canvas` 只画到 32bpp 预乘 alpha 帧；`Presenter` 决定用普通窗口、layered window 还是 DComp；`Material` 决定 Mica、Acrylic 还是 Solid。Widget 不感知材质。
4. **保留现有 `Wizard` 状态机、`gui_worker.cpp`、引擎、字符串与命令行。** 只替换 `gui_wizard.cpp` 里的标准控件层；`gui_ui.cpp` 的 42 条 `ControlSpec` 继续作为布局与 Tab 顺序的单一事实来源。
5. **无障碍不是可选项。** 标准控件一旦移除，`IsDialogMessageW` 的键盘导航、3 个只读多行 EDIT 的复制/朗读、ListView 的表格语义都要自己补。建议先做 MSAA `IAccessible`，不要等五页写完再补。
6. **最大风险是 Win10 的 alpha 合成。** Mica 在 26100+ 依赖 `DWMWA_REDIRECTIONBITMAP_ALPHA`；Win10 没有这个属性，Acrylic 能否透过 GDI+ 内容必须先做 1 天 spike。通过后再铺五页。

---

## 1. 现状与约束

### 1.1 现有 UI

| 项 | 现状 |
|---|---|
| 技术栈 | 纯 Win32 + `comctl32` v6 + `dwmapi` + `gdiplus`；无 .NET |
| 窗口 | 固定 720×480 dp，标准 caption，PerMonitorV2 |
| 控件 | 42 个 `ControlSpec`：`STATIC`、`BUTTON`（普通/复选框）、`EDIT`（只读多行）、`PROGRESS_CLASSW`、`WC_LISTVIEWW` |
| 代码 | `gui_wizard.cpp` ~2,150 行；`gui_ui.cpp` 234 行；`gui_worker.cpp` ~750 行；`gui_strings.cpp` 182 行 |
| 页面 | Summary / Consent / Progress / Result / Manage |
| 线程 | UI 线程只读 `SharedState`；耗时工作经 `gui_worker.cpp` 投到后台，结果用 `PostMessage` 回到 `Wizard::handle` |
| 无障碍 | 目前完全依赖标准控件；`IsDialogMessageW` 提供 Tab/Shift+Tab/Enter/Esc |

### 1.2 标准控件目前承担了什么

这些不是“画出来的像素”，而是**行为**：

- `IsDialogMessageW` 的 Tab/Shift+Tab、默认按钮 Enter、Esc、方向键；
- 只读多行 `EDIT` 的滚动、Ctrl+A/C、屏幕阅读器文本读取（同意页证书详情、进度阶段表、管理页详情）；
- `ListView` 的 4 列、表头、行选择、滚动、屏幕阅读器“第几行第几列”；
- `WM_CTLCOLOR*` 的主题色、禁用态、焦点框；
- `WS_TABSTOP` 的焦点顺序（创建顺序 == `ControlSpec` 顺序）。

代码里已经明确写了这些取舍：

- `gui_ui.cpp:53-54`：证书详情放在只读多行 `EDIT`，是为了 Tab 聚焦、Ctrl+A/C 复制与屏幕阅读器读取，比一堆静态文本更满足 §8。
- `gui_wizard.cpp:678-679`：列表要能被 Tab 聚焦、能被屏幕阅读器读出“第几行第几列”，所以用标准 `ListView` 而不是自绘列表。
- `gui_wizard.cpp:301-304`：`IsDialogMessageW` 提供标准键盘导航，自己实现这套规则是无障碍缺陷的常见来源。

全自绘必须把这些**逐项补回来**；这是工作量的主体，不是画圆角。

### 1.3 现有 Mica 路径

当前工作区这版已经能在 Win11 26100+ 上让 Mica 透出来：

1. `DWMWA_SYSTEMBACKDROP_TYPE = DWMSBT_MAINWINDOW`（属性 38）；
2. `DWMWA_REDIRECTIONBITMAP_ALPHA = TRUE`（属性 39，26100+）；
3. `DwmExtendFrameIntoClientArea(-1)`；
4. `WM_ERASEBKGND` 用 GDI+ `SourceCopy` 把客户区清成 `alpha=0`（`clear_to_transparent`）；
5. 文字/按钮仍由 GDI 标准控件画，alpha 写成不透明，留在材质上。

失败路径：`apply_backdrop` 在属性 39 失败时直接返回，客户区退回纯色（`#202020` 或系统窗口色）。Win10 目前就是这条路径。

**全自绘可以复用 1–4 步**，但要把“文字/按钮由 GDI 画”换成“整个帧由 GDI+ 画”。这带来两个新问题：

- GDI+ 文字在 alpha 表面上通常只能灰度抗锯齿，ClearType 会出彩边；
- 整帧 alpha 的 present 是否稳定，需要 spike 验证（当前只验证了“清背景”）。

### 1.4 为什么值得换

- 标准控件在深色/材质背景上需要 `WM_CTLCOLOR*`、`NM_CUSTOMDRAW`、`repaint_behind` 等补丁；EDIT 仍可能白底、按钮仍是系统样式，视觉不统一。
- 全自绘只有一条渲染路径：卡片、文字、按钮、复选框、进度条、表格都在同一个 `Canvas` 上，材质、圆角、间距、字体可以统一。
- 材质策略（Mica/Acrylic/Solid）与 widget 解耦，一套 UI 可以覆盖 Win10/11。
- 代价是无障碍、键盘、文本选择/复制、表格语义都要自己实现；这是本文档的重点。

---

## 2. 目标架构

### 2.1 分层

```
Wizard / SharedState / gui_worker.cpp        （现有，不动）
            │ 页面状态、进度、同意、结果
            ▼
WizardView                                   （五页组装，替换 render_*）
            │ Widget 树 + 绑定
            ▼
ui::Host                                     （HWND、消息、焦点、UIA）
   ├── Canvas (GDI+)                         （圆角/文字/图标/alpha）
   ├── Presenter                            （Dwm / Layered / DComp）
   ├── Material                             （Mica / Acrylic / Solid）
   └── Accessibility                        （MSAA IAccessible）
            │
            ▼
DWM / Win32                                  （窗口、材质、输入）
```

### 2.2 文件布局

```
src/MsixpkgInstaller.Native/gui/
  ui/
    canvas.h/.cpp          GDI+ Canvas、Frame、字体缓存、颜色、裁剪
    widget.h/.cpp          Widget、Panel、事件、命中测试
    host.h/.cpp            HWND、消息循环、双缓冲、焦点、失效
    presenter.h/.cpp       Presenter 接口 + GdiPresenter/LayeredPresenter
    material.h/.cpp        Material 接口 + Mica/Acrylic/Solid
    theme.h/.cpp           Light/Dark/HighContrast、强调色、字号、间距
    controls.h/.cpp        Label/Button/Checkbox/ProgressBar/TextView/Table/ScrollBar
    automation.h/.cpp      IAccessible 根对象与子对象
  wizard_view.h/.cpp       五页 widget 组装、SharedState 绑定、动作回调
  gui_wizard.cpp           保留：入口、单实例、消息、状态机、动作
  gui_ui.cpp               保留：ControlSpec 布局表（42 条）
  gui_worker.cpp           保留：后台编排
  gui_strings.cpp          保留：字符串表
```

> `ui/` 是内部框架，不导出给引擎；不要为了“以后复用”提前做通用布局/绑定。

### 2.3 对象模型

```cpp
namespace msixpkg::ui {

struct RectF { float x, y, width, height; };
struct Color { float r, g, b, a; };

using ControlId = int;
enum class Role { Label, Button, Checkbox, Progress, Text, Table, Row, ScrollBar };
// 位掩码：1=Focusable, 2=Focused, 4=Disabled, 8=Checked, 16=Selected, 32=ReadOnly
using State = uint32_t;

class Frame;
class Material;

struct TextStyle {
    std::wstring family = L"Microsoft YaHei UI";
    float size = 14.0f;        // DIP
    int weight = 400;          // 400 / 600
    bool wrap = false;
    bool ellipsis = false;
};

class Canvas {
public:
    virtual ~Canvas() = default;
    virtual void fill_round_rect(const RectF&, float radius, Color) = 0;
    virtual void stroke_round_rect(const RectF&, float radius, Color, float width) = 0;
    virtual void draw_text(std::wstring_view, const RectF&, const TextStyle&, Color) = 0;
    virtual void draw_icon(IconId, const RectF&, Color) = 0;
    virtual void draw_image(const Image&, const RectF&) = 0;
    virtual void push_clip(const RectF&) = 0;
    virtual void pop_clip() = 0;
};

class Widget {
public:
    virtual ~Widget() = default;

    RectF bounds;
    bool visible = true;
    bool enabled = true;

    virtual void paint(Canvas&) = 0;
    virtual bool hit_test(float x, float y) const;
    virtual bool on_mouse(const MouseEvent&) { return false; }
    virtual bool on_key(const KeyEvent&) { return false; }
    virtual void on_focus() {}
    virtual void on_blur() {}

    virtual std::wstring accessible_name() const = 0;
    virtual Role accessible_role() const = 0;
    virtual State accessible_state() const = 0;
};

class Presenter {
public:
    virtual ~Presenter() = default;
    virtual bool attach(HWND hwnd, Material& material) = 0;
    virtual void present(const Frame& frame, HDC dc) = 0;
    virtual void on_dpi_changed(UINT dpi) = 0;
};

class Host {
public:
    bool create(HINSTANCE instance, std::unique_ptr<Widget> root,
                std::unique_ptr<Presenter> presenter, std::unique_ptr<Material> material);
    void invalidate(const RectF* dirty = nullptr);
    int run_message_loop();
};

}  // namespace msixpkg::ui
```

设计规则：

- **单 HWND。** 没有子控件，所有内容在一个客户区里画。
- **固定尺寸。** 720×480 dp；DPI 只影响像素缩放，不做响应式重排。
- **绝对布局。** 复用 `ControlSpec` 的 dp 坐标；不做 measure/arrange 约束系统。
- **Tab 顺序 = `ControlSpec` 顺序。** 只跳过不可见/禁用；与现在 `IsDialogMessageW` 的视觉顺序一致。
- **Widget 不感知材质。** 背景由 `Material`/`Presenter` 透出；Widget 只画自己的半透明/实色表面。

---

## 3. 渲染

### 3.1 Frame

`Frame` 是一张 32bpp 预乘 alpha 的位图，尺寸为“客户区像素”，DPI 由 `Host` 维护。

```cpp
class Frame {
public:
    Frame(int width_px, int height_px, float dpi);
    Canvas& canvas();
    Gdiplus::Bitmap& bitmap();
    int width_px() const;
    int height_px() const;
    float dpi() const;

    void clear_transparent();   // GDI+ SourceCopy, alpha=0
};
```

- 每帧先 `clear_transparent()`；不要在 `WM_ERASEBKGND` 里画任何不透明底色。
- 所有绘制用 DIP 坐标；`Frame` 的 `Graphics` 设置 `SetPageUnit(UnitPixel)`，再 `ScaleTransform(dpi/96, dpi/96)`。
- 颜色用预乘 alpha 由 GDI+ 处理；不要自己乘。
- 字体按 DPI 创建并缓存；主题切换/DPI 变化时清缓存。

### 3.2 绘制与 present

```cpp
void Host::paint(HDC dc) {
    if (!frame_) frame_ = std::make_unique<Frame>(client_w_, client_h_, dpi_);
    frame_->clear_transparent();
    root_->paint(frame_->canvas());
    presenter_->present(*frame_, dc);
}
```

`GdiPresenter::present`：

```cpp
Gdiplus::Graphics screen(dc);
screen.SetCompositingMode(Gdiplus::CompositingModeSourceCopy);
screen.DrawImage(frame.bitmap(), 0, 0);
```

- `SourceCopy` 是关键：把整帧（含 alpha）覆盖到窗口重定向位图。
- 不要用 `AlphaBlend` 直接 blit 到窗口 DC，除非已验证目标 alpha 语义；`AlphaBlend` 可能保留目标 alpha，导致透明区不透。
- `GdiPresenter` 用**普通窗口**，不要加 `WS_EX_NOREDIRECTIONBITMAP`；后者是 DComp 交换链的路径。当前 Mica 验证过的是“普通窗口 + 属性 39 + GDI 重定向位图”，两者不要混。
- `WM_ERASEBKGND` 一律 `return 1`；背景由帧的 alpha 决定。
- 双缓冲：先画到 `Frame`，再一次性 present；不要在 `WM_PAINT` 里边画边显示。
- 只重画脏区是优化，不是第一版必需；720×480 全量重画成本很低。
- 进度/动画用 `InvalidateRect` 驱动，不做 60fps 常驻循环；不可见时停 timer。

### 3.3 文字

第一版建议先用 GDI+：

- `Graphics::DrawString` + `StringFormat`；
- `SetTextRenderingHint(AntiAliasGridFit)`；**不要**在 alpha 表面上用 `ClearTypeGridFit`，会出彩边；
- 中文用 `Microsoft YaHei UI`；英文标题可回退 `Segoe UI`；Win11 可选 `Segoe UI Variable Display`；
- 省略号用 `StringTrimmingEllipsisCharacter`；换行用 `StringFormatFlagsLineLimit` + 矩形；
- `MeasureString` 有内边距，布局时给 1–2 px 余量。

风险：GDI+ 的灰度 AA 会比当前 GDI 标准控件的 ClearType 略软，尤其是 100% DPI 的小字号。**这是全自绘相对当前方案最大的观感差异**，spike 里必须用同一段中文在 100%/150%/200% 下对比。

如果不可接受，两条路：

1. **文字继续用 GDI `DrawTextW`（ClearType）**，GDI+ 只画形状/图片。仍是全自绘控件，只是文字后端用 GDI；需要 `Graphics::Flush(FlushIntentionSync)` 后再调用 GDI，避免 GDI+ 批处理顺序问题。
2. 换 D2D/DirectWrite（灰度 AA，质量更稳但仍有取舍）。

> **实现记录（2026-10-29）**：最终采用方案 2 —— 全框架文字统一由 DirectWrite 灰度
> AA 绘制，GDI+ 只画形状并作为 DWrite 初始化失败时的兜底。原因：透明 Mica 表面需要
> 真实的逐像素 alpha；GDI ClearType 在预乘 alpha 位图上会出鬼影，而 GDI/GDI+ 双后端
> 会让透明标题与卡片正文的基线和字形不一致。实现与截图见
> `src/MsixpkgInstaller.Native/gui/ui/README.md`。

不要同时上 GDI+ 和 D2D 两套文字栈。

### 3.4 DPI、裁剪、脏矩形

- `WM_DPICHANGED`：更新 `dpi_`，重算 `client_w_/client_h_`，重建 `Frame`，`InvalidateRect`。
- 布局只存 DIP；像素 = `MulDiv(dp, dpi, 96)`。
- 裁剪用 `Canvas::push_clip/pop_clip`，内部用 `Graphics::SetClip`；Widget 自己不能越界画。
- 文本/表格滚动时只失效自己的矩形；第一版可以全窗口失效。
- 不要用 `WS_EX_COMPOSITED` 或 `CS_HREDRAW|CS_VREDRAW` 依赖系统擦除；自己双缓冲。

---

## 4. 材质

### 4.1 选择矩阵

| 系统 | 材质 | 实现 |
|---|---|---|
| Win11 26100+ | Mica | `DWMWA_SYSTEMBACKDROP_TYPE` + `DWMWA_REDIRECTIONBITMAP_ALPHA` + `DwmExtendFrameIntoClientArea(-1)` |
| Win11 22621–26099 | Acrylic | `SetWindowCompositionAttribute` + `DwmExtendFrameIntoClientArea(-1)`；或 D2D+DComp 的 Mica |
| Win10 19041+ | Acrylic | 同上；未文档化 API |
| 高对比度 | Solid | 系统色；不启用材质 |
| 远程桌面 / 关闭透明效果 | Solid | 读 `SM_REMOTESESSION`、`EnableTransparency` |
| 任一 API 失败 | Solid | 主题底色；不允许黑屏/白屏 |

`Material` 接口：

```cpp
enum class MaterialKind { Solid, Acrylic, Mica };

class Material {
public:
    virtual ~Material() = default;
    virtual MaterialKind kind() const = 0;
    virtual bool apply(HWND hwnd, bool dark) = 0;
    virtual Color tint() const = 0;          // 半透明卡片的基准色
    virtual void paint_fallback(Canvas&) = 0; // Solid 时填底
};
```

### 4.2 Mica

```cpp
DWORD backdrop = 2;  // DWMSBT_MAINWINDOW
if (FAILED(DwmSetWindowAttribute(hwnd, 38, &backdrop, sizeof(backdrop)))) return false;

BOOL alpha = TRUE;
if (FAILED(DwmSetWindowAttribute(hwnd, 39, &alpha, sizeof(alpha)))) return false;

MARGINS margins{-1, -1, -1, -1};
if (FAILED(DwmExtendFrameIntoClientArea(hwnd, &margins))) return false;
```

- 属性 39 是 Win11 26100+；更老的系统直接回退。
- `DWMWA_USE_IMMERSIVE_DARK_MODE`（20）控制标题栏/材质亮暗；`DWMWA_WINDOW_CORNER_PREFERENCE`（33）保留圆角。
- 当前 `gui_wizard.cpp` 的 `apply_backdrop` 已经是这条路径；迁移时搬进 `MicaMaterial`。
- 高对比度必须关闭材质：系统色可能白底，而文字颜色按深色写死会白底白字。

### 4.3 Acrylic

Win10 没有 Mica，但 `SetWindowCompositionAttribute` 的 `ACCENT_ENABLE_ACRYLICBLURBEHIND` 从 1803 起可用；Win11 22H2/23H2 也能用。它是**未文档化 API**，所以：

- 用 `GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetWindowCompositionAttribute")` 动态取，失败不崩溃；
- 结构体自己定义，不依赖 SDK 头；
- 封装在 `AcrylicMaterial` 一个文件里，代码审查时只看这一处。

```cpp
struct AccentPolicy {
    DWORD state;            // ACCENT_ENABLE_ACRYLICBLURBEHIND = 4
    DWORD flags;            // 2
    DWORD gradient_color;   // 0xAABBGGRR；按主题取深/浅
    DWORD animation_id;     // 0
};

struct WindowCompositionAttributeData {
    DWORD attribute;        // WCA_ACCENT_POLICY = 19
    void* data;
    SIZE_T size;
};

using SetWindowCompositionAttributeFn =
    BOOL(WINAPI*)(HWND, WindowCompositionAttributeData*);
```

顺序：

1. `DwmExtendFrameIntoClientArea(-1)`，让客户区参与扩展边框合成；
2. `SetWindowCompositionAttribute(WCA_ACCENT_POLICY, ...)`；
3. `WM_ERASEBKGND` 不画底，`WM_PAINT` 用 GDI+ 画内容；
4. 如果客户区变黑/不透明，说明 GDI+ 的 alpha 没有被 DWM 认，转 §4.5 的 layered presenter 或 D2D。

Acrylic 的现实约束：

- 性能：Win10 的亚克力拖窗/缩放明显比 Mica 贵；固定窗口、静态页面可以接受；进度动画不要 60fps。
- 透明效果关闭/节能模式：Windows 可能不显示材质；读 `HKCU\...\Themes\Personalize\EnableTransparency`，为 0 时用 Solid。
- 远程桌面：`GetSystemMetrics(SM_REMOTESESSION)` 为真时用 Solid。
- 高对比度：禁用。
- 观感：亚克力的 tint/noise 与 Mica 不同，主题 token 要分开；不要用同一套半透明值硬套。
- API 稳定性：未文档化，但长期被 TranslucentTB 等使用；仍要保留 Solid 回退。
- 如果 Acrylic 不可用，可退到 `ACCENT_ENABLE_BLURBEHIND`（3）或文档化的 `DwmEnableBlurBehindWindow`（只有模糊，没有 tint/noise），再不行用 Solid。

### 4.4 Solid

Solid 不是“失败态”，而是正式支持的材质：

- 主题底色 + 卡片色；
- 高对比度、RDP、关闭透明效果、旧系统都用它；
- 文字对比度必须按 WCAG/系统 HC 校验；
- 不要在 Solid 下还画半透明白卡，否则会灰。

### 4.5 Win10 alpha 的未决问题（最高风险）

Mica 的 alpha 依赖 26100+ 的 `DWMWA_REDIRECTIONBITMAP_ALPHA`；Win10 没有。Acrylic 要透过 GDI+ 内容，必须验证以下之一：

**方案 A：普通窗口 + glass frame + GDI+ `SourceCopy`**

- `DwmExtendFrameIntoClientArea(-1)`；
- `SetWindowCompositionAttribute` Acrylic；
- `Frame` 清 alpha 0，`SourceCopy` present；
- 观察：材质是否可见、文字是否正常、窗口拖动是否闪烁。

**方案 B：`WS_EX_LAYERED` + `UpdateLayeredWindow`**

- 逐像素 alpha 确定；
- 但亚克力在 layered window 后面是否可见要实测；
- 没有 DWM 阴影/圆角，需要自己画；
- 720×480 静态页面性能可接受，但不要 60fps。

**方案 C：D2D + DirectComposition**

- 透明交换链 + Acrylic；Win11 22H2+ 也可以顺便上 Mica；
- 渲染不再是纯 GDI+，成本 +1–2 周；
- 如果 A/B 都不行，这是可靠退路。

**判定标准**：Win10 22H2 干净 VM，浅色/深色各一张截图；材质可见、文字可读、无黑块/白块、拖动不严重闪烁；关闭透明效果后自动 Solid。

---

## 5. 控件与交互

### 5.1 布局：复用 `ControlSpec`

`gui_ui.cpp` 的 42 条 `ControlSpec` 已经是布局与 Tab 顺序的单一事实来源。不要另写一套坐标。

映射：

| `klass` / style | Widget |
|---|---|
| `STATIC` | `Label`；`SS_RIGHT` 右对齐，`SS_ENDELLIPSIS` 省略 |
| `BUTTON` + `BS_AUTOCHECKBOX` | `Checkbox` |
| `BUTTON` | `Button`；`BS_DEFPUSHBUTTON` 为主按钮/默认 |
| `EDIT` + `ES_READONLY` | `TextView`（只读、可滚动、Ctrl+A/C） |
| `PROGRESS_CLASSW` | `ProgressBar` |
| `WC_LISTVIEWW` | `Table`（4 列、选择、滚动） |

```cpp
std::unique_ptr<Widget> make_widget(const ControlSpec& spec, const Strings& strings);
```

- `ControlSpec.group` 决定页面可见性（Shell 永远显示）。
- `WS_TABSTOP` 决定是否进入焦点链；顺序 = `control_layout()` 顺序。
- `ES_MULTILINE`、`WS_VSCROLL`、`LVS_REPORT` 等只用来选择 widget 行为，不再创建真实控件。
- 新增控件仍然只改 `gui_ui.cpp` + `make_widget` 映射。

### 5.2 Widget 集

| Widget | 行为 |
|---|---|
| `Label` | 单行/多行、左/右对齐、省略号；不参与焦点 |
| `Button` | hover/pressed/disabled/focus、默认按钮、Enter/Space 触发、primary/secondary 样式 |
| `Checkbox` | checked/unchecked、Space 切换、焦点框、多行标签、禁用态 |
| `ProgressBar` | 确定性百分比；可选 marquee；保留现有 `WS_TABSTOP` 行为，UIA 暴露百分比 |
| `TextView` | 只读；滚动（滚轮/滚动条/PageUp/Down）；Ctrl+A/C 复制全文；焦点框；UIA value |
| `Table` | 表头 + 行 + 列；单选；滚动；方向键/Home/End；行高亮；UIA 行 |
| `ScrollBar` | 竖向；`TextView`/`Table` 共用；可拖动/点击轨道 |
| `Tooltip` | 可选；先不做，长文本用 TextView/省略号 |

不做：文本输入、富文本、语法高亮、拖放重排、排序、列宽拖动、右键菜单（第一版）。

### 5.3 焦点与键盘

`FocusManager` 维护可见、启用且 `tab_stop` 的 widget 列表（按 `ControlSpec` 顺序）。

| 键 | 行为 |
|---|---|
| Tab / Shift+Tab | 下一个/上一个；循环 |
| Enter | 默认按钮（`BS_DEFPUSHBUTTON`）；TextView 不吞 Enter |
| Esc | 关闭/取消，走 `Wizard::on_close` 的现有语义 |
| Space | 按钮/复选框激活 |
| 方向键 | Table 行移动；TextView 滚动；其他忽略 |
| Home/End/PageUp/PageDown | TextView/Table 滚动 |
| Ctrl+A / Ctrl+C | TextView 全选/复制全文 |
| Alt+字母 | 第一版不做助记键；字符串表里的 `&` 先按普通字符处理 |

鼠标：

- `WM_MOUSEMOVE` 做 hover；`TrackMouseEvent` 处理 `WM_MOUSELEAVE`；
- `WM_LBUTTONDOWN` 设焦点 + capture；`WM_LBUTTONUP` 在按钮内触发 click；
- `WM_MOUSEWHEEL` 给 `TextView`/`Table`；
- 禁用态不响应；焦点框用 1px 系统色/强调色。

### 5.4 TextView 与 Table 的范围

**TextView（MVP）**：

- 只读；自动换行；滚动；
- Ctrl+A 选择全部、Ctrl+C 复制全部；不做部分选择的鼠标拖选（后续可加）；
- 滚动条自绘；
- UIA：`ROLE_SYSTEM_TEXT`，`get_accValue` 返回全文；`STATE_SYSTEM_READONLY`；
- 同意页证书指纹必须可复制，这是 §8 的硬要求。

**Table（MVP）**：

- 4 列：名称/版本/证书 SHA-1/状态；列宽从现有 `LVM_INSERTCOLUMNW` 的 236/84/250/100 dp 迁移；
- 表头、行、选择、滚动；行高 26–28 dp；
- 方向键/Home/End；单击选中；双击不做事（第一版）；
- UIA：`ROLE_SYSTEM_LIST`，每行 `ROLE_SYSTEM_LISTITEM`，name 为“名称, 版本, 证书, 状态”；
- 选中行变化时 `NotifyWinEvent(EVENT_OBJECT_SELECTION)`；`get_accSelection` 返回当前行。

### 5.5 主题与动画

`Theme` 只放 token，不放控件逻辑：

```cpp
struct Theme {
    Color text, text_secondary, text_disabled;
    Color accent, on_accent;
    Color card, card_border;
    Color control, control_border, control_hover, control_pressed;
    Color window_solid;
    float radius_card = 12.0f;
    float radius_control = 6.0f;
    TextStyle title, heading, body, caption, button;
};
```

- Light/Dark/HighContrast 三套；强调色从 `DwmGetColorizationColor` 或注册表读，不硬编码。
- 字体：`Microsoft YaHei UI`；标题 600 字重；正文 14/12；行高 20/18。
- 动画：只做进度条 marquee、页面淡入（150–200ms）；读 `SPI_GETCLIENTAREAANIMATION`，关闭时直接跳变。
- 不用 60fps 常驻 timer；不可见/最小化时停。

---

## 6. 无障碍（UIA/MSAA）

### 6.1 为什么不能省

`docs/handoff.md` 已经记录：现有标准控件在 UIA 客户端里被报成 `ControlType.Pane`，名字/enabled 正确；这可能是 MSAA→UIA 桥的问题。全自绘后如果什么都不做，整窗口就是一个 Pane，Narrator 读不到任何控件，属于明显回归。

### 6.2 方案：先 MSAA `IAccessible`

对五页安装器，MSAA 足够覆盖按钮/复选框/文本/列表；UIA 客户端会通过 MSAA 桥接。完整 UIA provider（`IRawElementProviderFragment`/`ITableProvider`）可以后续再加。

`Host` 实现根 `IAccessible`，每个 `Widget` 实现一个子 `IAccessible`：

```cpp
LRESULT Host::on_get_object(WPARAM wparam, LPARAM lparam) {
    if (lparam == OBJID_CLIENT) {
        return LresultFromObject(IID_IAccessible, wparam, accessibility_.Get());
    }
    return DefWindowProcW(hwnd_, WM_GETOBJECT, wparam, lparam);
}
```

必须实现：

| 方法 | 内容 |
|---|---|
| `get_accChildCount` | 可见 widget 数 + Table 行数；Table 行作为子对象 |
| `get_accChild` | 返回子 `IDispatch*`；child id 从 1 开始 |
| `get_accName` | 来自 `Strings` 的标签；按钮/复选框文案 |
| `get_accRole` | `ROLE_SYSTEM_PUSHBUTTON` / `CHECKBUTTON` / `PROGRESSBAR` / `TEXT` / `LIST` / `LISTITEM` / `STATICTEXT` |
| `get_accState` | `FOCUSABLE`/`FOCUSED`/`INVISIBLE`/`UNAVAILABLE`/`CHECKED`/`SELECTED`/`READONLY` |
| `get_accLocation` | 屏幕坐标：`GetWindowRect` + widget bounds |
| `get_accValue` | TextView 全文；Table 当前行文本；ProgressBar 百分比 |
| `get_accDefaultAction` | 按钮“按下”；复选框“切换” |
| `accDoDefaultAction` | 走 `Wizard` 的动作回调 |
| `accNavigate` / `get_accFocus` / `get_accSelection` / `accSelect` | 焦点/表格选择 |
| `get_accKeyboardShortcut` | 可选；第一版不做 |

事件：

- 焦点变化：`NotifyWinEvent(EVENT_OBJECT_FOCUS, hwnd, OBJID_CLIENT, child_id)`；
- 复选框：`EVENT_OBJECT_STATECHANGE`；
- 进度/百分比：`EVENT_OBJECT_VALUECHANGE`；
- 文本变化：`EVENT_OBJECT_NAMECHANGE`；
- 表格选择：`EVENT_OBJECT_SELECTION`。

### 6.3 键盘与高对比度

- 键盘矩阵必须完整：Tab/Shift+Tab/Enter/Esc/Space/方向键/Home/End/PageUp/PageDown/Ctrl+A/C。
- 焦点框不能只靠颜色：1px 实线 + 2px 内缩，深色/浅色/HC 都可见。
- HC：不启用 Mica/Acrylic；用系统 `COLOR_WINDOW`/`COLOR_WINDOWTEXT`/`COLOR_HIGHLIGHT`；卡片不透明；边框可见。
- 读屏：Narrator 至少能读完同意页的证书详情、勾选“我了解并同意”、Tab 到“下一步”、读出表格行。
- 验收工具：Accessibility Insights for Windows、Narrator、`winapp ui`（如果环境可用）。

---

## 7. 与现有代码集成

### 7.1 保留什么

- `Wizard` 的公开方法：`create/show/show_uninstall/adopt_container/exit_code`；
- `Wizard` 的状态机与动作：`begin_preview/begin_install/submit_consent/begin_manage_load/begin_uninstall/cancel_install/open_log_file/...`；
- `gui_worker.cpp` 全部；`SharedState`、`PackageView`、`InstallView`、`ManageView`、`ConsentAnswers`；
- `gui_ui.cpp` 的 `control_layout()`；`gui_strings.cpp` 的字符串表；资源/命令行/单实例/提权；
- 退出码、`WM_COPYDATA`、ARP 卸载入口。

构建新增：MSAA 的 `LresultFromObject` 需要 `oleacc.lib`（当前 `gui.cmake` 还没有）；`gdiplus`/`dwmapi` 已有。只有将来改用完整 UIA provider 时才需要 `UIAutomationCore.lib`。

### 7.2 替换什么

`gui_wizard.cpp` 里这些标准控件相关实现逐步替换：

| 现在 | 之后 |
|---|---|
| `create_control` + 42 个 HWND | `WizardView` 建 42 个 Widget（无 HWND） |
| `set_text` / `ShowWindow` / `EnableWindow` | `Widget::text/visible/enabled` + `Host::invalidate` |
| `on_ctl_color` / `NM_CUSTOMDRAW` | `Theme` + `Widget::paint` |
| `repaint_behind` | 不存在（整帧重画，无子控件残影） |
| `IsDialogMessageW` | `FocusManager` + `Host::on_key` |
| `ListView`/`EDIT` 消息 | `Table`/`TextView` 自己的模型 |
| `WM_PAINT` 空实现 | `Host::paint` 画整帧 |

### 7.3 `WizardView` 绑定

```cpp
class WizardView {
public:
    explicit WizardView(Wizard& wizard, Host& host, const Strings& strings);

    void show_page(WizardPage page);
    void render_summary(const PackageView&);
    void render_consent(const PackageView&);
    void render_progress(uint32_t percent, PhaseView, const std::vector<std::wstring>& log);
    void render_result(const InstallView&);
    void render_manage(const ManageView&);

    void set_text(ControlId id, std::wstring_view);
    void set_visible(ControlId id, bool);
    void set_enabled(ControlId id, bool);
    bool is_checked(ControlId id) const;
    int selected_row() const;

    Widget* find(ControlId id);
};
```

- `Wizard::render_*` 改为从 `SharedState` 读数据，调用 `view_->render_*`；UI 线程规则不变。
- 按钮回调直接调用 `Wizard` 的现有动作；不要在新框架里复制业务逻辑。
- `WM_COPYDATA`、`WM_CLOSE`、提权前台/闪烁等仍留在 `Wizard::handle`。
- `Host` 只处理窗口级消息：`WM_PAINT`、鼠标/键盘、`WM_DPICHANGED`、`WM_GETOBJECT`、`WM_TIMER`。

### 7.4 字符串与布局

- `Strings::get` 返回的文案直接喂给 Widget；`Label`/`Button`/`Checkbox` 用 `MeasureString` 做省略/换行。
- 中文/英文长度差异大：按钮宽度沿用现有 dp，但文本省略；不要写死“中文刚好”。
- 同意页的证书详情是长文本：`TextView` 负责换行/滚动；指纹复制走 Ctrl+A/C。
- 现有 `ControlSpec` 的 `SS_ENDELLIPSIS` 映射到 `Label::ellipsis`；`BS_MULTILINE` 映射到 `Checkbox::wrap`。

---

## 8. 实施计划

### 8.1 Spike（3–5 天，go/no-go）

目标：证明“GDI+ 整帧 + 材质 + 文字 + 双缓冲”可行，再写五页。

| 天 | 内容 | 通过标准 |
|---|---|---|
| 1 | Win11 26100：Host + Frame + `SourceCopy` + Mica；画一块圆角卡片 + 中文标题 | 材质可见、文字可读、无黑块 |
| 2 | 同一份代码在 Win10 22H2 VM 用 Acrylic + glass frame | 材质可见；失败则试 layered |
| 3 | 双缓冲 + hover/pressed + 键盘 Tab/Enter | 无闪烁；焦点正确 |
| 4 | UIA：`WM_GETOBJECT` + 一个按钮/一个 TextView | Accessibility Insights 能看到角色/名字 |
| 5 | 150%/200% DPI、浅色/深色、HC | 不重叠、不裁字；HC 自动 Solid |

如果第 2 天 Win10 失败：

- 先试 layered presenter（1 天）；
- 仍失败则本阶段只承诺 Win11 26100+ Mica，Win10 用 Solid/自绘材质；D2D+DComp 另立 spike。

### 8.2 分阶段

| 阶段 | 内容 | 预估 |
|---|---|---|
| 0 | Spike（上表） | 3–5 天 |
| 1 | 框架核心：Canvas/Frame/Widget/Host/Focus/Theme/Solid | 1 周 |
| 2 | Summary 页：Label/Button/导航，接 `PackageView` | 3–4 天 |
| 3 | Consent/Progress/Result：TextView/Checkbox/ProgressBar | 1.5–2 周 |
| 4 | Manage：Table/ScrollBar/选择/动作 | 1–1.5 周 |
| 5 | 材质收尾：Mica/Acrylic/Solid、RDP/HC/透明度检测 | 3–5 天 |
| 6 | UIA/MSAA、键盘矩阵、DPI、截图回归、性能 | 1 周 |

合计 **4–6 周**（一人；含测试，不含视觉设计）。你 + AI 编码可以压到 3–5 周，但 Win10/11 双系统验证、Narrator、DPI 回归仍要真机时间。

迁移期建议加一个 CMake option `MSIXPKG_GUI_SELFDRAWN`：旧 GUI 与新 `WizardView` 都保留，默认仍走旧实现；新实现逐页对齐截图、键盘、UIA 后再切默认。这样任何一页回归都能单独回退，不需要 revert 整个框架。

### 8.3 代码量预估

| 模块 | 行数 |
|---|---|
| Canvas/Frame/Presenter | 600–900 |
| Widget/Host/Focus | 500–800 |
| Controls（Label/Button/Checkbox/Progress/TextView/Table/ScrollBar） | 900–1,400 |
| WizardView 五页 | 700–1,100 |
| UIA/MSAA | 600–1,000 |
| Theme/DPI | 300–500 |
| **合计** | **3,600–5,700** |

这符合“小型框架”，但已经不是“几千行换个皮肤”。

---

## 9. 测试与验收

### 9.1 自动化

- `ui` 单元测试：布局（dp→px）、命中测试、Tab 顺序、焦点循环、文本测量、表格选择模型；
- 截图回归：5 页 × 3 主题（Light/Dark/HC）× 3 DPI（100/150/200），共 45 张；允许像素阈值；
- 行为测试：安装/取消/卸载/修复/打开日志/启动应用/复制详情；退出码 0–7 不变；
- 现有基线：63 原生测试、132 托管测试、33 负例 E2E、46 NSIS E2E 全绿。

### 9.2 人工

- Win10 22H2、Win11 22H2/23H2/24H2 各一台干净 VM；
- 浅色/深色/HC；透明效果开/关；RDP；150%/200% + 双屏混合 DPI；
- Narrator 走完同意页；Accessibility Insights 检查角色/名字/状态；
- 键盘全矩阵：Tab/Shift+Tab/Enter/Esc/Space/方向键/Home/End/PageUp/PageDown/Ctrl+A/C；
- 材质失败时 Solid 不黑屏、不白底白字。

### 9.3 性能目标（记录基线，不作硬门槛）

- 冷启动：≤ 250 ms（当前 76 ms；全自绘会增加 GDI+ 初始化）；
- 工作集：≤ 60 MB（当前 17 MB；GDI+ 帧/字体/缓存会增加）；
- 720×480 全量重绘：≤ 16 ms；
- 进度更新：10–20 Hz，不卡 UI；不可见时无 timer。

---

## 10. 风险

| 风险 | 影响 | 缓解 |
|---|---|---|
| **Win10 Acrylic 的 alpha 合成不成立** | 高：Win10 没有材质 | Spike 先验证；失败走 layered / D2D+DComp / Solid |
| GDI+ 文字在 alpha 上发虚/彩边 | 中：观感下降 | 灰度 AA；或文字用 GDI ClearType、GDI+ 只画形状 |
| UIA/MSAA 回归 | 高：Narrator 读不到 | 第一个 widget 就做 accessibility；Accessibility Insights 验收 |
| `SetWindowCompositionAttribute` 未文档化 | 中：系统更新失效 | 动态加载；失败回退；隔离在一个文件 |
| Layered window 性能/无阴影 | 中：观感/流畅度 | 只作 Win10 备选；静态页面；自绘阴影 |
| DPI/多屏裁字/重叠 | 中 | 全矩阵截图；布局只用 DIP；文本测量留余量 |
| 范围膨胀成通用框架 | 高：工期失控 | 明确非目标：无数据绑定/多窗口/富文本/IME/约束布局 |
| 五页行为回归 | 高：安装器可靠性 | 保留 `Wizard` 状态机/worker；逐页替换；现有 E2E 全跑 |

---

## 11. 开放问题

1. **目标 OS**：只 Win11 24H2+，还是 Win10 2004+ / Win11 22H2+？这决定 GDI+（Mica 26100+ + Acrylic）还是 D2D+DComp。
2. **Win10 是否必须真 Acrylic**？如果 Solid/自绘材质可接受，可以完全绕开未文档化 API。
3. **UIA 深度**：MSAA 够不够，还是必须完整 UIA provider（尤其表格）？
4. **自定义标题栏**是否在范围内？建议第一版保留标准 caption（Mica/亚克力已经覆盖标题栏），自定义 chrome 另做。
5. 这个框架是否要复用到别的工具？如果要，布局以后再加 `Stack/Grid`；第一版仍用绝对坐标。

---

## 附录 A：代码草图

### A.1 Acrylic

```cpp
constexpr DWORD kWcaAccentPolicy = 19;
constexpr DWORD kAccentEnableAcrylicBlurBehind = 4;

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

bool apply_acrylic(HWND hwnd, DWORD tint) {
    static auto set_attribute = reinterpret_cast<BOOL(WINAPI*)(HWND, WindowCompositionAttributeData*)>(
        ::GetProcAddress(::GetModuleHandleW(L"user32.dll"), "SetWindowCompositionAttribute"));
    if (set_attribute == nullptr) return false;

    AccentPolicy policy{kAccentEnableAcrylicBlurBehind, 2, tint, 0};
    WindowCompositionAttributeData data{kWcaAccentPolicy, &policy, sizeof(policy)};
    return set_attribute(hwnd, &data) != FALSE;
}
```

### A.2 Mica

```cpp
bool apply_mica(HWND hwnd) {
    DWORD backdrop = 2;  // DWMSBT_MAINWINDOW
    if (FAILED(::DwmSetWindowAttribute(hwnd, 38, &backdrop, sizeof(backdrop)))) return false;

    BOOL alpha = TRUE;
    if (FAILED(::DwmSetWindowAttribute(hwnd, 39, &alpha, sizeof(alpha)))) return false;

    MARGINS margins{-1, -1, -1, -1};
    return SUCCEEDED(::DwmExtendFrameIntoClientArea(hwnd, &margins));
}
```

### A.3 Present

```cpp
void GdiPresenter::present(const Frame& frame, HDC dc) {
    Gdiplus::Graphics screen(dc);
    screen.SetCompositingMode(Gdiplus::CompositingModeSourceCopy);
    screen.SetInterpolationMode(Gdiplus::InterpolationModeNearestNeighbor);
    screen.DrawImage(frame.bitmap(), 0, 0);
}
```

### A.4 UIA 根对象

```cpp
LRESULT Host::on_get_object(WPARAM wparam, LPARAM lparam) {
    if (lparam == OBJID_CLIENT) {
        return ::LresultFromObject(IID_IAccessible, wparam, accessibility_.Get());
    }
    return ::DefWindowProcW(hwnd_, WM_GETOBJECT, wparam, lparam);
}
```

---

## 附录 B：参考

- `DwmSetWindowAttribute`：<https://learn.microsoft.com/en-us/windows/win32/api/dwmapi/nf-dwmapi-dwmsetwindowattribute>
- `DwmExtendFrameIntoClientArea`：<https://learn.microsoft.com/en-us/windows/win32/api/dwmapi/nf-dwmapi-dwmextendframeintoclientarea>
- `IAccessible`：<https://learn.microsoft.com/en-us/windows/win32/accessibility/iaccessible>
- UI Automation Providers：<https://learn.microsoft.com/en-us/windows/win32/winauto/ui-automation-providers>
- GDI+：<https://learn.microsoft.com/en-us/windows/win32/gdiplus/-gdiplus-gdi-start>
- 现有仓库：`docs/installer-design.md` §8、`docs/wpf-ui-feasibility.md` §10、`docs/handoff.md`
- `SetWindowCompositionAttribute` 没有官方文档；使用前按 §4.5 在 Win10 VM 验证，并保留 Solid 回退。
