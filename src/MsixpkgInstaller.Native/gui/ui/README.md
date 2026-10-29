# 自绘 UI 框架（`gui/ui`）

> 状态：框架核心 + demo 可用（`feature/self-drawn-ui`，2026-10-29）
> 设计依据：[`docs/self-drawn-ui-framework.md`](../../../../docs/self-drawn-ui-framework.md)

一个**小型 retained-mode**、单 HWND 的 Win32 UI 框架。默认用 D2D 把形状和文字直接
画进 DirectComposition 交换链（GPU），D2D 不可用时退回 GDI+ 整帧 DIB；两者共用同一
套 Widget / 布局 / 焦点 / MSAA 模型。它不是通用 UI 库：固定尺寸、绝对布局、无数据
绑定、无文本输入。目标是把安装器现有的 42 个标准控件换成一套统一的绘制与焦点/
无障碍模型，同时保留 Mica/Acrylic/Solid 三种背景材质。

## 快速开始

```powershell
# 独立构建（不依赖 msixpkg_core / WinUI）
cmake -S src/MsixpkgInstaller.Native/gui/ui -B artifacts/ui-demo-build `
      -G "Visual Studio 18 2026" -A x64
cmake --build artifacts/ui-demo-build --config Debug

# 运行 demo：默认跟随系统主题
artifacts/ui-demo-build/Debug/msixpkg-ui-demo.exe

# 指定主题 / 材质
artifacts/ui-demo-build/Debug/msixpkg-ui-demo.exe --theme light --material solid
artifacts/ui-demo-build/Debug/msixpkg-ui-demo.exe --theme dark  --material mica

# 指定呈现后端（默认 gpu，失败自动退回 gdi）
artifacts/ui-demo-build/Debug/msixpkg-ui-demo.exe --presenter gpu
artifacts/ui-demo-build/Debug/msixpkg-ui-demo.exe --presenter gdi
artifacts/ui-demo-build/Debug/msixpkg-ui-demo.exe --presenter layered
```

demo 参数：

| 参数 | 说明 |
|---|---|
| `--theme light\|dark\|hc` | 强制外观；缺省 `auto`（跟随系统） |
| `--material auto\|solid\|mica\|acrylic` | 强制材质；`auto` 依次尝试 Mica → Acrylic → Solid |
| `--presenter gpu\|gdi\|layered` | 强制呈现后端；`gpu` 失败自动退 `gdi`（等价于环境变量 `MSIXPKG_UI_PRESENTER`） |
| `--report <path>` | 把主题/材质/DPI、**MSAA 子对象清单**与帧统计写到文件后继续运行 |
| `--auto` | 约 0.7s 后自动开始演示安装（用于截图/基准） |
| `--exit-after <ms>` | 到时间自动关窗；退出前再写一次 `--report`（含最终帧统计） |

`--report` 走的是真实 `AccessibleObjectFromWindow(hwnd, OBJID_CLIENT)` 路径，
即 `WM_GETOBJECT` + `LresultFromObject` + MSAA 桥，不是内部直读。

单测（不需要窗口）：

```powershell
cmake --build artifacts/ui-demo-build --config Debug
artifacts/ui-demo-build/Debug/msixpkg_ui_tests.exe
# 或：ctest --test-dir artifacts/ui-demo-build -C Debug --output-on-failure
```

## 截图

浅色 Mica：

![浅色 Mica](../../../../docs/images/self-drawn-ui/demo-light-mica.png)

深色 Mica：

![深色 Mica](../../../../docs/images/self-drawn-ui/demo-dark-mica.png)

Solid 回退（透明效果不可用时也长这样）：

![Solid](../../../../docs/images/self-drawn-ui/demo-light-solid.png)

安装进度（Enter 触发默认按钮 / `--auto`）：

![进度](../../../../docs/images/self-drawn-ui/demo-dark-progress.png)

高对比度（材质自动关闭）：

![高对比度](../../../../docs/images/self-drawn-ui/demo-high-contrast.png)

文字清晰度对比（3× 最近邻放大，同一块「示例包」卡片）：

| 修正前：GDI+ 灰度 AA | 修正后：DirectWrite 灰度 AA |
|---|---|
| ![before](../../../../docs/images/self-drawn-ui/text-before.png) | ![after](../../../../docs/images/self-drawn-ui/text-after.png) |

## 分层

```
Widget 树（Label/Button/Checkbox/ProgressBar/TextView/Table）
        │ 绝对 DIP 布局；paint(Canvas&)
        ▼
