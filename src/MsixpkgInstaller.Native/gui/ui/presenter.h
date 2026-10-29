#pragma once

// 帧 → 窗口的呈现方式（docs/self-drawn-ui-framework.md §3.2 / §4.5）。
//
// GdiPresenter 是兼容路径：普通窗口 + 重定向位图 + GDI+ SourceCopy，不受
// 垂直同步约束，撕裂只能靠人眼看不见；LayeredPresenter 是 Acrylic 的备选；
// GpuPresenter 用 D3D11 flip-model 交换链（预乘 alpha）上屏，Present(1) 由
// vsync 驱动，既没有 BitBlt 撕裂，也把「什么时候该画下一帧」交给合成器。

#include "canvas.h"

#include <Windows.h>
#include <d2d1.h>

#include <memory>

namespace msixpkg::ui {

enum class PresenterKind { Gdi, Gpu, Layered };

class Presenter {
public:
    virtual ~Presenter() = default;
    virtual PresenterKind kind() const = 0;
    virtual const wchar_t* name() const = 0;

    /// 与窗口建立绑定。GDI/layered 不需要额外资源，默认成功。
    virtual bool initialize(HWND hwnd, int width_px, int height_px) {
        (void)hwnd;
        (void)width_px;
        (void)height_px;
        return true;
    }

    /// 客户区像素尺寸变化。交换链必须跟着 ResizeBuffers，否则会被拉伸。
    virtual void resize(int width_px, int height_px) {
        (void)width_px;
        (void)height_px;
    }

    virtual void present(const Frame& frame, HDC dc) = 0;

    /// 非空的 D2D 设备上下文表示这个 presenter 能直接画硬件表面：
    /// Host 用 Canvas(d2d_context) + DirectWriteText(bind) 画完后调 end_frame()，
    /// 不再经过 DIB 和 staging 上传。
    virtual ID2D1DeviceContext* d2d_context() const { return nullptr; }
    /// true：组合交换链不会用 Present(1) 反压，Host 改用合成器时钟节拍。
    virtual bool compositor_paced() const { return false; }
    virtual bool begin_frame(float dpi) {
        (void)dpi;
        return false;
    }
    virtual void end_frame() {}

    /// 非空表示由合成器的帧节拍驱动：Host::run 用这个句柄等下一帧，
    /// 不再依赖 WM_TIMER（默认 15.6ms 粒度，本身就是卡顿来源之一）。
    virtual HANDLE frame_wait_handle() const { return nullptr; }

    /// 设备丢失等不可恢复状态。false 时 Host 会降级到 GDI 呈现。
    virtual bool healthy() const { return true; }
};

std::unique_ptr<Presenter> make_gdi_presenter();
std::unique_ptr<Presenter> make_layered_presenter(HWND hwnd);

/// 失败（没有 D3D11、交换链不接受预乘 alpha、设备丢失）时返回 nullptr，
/// 由调用方退回 GDI。成功返回时已经完成 device/swapchain 的创建。
std::unique_ptr<Presenter> make_gpu_presenter(HWND hwnd, int width_px, int height_px);

}  // namespace msixpkg::ui
