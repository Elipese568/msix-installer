#include "presenter.h"

namespace msixpkg::ui {
namespace {

class GdiPresenter final : public Presenter {
public:
    const wchar_t* name() const override { return L"GDI SourceCopy"; }

    void present(const Frame& frame, HDC dc) override {
        if (dc == nullptr) {
            return;
        }

        // SourceCopy 是关键：整帧（含 alpha）覆盖到窗口重定向位图，
        // 不能只用 AlphaBlend —— 它会保留目标 alpha，透明区就不透了。
        Gdiplus::Graphics screen(dc);
        screen.SetCompositingMode(Gdiplus::CompositingModeSourceCopy);
        screen.SetInterpolationMode(Gdiplus::InterpolationModeNearestNeighbor);
        screen.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
        screen.DrawImage(&frame.bitmap(), 0, 0, frame.width_px(), frame.height_px());
    }
};

class LayeredPresenter final : public Presenter {
public:
    explicit LayeredPresenter(HWND hwnd) : hwnd_(hwnd) {}

    const wchar_t* name() const override { return L"UpdateLayeredWindow"; }

    void present(const Frame& frame, HDC dc) override {
        (void)dc;  // 分层窗口不需要目标 DC，帧数据直接从 DIB 送进 DWM。

        if (hwnd_ == nullptr || frame.memory_dc() == nullptr) {
            return;
        }

        LONG_PTR ex_style = ::GetWindowLongPtrW(hwnd_, GWL_EXSTYLE);
        if ((ex_style & WS_EX_LAYERED) == 0) {
            ::SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, ex_style | WS_EX_LAYERED);
        }

        RECT window_rect{};
        if (::GetWindowRect(hwnd_, &window_rect) == FALSE) {
            return;
        }

        POINT destination{window_rect.left, window_rect.top};
        SIZE size{frame.width_px(), frame.height_px()};
        POINT source{0, 0};
        BLENDFUNCTION blend{};
        blend.BlendOp = AC_SRC_OVER;
        blend.SourceConstantAlpha = 255;
        blend.AlphaFormat = AC_SRC_ALPHA;

        ::UpdateLayeredWindow(hwnd_, nullptr, &destination, &size, frame.memory_dc(), &source, 0, &blend,
                              ULW_ALPHA);
    }

private:
    HWND hwnd_ = nullptr;
};

}  // namespace

std::unique_ptr<Presenter> make_gdi_presenter() {
    return std::make_unique<GdiPresenter>();
}

std::unique_ptr<Presenter> make_layered_presenter(HWND hwnd) {
    return std::make_unique<LayeredPresenter>(hwnd);
}

}  // namespace msixpkg::ui