Host ── Canvas（同一套绘制动词，两套后端）
  │      ├─ D2D 设备上下文 → 普通纹理 ─CopyResource→ 组合交换链（默认，GPU）
  │      ├─ GDI+ Frame(32bppPABGR DIB) → GdiPresenter（回退，SourceCopy）
  │      └─ GDI+ Frame(DIB) → LayeredPresenter（opt-in，UpdateLayeredWindow）
  ├─ DirectWriteText（灰度 AA；两条后端共用同一份 layout）
  ├─ Material（Mica/Acrylic/Solid）
  ├─ 合成器时钟节拍（180Hz 屏按 3 tick 出一帧 60fps）；空闲不跑帧
  └─ 焦点 / Tab / Enter / Esc / 鼠标 / 滚轮 / MSAA
```

| 文件 | 职责 |
|---|---|
| `canvas.*` | 双后端绘制动词（D2D / GDI+）、`Frame` DIB、字体/格式缓存、测量、裁剪 |
| `dwrite_text.*` | DirectWrite 灰度 AA 文字后端（唯一的绘制 + 度量来源，D2D/DC 两个 target 共用） |
| `text_layout.*` | 换行算法（CJK 逐字、拉丁按词、超长词硬拆、`\n`/`\r\n`） |
| `theme.*` | Light/Dark/HC token、系统强调色、表面颜色 |
| `widget.*` | `Widget` / `Panel` / 命中测试 / 焦点态 / 无障碍默认值 |
| `controls.*` | Label/Button/Checkbox/ProgressBar/TextView/Table + 共用滚动条几何 |
| `host.*` | 单 HWND、合成器节拍、焦点链、键盘、DPI、帧统计、`WM_GETOBJECT` |
| `material.*` | Mica（属性 38/39）、Acrylic（未文档化 API 隔离）、Solid 回退 |
| `presenter.*` | 呈现接口；`SourceCopy` 与 `UpdateLayeredWindow` 两个软件上屏实现 |
| `gpu_presenter.*` | D3D11 + DirectComposition：预乘 alpha 组合交换链、D2D target、vsync 节拍 |
| `automation.*` | `IAccessible` 根/子对象、角色/状态/名字/值/焦点/选择/命中测试 |
| `demo/*` | 760×620 的演示程序，覆盖全部控件与主题/材质切换 |

## 已实现

- **GPU 渲染与呈现（默认）**：形状、图标、文字都画在 `ID2D1DeviceContext` 上；
  D2D 先画到一张普通纹理（组合交换链的 back buffer 不能直接当 D2D target），
  再 `CopyResource` 到 `D3D11 + DirectComposition` 的预乘 alpha 交换链，最后
  `Present(1)` + `Commit`。Mica/Acrylic 继续从透明像素透出来。`--presenter gdi`
  或 `MSIXPKG_UI_PRESENTER=gdi` 可强制走软件路径。
- **合成器节拍**：组合交换链的 `Present` 不做反压，Host 用
  `DCompositionWaitForCompositorClock` 等 DWM 的合成时钟；按测得的周期把
  60fps 折算成「每 N 个 tick 一帧」（本机 180Hz → 每 3 tick 一帧）。
  空闲或安装结束后 `set_animating(false)`，主循环回到纯消息等待。
- **Frame（回退路径）**：`CreateDIBSection` + GDI+ `Bitmap` 包裹同一块像素；
  `clear_transparent()` 用 `SourceCopy` 写 alpha=0，Mica/Acrylic 从底下透出。
  同一块 DIB 也被 `LayeredPresenter` 直接交给 `UpdateLayeredWindow`。
- **材质**：`Auto` 在 HC / 远程桌面 / 关闭透明效果时直接 Solid；否则尝试
  Mica（属性 38 + 39 + 扩展边框，26100+），失败退 Acrylic（动态加载
  `SetWindowCompositionAttribute`），再失败退 Solid。任一步失败都不会黑屏/白屏。
- **控件**：Label、Button（primary/default/icon/hover/pressed/disabled）、Checkbox、
  ProgressBar（确定值 + marquee）、TextView（只读、换行、滚动、Ctrl+A/C、UIA value）、
  Table（4 列、单选、滚动、方向键/Home/End/PageUp/PageDown、行高亮、行级 MSAA）。
- **焦点/键盘**：Tab/Shift+Tab 循环（顺序 = Widget 创建顺序，对应 `ControlSpec`）、
  Enter 触发默认按钮、Esc、Space、方向键、Home/End/PageUp/PageDown、Ctrl+A/C。
  焦点框为 1px 强调色 + 2px 内缩。
- **MSAA**：根对象 + 可见 Widget + Table 行；`get_accName/Role/State/Value/
  DefaultAction/accLocation/accHitTest/accSelect/accNavigate/get_accFocus/
  get_accSelection/accDoDefaultAction`。事件：焦点、状态变化、值变化、表格选择。
- **主题**：Light/Dark/HighContrast；强调色读 `HKCU\...\DWM\AccentColor`；材质可见时
  卡片/控件是半透明表面，材质从底下透出。
- **DPI**：`WM_DPICHANGED` 更新 dpi、重建 Frame、按建议矩形重排；布局只存 DIP。
- **文字渲染（2026-10-29 修正）**：
  - 文字统一由 DirectWrite 灰度 AA 绘制（`dwrite_text.*`），透明/半透明表面也能写
    正确的预乘 alpha，不需要 ClearType 的 alpha 补丁。
  - D2D 硬件路径直接画在设备上下文上；GDI+ 回退路径把它绑到 DIB 的
    `ID2D1DCRenderTarget`。两条路径使用同一份 `IDWriteTextLayout`。
  - 测量与绘制共用同一套 layout：标题、正文、按钮、表格的基线与缩放一致，不会再
    出现「透明标题和卡片正文差半个像素」。
  - 之前试过 GDI ClearType + GDI+ 灰度双后端：透明标题与卡片正文来自两套排版，
    位置和字形不一致，已移除。
  - GDI+ 文字只保留为 DWrite 初始化失败时的兜底（`Canvas::draw_text_immediate`）。

## 验证记录（本机 Windows 11 26H2 / Build 26300，96 DPI）

- Debug 与 Release（`/WX`）构建零警告；`msixpkg_ui_tests` **31 项全过**
  （换行、滚动条几何、颜色、命中测试）。
- demo `--report`：MSAA `childCount=34`，role/state/name 正确；焦点、选择事件可用。
- 交互实测：单击「切换到深色」→ 内容切浅色；Tab → TextView 焦点框；Enter →
  默认按钮启动模拟安装，进度/阶段文案/禁用态正常（见进度截图）。
- 材质：Mica（浅/深）、Acrylic（深）、Solid（浅/高对比度）均能上屏；
  `MSIXPKG_UI_PRESENTER=layered` 可用（窗口自动切无边框）。
- 高对比度下自动 Solid、禁用材质、进度条边框可见。
- 文字：DirectWrite 灰度 AA 单一后端；透明标题与卡片正文的基线/字形一致。
  放大对比见上面的 `text-before/after`（旧 GDI+ 灰度 → 新 DirectWrite 灰度）。
- **帧率/CPU（Release，760×620，浅色 Mica，安装进度动画 3.5s）**：

  | 路径 | 平均间隔 | p95 | 丢帧 | render | present | CPU（单核） |
  |---|---|---|---|---|---|---|
  | D2D + DComp + 合成器节拍 | 16.3–16.6 ms（61 fps） | 16.8 ms | 0 | 0.97–1.65 ms | 0.58–1.02 ms | 15–21% |
  | GDI+ DIB + SourceCopy | 25–38 ms（27–40 fps） | 32–107 ms | 58–72 | 16–27 ms | 4.8–8.4 ms | 67–77% |

  空闲（没有动画）时两条路径都是 0–1%：`set_animating(false)` 后主循环只等消息。
  表中是本机（RTX 4060 Ti / 180 Hz）实测，数值随系统负载浮动；paint 从 ~14 ms
  降到 <0.2 ms 是 D2D 硬件路径的主要收益。

## 已知限制

1. **标题栏亮暗**：本机（26H2）系统为深色时，`DWMWA_USE_IMMERSIVE_DARK_MODE
   = FALSE`（20 与 19 都试过）不再把 caption 强制变亮。`Auto` 模式跟随系统所以
   一致；demo 的强制 `--theme light` 会出现「浅色内容 + 深色标题栏」。逐窗强制
   亮色标题栏需要自定义 chrome，本阶段按设计文档 §11 的范围不做。
2. **DPI**：代码路径完整（PerMonitorV2 + `WM_DPICHANGED`），但只在 96 DPI 真机
   验证过；150%/200% 需要按设计文档 §8.1 第 5 天做截图矩阵。
3. **Win10 Acrylic**：本机是 Win11，未在 Win10 22H2 干净 VM 验证「GLASS Frame +
   GDI+ alpha」是否成立；失败时已有 Solid 与 Layered 两条回退。
4. **五页接入未做**：本分支交付框架 + demo，`WizardView` 与 `Wizard` 的 42 个
   `ControlSpec` 映射还未接（见下节）。
5. TextView 只读；只支持 Ctrl+A 全选 + Ctrl+C 复制全文，没有鼠标拖选。无
   IME/文本输入（安装器不需要）。
6. Table 列宽固定；无排序、列宽拖动、双击。
7. Layered 模式没有系统 caption/shadow，顶部 40dp 用于拖动；它是 Win10 的
   备选路径，不是默认。
8. **全框架使用灰度 AA，不追求 ClearType**：这是为「透明材质 + 单一文字后端 +
   位置一致」做的取舍。一定要 ClearType 就得回到不透明表面 + 第二套后端，代价
   正是之前那种标题/正文位置和字形不一致。
9. **硬件路径依赖 D3D11 + DirectComposition**：`CreateSwapChainForComposition`
   或 D2D 设备创建失败时自动退回 GDI+，窗口不会黑；但 GDI+ 回退是整帧软件光栅
   （本机 27–40 fps），只适合无 GPU/远程桌面兜底。
10. **多显示器 / VRR 未验证**：合成器节拍按窗口所在合成器的时钟走，`tick` 折算
    假设刷新率稳定；180 Hz 下实测 3 tick = 60 fps。
11. **硬件 Canvas 不支持图片**：`draw_image()` 在 D2D 路径是空实现（当前控件集
    不用图片）。GDI 回退仍支持。

## 接入安装器（下一步）

1. 把 `MSIXPKG_UI_SOURCES` 里的 11 个文件加进 `gui/gui.cmake`，或从 Native 的
   `CMakeLists.txt` `add_subdirectory(gui/ui)`；链接 `gdiplus d2d1 dwrite d3d11
   dcomp dxgi dwmapi oleacc oleaut32 ole32 user32 gdi32 advapi32`。
2. 按设计文档 §5.1 写 `make_widget(const ControlSpec&)`：`STATIC`→Label、
   `BS_AUTOCHECKBOX`→Checkbox、`BUTTON`→Button、`ES_READONLY`→TextView、
   `PROGRESS_CLASSW`→ProgressBar、`WC_LISTVIEWW`→Table。
3. `WizardView` 按 `SharedState` 重建五页；`Wizard` 状态机、`gui_worker.cpp`、
   字符串表、退出码、`WM_COPYDATA` 原样保留。按钮回调直接调 `Wizard` 现有动作。
4. 迁移期加 `MSIXPKG_GUI_SELFDRAWN` option，旧 GUI 与新 `WizardView` 并存，逐页
   对齐截图/键盘/UIA 后再切默认。

`HostOptions` 默认 720×480 dp，与现有窗口一致；demo 用 760×620 只是为了把
Table 和细节都塞进一屏。

## 实现时踩到的坑（给后续维护）

- **`WIN32_LEAN_AND_MEAN` + GDI+**：`gdiplus.h` 的 `Image`/`Metafile` 声明需要
  `IStream`/`PROPID`，必须在它之前 `#include <objidl.h>`，否则整片语法错误。
- **`StringFormatFlagsLineLimit`**：当布局矩形高度小于字体行高时，GDI+ 会**整行
  不画**（按钮文字、TextView 行首次实现时全空）。单行绘制不要设置这个标志，
  只靠布局矩形裁剪 + `StringFormatFlagsNoWrap`。
- **`WM_GETOBJECT` 的 LPARAM**：64 位下 `OBJID_CLIENT` 可能被零扩展成
  `0x00000000FFFFFFFC`；用 `static_cast<LONG>(lParam) == OBJID_CLIENT` 比较，
  否则 MSAA 永远走到 `DefWindowProc` 的默认对象（`childCount=0`）。
- **`UpdateLayeredWindow` 的 `psize` 是整窗尺寸**，不是客户区。分层窗口必须去掉
  caption 并按“客户区 == 整窗”设置，否则窗口会被 ULW 重设尺寸、布局被裁。
- **文字统一走 DirectWrite**：GDI+ 只画形状，画完后 `Graphics::Flush(Sync)`，再让
  `ID2D1DCRenderTarget` 绑定帧的 DIB 做 `BeginDraw/DrawTextLayout/EndDraw`。测量与
  绘制必须使用同一套 `IDWriteTextLayout`，否则换行位置和基线会对不上。
- GDI+ 的 ClearType 在预乘 alpha 位图上会出彩边/重影；GDI `DrawTextW` 写 32bpp
  DIB 时又会把字形 alpha 留成 0。这两条老路都试过并移除了。
- 只有 `SourceCopy` 能把 alpha 写进重定向位图；`AlphaBlend` 会保留目标 alpha。
- **`CreateSwapChainForHwnd` 不接受预乘 alpha**：`DXGI_ALPHA_MODE_PREMULTIPLIED`
  直接返回 `DXGI_ERROR_INVALID_CALL`。带 alpha 的交换链只能走
  `CreateSwapChainForComposition` + DirectComposition。
- **组合交换链的 back buffer 不能直接当 D2D target**：`CreateBitmapFromDxgiSurface`
  返回 `E_INVALIDARG`。先画到一张普通 `RENDER_TARGET` 纹理，再 `CopyResource`
  到 back buffer；交换链只负责呈现。
- **`D3D11_USAGE_DYNAMIC` 纹理必须至少有一个 bind flag**：`BindFlags=0` 时
  `CreateTexture2D` 返回 `E_INVALIDARG`；上传用的中转纹理给 `SHADER_RESOURCE`
  即可（不必创建 SRV）。
- **组合交换链的 `Present(1)`/帧延迟句柄不反压**：不会自动按 vsync 阻塞；
  要用 `DCompositionWaitForCompositorClock` 等合成器时钟，否则 UI 线程会以
  上千 fps 空转，基于 `GetTickCount64` 的动画步长也会一直是 0。
- **DComp 窗口的 Mica**：DComp 只覆盖内容，窗口自己的重定向位图仍要清成
  alpha=0（`PatBlt(BLACKNESS)`），否则透明处会露出不透明的旧底色。
