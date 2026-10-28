# 自绘 UI 框架（`gui/ui`）

> 状态：框架核心 + demo 可用（`feature/self-drawn-ui`，2026-10-29）
> 设计依据：[`docs/self-drawn-ui-framework.md`](../../../../docs/self-drawn-ui-framework.md)

一个**小型 retained-mode**、单 HWND、GDI+ 整帧渲染的 Win32 UI 框架。它不是通用
UI 库：固定尺寸、绝对布局、无数据绑定、无文本输入。目标是把安装器现有的 42 个
标准控件换成一套统一的绘制与焦点/无障碍模型，同时保留 Mica/Acrylic/Solid 三种
背景材质。

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
```

demo 参数：

| 参数 | 说明 |
|---|---|
| `--theme light\|dark\|hc` | 强制外观；缺省 `auto`（跟随系统） |
| `--material auto\|solid\|mica\|acrylic` | 强制材质；`auto` 依次尝试 Mica → Acrylic → Solid |
| `--report <path>` | 把主题/材质/DPI 与 **MSAA 子对象清单**写到文件后继续运行 |
| `--auto` | 约 0.7s 后自动开始演示安装（用于截图） |
| `--exit-after <ms>` | 到时间自动关窗 |

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

## 分层

```
Widget 树（Label/Button/Checkbox/ProgressBar/TextView/Table）
        │ 绝对 DIP 布局；paint(Canvas&)
        ▼
Host ── Frame(32bppPABGR DIB) ── Presenter
  │        │  clear_transparent()      ├─ GdiPresenter（默认）
  │        │  Canvas(GDI+)             └─ LayeredPresenter（opt-in）
  │        └─ Material（Mica/Acrylic/Solid）
  ├─ 焦点 / Tab / Enter / Esc / 鼠标 / 滚轮
  └─ MSAA IAccessible（根 + 子对象，Table 行也展开）
```

| 文件 | 职责 |
|---|---|
| `canvas.*` | GDI+ 绘制动词、`Frame` DIB、字体/格式缓存、测量、裁剪 |
| `text_layout.*` | 换行算法（CJK 逐字、拉丁按词、超长词硬拆、`\n`/`\r\n`） |
| `theme.*` | Light/Dark/HC token、系统强调色、材质可见性影响卡片透明度 |
| `widget.*` | `Widget` / `Panel` / 命中测试 / 焦点态 / 无障碍默认值 |
| `controls.*` | Label/Button/Checkbox/ProgressBar/TextView/Table + 共用滚动条几何 |
| `host.*` | 单 HWND、消息循环、双缓冲、焦点链、键盘、DPI、`WM_GETOBJECT` |
| `material.*` | Mica（属性 38/39）、Acrylic（未文档化 API 隔离）、Solid 回退 |
| `presenter.*` | `SourceCopy` 上屏；`UpdateLayeredWindow` 备选（需无边框） |
| `automation.*` | `IAccessible` 根/子对象、角色/状态/名字/值/焦点/选择/命中测试 |
| `demo/*` | 760×620 的演示程序，覆盖全部控件与主题/材质切换 |

## 已实现

- **Frame**：`CreateDIBSection` + GDI+ `Bitmap` 包裹同一块像素；`clear_transparent()`
  用 `SourceCopy` 写 alpha=0，Mica/Acrylic 从底下透出。同一块 DIB 也被
  `LayeredPresenter` 直接交给 `UpdateLayeredWindow`。
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
- **主题**：Light/Dark/HighContrast；强调色读 `HKCU\...\DWM\AccentColor`；材质是否
  可见决定卡片是半透明还是实色（Solid 下半透明会发灰）。
- **DPI**：`WM_DPICHANGED` 更新 dpi、重建 Frame、按建议矩形重排；布局只存 DIP。

## 验证记录（本机 Windows 11 26H2 / Build 26300，96 DPI）

- Debug 与 Release（`/WX`）构建零警告；`msixpkg_ui_tests` **31 项全过**
  （换行、滚动条几何、颜色、命中测试）。
- demo `--report`：MSAA `childCount=34`，role/state/name 正确；焦点、选择事件可用。
- 交互实测：单击「切换到深色」→ 内容切浅色；Tab → TextView 焦点框；Enter →
  默认按钮启动模拟安装，进度/阶段文案/禁用态正常（见进度截图）。
- 材质：Mica（浅/深）、Acrylic（深）、Solid（浅/高对比度）均能上屏；
  `MSIXPKG_UI_PRESENTER=layered` 可用（窗口自动切无边框）。
- 高对比度下自动 Solid、禁用材质、卡片不透明、进度条边框可见。

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

## 接入安装器（下一步）

1. 把 `MSIXPKG_UI_SOURCES` 里的 9 个文件加进 `gui/gui.cmake`，或从 Native 的
   `CMakeLists.txt` `add_subdirectory(gui/ui)`；链接 `gdiplus dwmapi oleacc
   oleaut32 ole32 advapi32`。
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
- GDI+ 在预乘 alpha 表面上用灰度 AA（`TextRenderingHintAntiAliasGridFit`）；
  不要开 ClearType，透明边缘会出彩边。
- 只有 `SourceCopy` 能把 alpha 写进重定向位图；`AlphaBlend` 会保留目标 alpha。
