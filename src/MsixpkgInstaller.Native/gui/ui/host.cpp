#include "host.h"

#include "automation.h"
#include "presenter.h"

#include <Windows.h>
#include <ole2.h>
#include <oleacc.h>
#include <windowsx.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <utility>

namespace msixpkg::ui {
namespace {

constexpr UINT_PTR kAnimationTimer = 1;
constexpr DWORD kWindowStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;

void collect_entries(Widget& widget, std::vector<Host::AccessibleEntry>& out) {
    if (!widget.visible) {
        return;
    }

    if (widget.accessible_role() != Role::Panel) {
        out.push_back({&widget, -1});
        const int rows = widget.accessible_row_count();
        for (int row = 0; row < rows; ++row) {
            out.push_back({&widget, row});
        }
    }

    if (const auto* children = widget.child_widgets()) {
        for (const auto& child : *children) {
            collect_entries(*child, out);
        }
    }
}

bool layered_requested() {
    wchar_t value[64]{};
    const DWORD length = ::GetEnvironmentVariableW(L"MSIXPKG_UI_PRESENTER", value,
                                                   static_cast<DWORD>(std::size(value)));
    return length > 0 && _wcsicmp(value, L"layered") == 0;
}

}  // namespace

Host::Host() = default;

Host::~Host() {
    if (animating_ && hwnd_ != nullptr) {
        ::KillTimer(hwnd_, kAnimationTimer);
        animating_ = false;
    }

    if (hwnd_ != nullptr) {
        if (::IsWindow(hwnd_) != FALSE) {
            ::DestroyWindow(hwnd_);
        }

        hwnd_ = nullptr;
    }

    accessibility_.reset();
    root_.reset();
    presenter_.reset();
    material_.reset();
    text_engine_.reset();  // D2D target 绑定在 frame 的 DC 上，先解绑
    frame_.reset();

    if (ole_initialized_) {
        ::OleUninitialize();
        ole_initialized_ = false;
    }

    if (gdiplus_ready_) {
        Gdiplus::GdiplusShutdown(gdiplus_token_);
        gdiplus_token_ = 0;
        gdiplus_ready_ = false;
    }
}

bool Host::register_window_class() {
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.style = 0;
    window_class.lpfnWndProc = &Host::window_proc;
    window_class.hInstance = instance_;
    window_class.hIcon = ::LoadIconW(nullptr, IDI_APPLICATION);
    window_class.hIconSm = window_class.hIcon;
    window_class.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    window_class.hbrBackground = nullptr;  // 背景完全由帧决定
    window_class.lpszClassName = options_.window_class.c_str();

    if (::RegisterClassExW(&window_class) == 0) {
        return ::GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }

    return true;
}

bool Host::create(HINSTANCE instance, std::unique_ptr<Widget> root, HostOptions options) {
    if (instance == nullptr || root == nullptr || hwnd_ != nullptr) {
        return false;
    }

    instance_ = instance;
    options_ = std::move(options);

    Gdiplus::GdiplusStartupInput startup{};
    if (Gdiplus::GdiplusStartup(&gdiplus_token_, &startup, nullptr) != Gdiplus::Ok) {
        return false;
    }

    gdiplus_ready_ = true;

    // 文字统一走 DirectWrite 灰度 AA；初始化失败时 Canvas 会退回 GDI+ 灰度。
    text_engine_.initialize();

    // MSAA 服务端（LresultFromObject）要求 UI 线程初始化过 OLE 公寓；
    // 调用方已经初始化时 OleInitialize 返回 S_FALSE，仍然计入引用。
    const HRESULT ole = ::OleInitialize(nullptr);
    ole_initialized_ = SUCCEEDED(ole);

    // 如果主程序没声明 PerMonitorV2，这里补一次；已经设过会失败，忽略即可。
    ::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    appearance_auto_ = options_.appearance == Appearance::Auto;
    appearance_ = appearance_auto_ ? detect_appearance() : options_.appearance;
    theme_ = make_theme(appearance_, false);

    if (!register_window_class()) {
        return false;
    }

    const HWND hwnd = ::CreateWindowExW(0, options_.window_class.c_str(), options_.title.c_str(), kWindowStyle,
                                        CW_USEDEFAULT, CW_USEDEFAULT, 200, 200, nullptr, nullptr, instance_, this);
    if (hwnd == nullptr) {
        return false;
    }

    dpi_ = static_cast<float>(::GetDpiForWindow(hwnd_));
    if (dpi_ <= 0.0f) {
        dpi_ = 96.0f;
    }

    // 客户区固定为 720x480 dp（或调用方给的值）；窗口尺寸用
    // AdjustWindowRectExForDpi 反推，150%/200% 下客户区不会被边框吃掉。
    RECT rect{0, 0, static_cast<LONG>(pixels(static_cast<float>(options_.width_dip))),
              static_cast<LONG>(pixels(static_cast<float>(options_.height_dip)))};
    ::AdjustWindowRectExForDpi(&rect, kWindowStyle, FALSE, 0, static_cast<UINT>(dpi_));
    ::SetWindowPos(hwnd_, nullptr, 0, 0, rect.right - rect.left, rect.bottom - rect.top,
                   SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);

    root_ = std::move(root);
    attach_host(*root_, this);
    update_client_size();

    // 标题栏亮暗要在窗口上屏前给到 DWM：系统是深色时，只在 ShowWindow 之后
    // 设置属性常常看不到效果（标题栏已经按系统主题画过一次）。
    apply_dark_title_bar(hwnd_, appearance_ == Appearance::Dark);

    ::ShowWindow(hwnd_, SW_SHOWNORMAL);
    ::UpdateWindow(hwnd_);

    // 材质必须在窗口上屏之后申请：DWM 只为已经显示的窗口建立合成表面。
    refresh_material();
    accessibility_ = create_accessibility_root(this);
    if (layered_) {
        render_now();
    }

    return true;
}

int Host::run() {
    MSG message{};
    while (::GetMessageW(&message, nullptr, 0, 0) > 0) {
        ::TranslateMessage(&message);
        ::DispatchMessageW(&message);
    }

    return static_cast<int>(message.wParam);
}

void Host::quit(int exit_code) {
    exit_code_ = exit_code;
    ::PostQuitMessage(exit_code);
}

void Host::close() {
    if (hwnd_ != nullptr) {
        ::PostMessageW(hwnd_, WM_CLOSE, 0, 0);
    }
}

void Host::set_appearance(Appearance appearance) {
    appearance_auto_ = appearance == Appearance::Auto;
    appearance_ = appearance_auto_ ? detect_appearance() : appearance;
    refresh_material();
}

void Host::set_material(MaterialPreference preference) {
    options_.material = preference;
    refresh_material();
}

const wchar_t* Host::presenter_name() const {
    return presenter_ != nullptr ? presenter_->name() : L"-";
}

void Host::refresh_material() {
    if (hwnd_ == nullptr || refreshing_material_) {
        return;
    }

    refreshing_material_ = true;

    std::vector<MaterialPreference> chain;
    if (appearance_ == Appearance::HighContrast) {
        // 高对比度必须 Solid：系统色可能白底，而文字颜色是按深色写死的。
        chain = {MaterialPreference::Solid};
    } else {
        switch (options_.material) {
            case MaterialPreference::Auto:
                if (remote_session() || !transparency_enabled()) {
                    chain = {MaterialPreference::Solid};
                } else {
                    chain = {MaterialPreference::Mica, MaterialPreference::Acrylic, MaterialPreference::Solid};
                }
                break;
            case MaterialPreference::Mica:
                chain = {MaterialPreference::Mica, MaterialPreference::Acrylic, MaterialPreference::Solid};
                break;
            case MaterialPreference::Acrylic:
                chain = {MaterialPreference::Acrylic, MaterialPreference::Solid};
                break;
            case MaterialPreference::Solid:
                chain = {MaterialPreference::Solid};
                break;
        }
    }

    const bool dark = appearance_ == Appearance::Dark;
    std::unique_ptr<Material> chosen;
    for (const MaterialPreference preference : chain) {
        auto candidate = make_material(preference);
        if (candidate != nullptr && candidate->apply(hwnd_, dark)) {
            chosen = std::move(candidate);
            break;
        }
    }

    if (chosen == nullptr) {
        // 任一 API 失败都必须落到 Solid：不允许黑屏/白屏。
        chosen = make_material(MaterialPreference::Solid);
        chosen->apply(hwnd_, dark);
    }

    material_ = std::move(chosen);
    material_kind_ = material_->kind();
    theme_ = make_theme(appearance_, material_kind_ != MaterialKind::Solid);

    // 分层窗口与标准 caption 不相容：UpdateLayeredWindow 会把 psize 当成**整窗**
    // 尺寸，而帧只有客户区大小。所以分层模式下切到无边框，窗口尺寸 == 帧尺寸；
    // 顶部 40dp 由 WM_NCHITTEST 提供拖动。
    const bool want_layered = layered_requested() && material_kind_ != MaterialKind::Solid;
    apply_window_mode(want_layered);

    if (want_layered) {
        presenter_ = make_layered_presenter(hwnd_);
    } else {
        presenter_ = make_gdi_presenter();
    }

    if (root_ != nullptr) {
        root_->on_theme_changed();
    }

    refreshing_material_ = false;
    invalidate(nullptr);
}

void Host::apply_window_mode(bool layered) {
    if (hwnd_ == nullptr) {
        return;
    }

    constexpr LONG_PTR kCaptionBits = WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_THICKFRAME;

    LONG_PTR style = ::GetWindowLongPtrW(hwnd_, GWL_STYLE);
    LONG_PTR ex_style = ::GetWindowLongPtrW(hwnd_, GWL_EXSTYLE);

    if (layered) {
        style = (style & ~kCaptionBits) | WS_POPUP;
        ex_style |= WS_EX_LAYERED;
    } else {
        style = (style & ~static_cast<LONG_PTR>(WS_POPUP)) | static_cast<LONG_PTR>(kWindowStyle);
        ex_style &= ~static_cast<LONG_PTR>(WS_EX_LAYERED);
    }

    ::SetWindowLongPtrW(hwnd_, GWL_STYLE, style);
    ::SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, ex_style);

    RECT rect{0, 0, static_cast<LONG>(pixels(static_cast<float>(options_.width_dip))),
              static_cast<LONG>(pixels(static_cast<float>(options_.height_dip)))};
    ::AdjustWindowRectExForDpi(&rect, static_cast<DWORD>(style), FALSE, static_cast<DWORD>(ex_style),
                               static_cast<UINT>(dpi_));
    ::SetWindowPos(hwnd_, nullptr, 0, 0, rect.right - rect.left, rect.bottom - rect.top,
                   SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);

    layered_ = layered;
    update_client_size();
}

void Host::update_client_size() {
    if (hwnd_ == nullptr) {
        return;
    }

    RECT client{};
    ::GetClientRect(hwnd_, &client);
    client_width_px_ = static_cast<int>(std::max<LONG>(1, client.right - client.left));
    client_height_px_ = static_cast<int>(std::max<LONG>(1, client.bottom - client.top));

    if (root_ != nullptr) {
        root_->bounds = {0.0f, 0.0f, dip(static_cast<float>(client_width_px_)),
                         dip(static_cast<float>(client_height_px_))};
    }
}

void Host::invalidate(const RectF* dirty) {
    (void)dirty;  // 720x480 全量重画成本很低，第一版不做脏矩形。

    if (hwnd_ == nullptr) {
        return;
    }

    if (layered_) {
        // 分层窗口不靠 WM_PAINT 上屏，每次失效都要重新 UpdateLayeredWindow。
        render_now();
        return;
    }

    ::InvalidateRect(hwnd_, nullptr, FALSE);
}

void Host::ensure_frame() {
    if (hwnd_ == nullptr) {
        return;
    }

    RECT client{};
    ::GetClientRect(hwnd_, &client);
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    if (width <= 0 || height <= 0) {
        return;
    }

    if (width != client_width_px_ || height != client_height_px_) {
        client_width_px_ = width;
        client_height_px_ = height;
        if (root_ != nullptr) {
            root_->bounds = {0.0f, 0.0f, dip(static_cast<float>(width)), dip(static_cast<float>(height))};
        }
    }

    if (frame_ == nullptr) {
        frame_ = std::make_unique<Frame>(width, height, dpi_);
    } else {
        frame_->resize(width, height, dpi_);
    }

    frame_->canvas().set_text_metrics(&text_engine_);
    text_engine_.bind(frame_->memory_dc(), width, height);
}

void Host::render(HDC dc) {
    ensure_frame();
    if (frame_ == nullptr) {
        return;
    }

    frame_->clear_transparent();
    Canvas& canvas = frame_->canvas();

    canvas.set_defer_text(true);
    canvas.clear_text_commands();

    const RectF client{0.0f, 0.0f, dip(static_cast<float>(client_width_px_)),
                       dip(static_cast<float>(client_height_px_))};
    if (material_kind_ == MaterialKind::Solid) {
        canvas.fill_rect(client, theme_.window_solid);
    }

    if (root_ != nullptr) {
        root_->paint(canvas);
    }

    // 所有文字走同一个 DirectWrite 灰度 AA 后端：透明与不透明表面、测量与
    // 绘制都来自同一套 layout，不会再出现「两种模式差半个像素」。
    canvas.flush();
    const auto& commands = canvas.text_commands();

    bool rendered = false;
    if (text_engine_.ready() && text_engine_.begin(dpi_)) {
        for (const TextCommand& command : commands) {
            text_engine_.draw(command);
        }

        text_engine_.end();
        rendered = true;
    }

    if (!rendered) {
        // DWrite 初始化/绑定失败：退回 GDI+ 灰度 AA，宁可略软也不能没字。
        for (const TextCommand& command : commands) {
            canvas.draw_text_immediate(command.text, command.rect, command.style, command.color, command.clip);
        }
    }

    canvas.clear_text_commands();
    canvas.set_defer_text(false);

    if (presenter_ != nullptr) {
        presenter_->present(*frame_, dc);
    }
}

void Host::render_now() {
    if (hwnd_ == nullptr) {
        return;
    }

    HDC dc = ::GetDC(hwnd_);
    render(dc);
    ::ReleaseDC(hwnd_, dc);
}

bool Host::to_dip(WPARAM lparam, float& x, float& y) const {
    x = dip(static_cast<float>(GET_X_LPARAM(lparam)));
    y = dip(static_cast<float>(GET_Y_LPARAM(lparam)));
    return true;
}

Widget* Host::hit_test(float x, float y) const {
    return root_ != nullptr ? root_->hit_test(x, y) : nullptr;
}

void Host::set_hover(Widget* widget) {
    if (hovered_ == widget) {
        return;
    }

    if (hovered_ != nullptr) {
        hovered_->hovered = false;
        hovered_->invalidate();
    }

    hovered_ = widget;
    if (hovered_ != nullptr) {
        hovered_->hovered = true;
        hovered_->invalidate();
    }
}

void Host::update_mouse(float x, float y) {
    Widget* target = hit_test(x, y);
    set_hover(target);

    MouseEvent event;
    event.type = MouseEvent::Type::Move;
    event.x = x;
    event.y = y;

    if (captured_ != nullptr) {
        captured_->on_mouse(event);
    } else if (target != nullptr) {
        target->on_mouse(event);
    }
}

void Host::collect_focusable(Widget& widget, std::vector<Widget*>& out) const {
    if (!widget.visible) {
        return;
    }

    if (widget.accepts_focus()) {
        out.push_back(&widget);
    }

    if (const auto* children = widget.child_widgets()) {
        for (const auto& child : *children) {
            collect_focusable(*child, out);
        }
    }
}

bool Host::focus_next(bool backwards) {
    if (root_ == nullptr) {
        return false;
    }

    std::vector<Widget*> focusable;
    collect_focusable(*root_, focusable);
    if (focusable.empty()) {
        return false;
    }

    const auto current = std::find(focusable.begin(), focusable.end(), focused_);
    int index = current != focusable.end() ? static_cast<int>(current - focusable.begin()) : -1;
    const int count = static_cast<int>(focusable.size());
    if (index < 0) {
        index = backwards ? count - 1 : 0;
    } else {
        index = (index + (backwards ? -1 : 1) + count) % count;
    }

    focus_widget(focusable[static_cast<size_t>(index)]);
    return true;
}

void Host::focus_widget(Widget* widget) {
    if (widget == focused_ || (widget != nullptr && !widget->accepts_focus())) {
        return;
    }

    if (focused_ != nullptr) {
        focused_->focused = false;
        focused_->on_blur();
        focused_->invalidate();
    }

    focused_ = widget;
    if (focused_ != nullptr) {
        focused_->focused = true;
        focused_->focus_visible = window_active_;
        focused_->on_focus();
        focused_->invalidate();
        notify_accessible(focused_, EVENT_OBJECT_FOCUS);
    }
}

void Host::collect_default(Widget& widget, Widget*& found) const {
    if (found != nullptr || !widget.visible || !widget.enabled) {
        return;
    }

    if (widget.is_default_button()) {
        found = &widget;
        return;
    }

    if (const auto* children = widget.child_widgets()) {
        for (const auto& child : *children) {
            collect_default(*child, found);
        }
    }
}

Widget* Host::default_button() const {
    Widget* found = nullptr;
    if (root_ != nullptr) {
        collect_default(*root_, found);
    }

    return found;
}

bool Host::handle_key(WPARAM vk, bool is_down) {
    KeyEvent event;
    event.vk = static_cast<UINT>(vk);
    event.is_down = is_down;
    event.ctrl = (::GetKeyState(VK_CONTROL) & 0x8000) != 0;
    event.shift = (::GetKeyState(VK_SHIFT) & 0x8000) != 0;
    event.alt = (::GetKeyState(VK_MENU) & 0x8000) != 0;

    if (event.vk == VK_TAB) {
        if (is_down) {
            focus_next(event.shift);
        }

        return true;
    }

    if (event.vk == VK_ESCAPE) {
        if (is_down) {
            if (options_.on_escape) {
                options_.on_escape();
            } else {
                close();
            }
        }

        return true;
    }

    if (event.vk == VK_RETURN) {
        if (is_down) {
            if (focused_ != nullptr && focused_->on_key(event)) {
                return true;
            }

            if (Widget* target = default_button()) {
                target->do_default_action();
            }
        }

        return true;
    }

    if (focused_ != nullptr) {
        return focused_->on_key(event);
    }

    return false;
}

void Host::set_tick_handler(std::function<void()> handler) {
    tick_handler_ = std::move(handler);
}

void Host::set_animating(bool animating, int interval_ms) {
    if (hwnd_ == nullptr || animating_ == animating) {
        return;
    }

    animating_ = animating;
    if (animating_) {
        last_tick_ms_ = ::GetTickCount64();
        ::SetTimer(hwnd_, kAnimationTimer, static_cast<UINT>(std::max(8, interval_ms)), nullptr);
    } else {
        ::KillTimer(hwnd_, kAnimationTimer);
    }
}

void Host::layout_changed() {
    invalidate(nullptr);
}

std::vector<Host::AccessibleEntry> Host::accessible_entries() const {
    std::vector<AccessibleEntry> entries;
    if (root_ != nullptr) {
        collect_entries(*root_, entries);
    }

    return entries;
}

int Host::child_id_of(const Widget* widget) const {
    if (widget == nullptr) {
        return 0;
    }

    const auto entries = accessible_entries();
    for (size_t index = 0; index < entries.size(); ++index) {
        if (entries[index].widget == widget && entries[index].row < 0) {
            return static_cast<int>(index) + 1;
        }
    }

    return 0;
}

void Host::notify_accessible(const Widget* widget, LONG event_id) {
    if (hwnd_ == nullptr || widget == nullptr) {
        return;
    }

    const int child_id = child_id_of(widget);
    if (child_id > 0) {
        ::NotifyWinEvent(event_id, hwnd_, OBJID_CLIENT, child_id);
    }
}

void Host::notify_list_selection(const Widget* table, int row) {
    if (hwnd_ == nullptr || table == nullptr || row < 0) {
        return;
    }

    const auto entries = accessible_entries();
    for (size_t index = 0; index < entries.size(); ++index) {
        if (entries[index].widget == table && entries[index].row < 0) {
            ::NotifyWinEvent(EVENT_OBJECT_SELECTION, hwnd_, OBJID_CLIENT, static_cast<LONG>(index) + 1 + row);
            return;
        }
    }
}

RectF Host::to_screen(const RectF& client_dip) const {
    if (hwnd_ == nullptr) {
        return client_dip;
    }

    POINT origin{0, 0};
    ::ClientToScreen(hwnd_, &origin);
    const float ratio = scale();
    return {origin.x + client_dip.x * ratio, origin.y + client_dip.y * ratio, client_dip.width * ratio,
            client_dip.height * ratio};
}

// ============================================================================
// 窗口过程
// ============================================================================

LRESULT CALLBACK Host::window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    Host* self = reinterpret_cast<Host*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        self = static_cast<Host*>(create->lpCreateParams);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }

    if (self == nullptr) {
        return ::DefWindowProcW(hwnd, message, wparam, lparam);
    }

    return self->handle(hwnd, message, wparam, lparam);
}

LRESULT Host::on_create() {
    dpi_ = static_cast<float>(::GetDpiForWindow(hwnd_));
    if (dpi_ <= 0.0f) {
        dpi_ = 96.0f;
    }

    update_client_size();
    return 0;
}

LRESULT Host::on_paint() {
    PAINTSTRUCT paint{};
    HDC dc = ::BeginPaint(hwnd_, &paint);
    render(dc);
    ::EndPaint(hwnd_, &paint);
    return 0;
}

LRESULT Host::on_get_object(WPARAM wparam, LPARAM lparam) {
    // LPARAM 在 64 位下可能被零扩展成 0x00000000FFFFFFFC；按 32 位有符号
    // 取回 OBJID_CLIENT(-4) 再比较，两种传法都能命中。
    if (static_cast<LONG>(lparam) == OBJID_CLIENT && accessibility_ != nullptr) {
        ++accessibility_requests_;
        return ::LresultFromObject(IID_IAccessible, wparam, accessibility_.get());
    }

    return ::DefWindowProcW(hwnd_, WM_GETOBJECT, wparam, lparam);
}

LRESULT Host::handle(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
        case WM_CREATE:
            hwnd_ = hwnd;
            return on_create();

        case WM_ERASEBKGND:
            // 背景由帧的 alpha 决定，系统擦背景只会闪。
            return 1;

        case WM_PAINT:
            return on_paint();

        case WM_SIZE:
            if (wparam != SIZE_MINIMIZED) {
                update_client_size();
                ::InvalidateRect(hwnd_, nullptr, FALSE);
            }

            return 0;

        case WM_DPICHANGED: {
            const UINT new_dpi = HIWORD(wparam);
            if (new_dpi != 0) {
                dpi_ = static_cast<float>(new_dpi);
            }

            const auto* suggested = reinterpret_cast<const RECT*>(lparam);
            if (suggested != nullptr) {
                ::SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                               suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
            }

            update_client_size();
            frame_.reset();  // 帧按像素建，DPI 变了必须重建
            ::InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }

        case WM_NCHITTEST:
            // 分层模式没有 caption：顶部 40dp 当标题栏拖动，其余仍走客户区。
            if (layered_) {
                POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
                ::ScreenToClient(hwnd_, &point);
                if (point.y < static_cast<int>(pixels(40.0f))) {
                    return HTCAPTION;
                }
            }

            break;

        case WM_MOUSEMOVE: {
            TRACKMOUSEEVENT track{};
            track.cbSize = sizeof(track);
            track.dwFlags = TME_LEAVE;
            track.hwndTrack = hwnd_;
            ::TrackMouseEvent(&track);

            float x = 0.0f;
            float y = 0.0f;
            to_dip(lparam, x, y);
            update_mouse(x, y);
            return 0;
        }

        case WM_MOUSELEAVE:
            set_hover(nullptr);
            return 0;

        case WM_LBUTTONDOWN: {
            ::SetFocus(hwnd_);

            float x = 0.0f;
            float y = 0.0f;
            to_dip(lparam, x, y);

            Widget* target = hit_test(x, y);
            if (target == nullptr) {
                return 0;
            }

            if (target->accepts_focus()) {
                focus_widget(target);
            }

            captured_ = target;
            ::SetCapture(hwnd_);

            MouseEvent event;
            event.type = MouseEvent::Type::Down;
            event.x = x;
            event.y = y;
            event.left_button = true;
            target->on_mouse(event);
            return 0;
        }

        case WM_LBUTTONUP: {
            float x = 0.0f;
            float y = 0.0f;
            to_dip(lparam, x, y);

            Widget* target = captured_;
            captured_ = nullptr;  // ReleaseCapture 会发 WM_CAPTURECHANGED，先摘掉避免重复

            if (target != nullptr) {
                MouseEvent event;
                event.type = MouseEvent::Type::Up;
                event.x = x;
                event.y = y;
                event.left_button = true;
                target->on_mouse(event);

                target->pressed = false;
                target->invalidate();
            }

            if (::GetCapture() == hwnd_) {
                ::ReleaseCapture();
            }

            return 0;
        }

        case WM_CAPTURECHANGED:
            if (captured_ != nullptr) {
                captured_->pressed = false;
                captured_->invalidate();
                captured_ = nullptr;
            }

            return 0;

        case WM_MOUSEWHEEL: {
            POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            ::ScreenToClient(hwnd_, &point);

            MouseEvent event;
            event.type = MouseEvent::Type::Wheel;
            event.x = dip(static_cast<float>(point.x));
            event.y = dip(static_cast<float>(point.y));
            event.wheel_delta = GET_WHEEL_DELTA_WPARAM(wparam);

            Widget* target = hit_test(event.x, event.y);
            if (target != nullptr && target->on_mouse(event)) {
                return 0;
            }

            if (focused_ != nullptr && focused_ != target) {
                focused_->on_mouse(event);
            }

            return 0;
        }

        case WM_KEYDOWN:
            if (handle_key(wparam, true)) {
                return 0;
            }

            break;

        case WM_KEYUP:
            if (handle_key(wparam, false)) {
                return 0;
            }

            break;

        case WM_SETFOCUS:
            window_active_ = true;
            if (focused_ == nullptr) {
                focus_next(false);
            } else {
                focused_->focus_visible = true;
                focused_->invalidate();
            }

            return 0;

        case WM_KILLFOCUS:
            window_active_ = false;
            if (focused_ != nullptr) {
                focused_->focus_visible = false;
                focused_->invalidate();
            }

            return 0;

        case WM_TIMER:
            if (wparam == kAnimationTimer) {
                const ULONGLONG now = ::GetTickCount64();
                float dt = last_tick_ms_ == 0 ? 0.0f : static_cast<float>(now - last_tick_ms_) / 1000.0f;
                last_tick_ms_ = now;
                dt = std::min(dt, 0.25f);

                if (root_ != nullptr) {
                    root_->tick(dt);
                }

                if (tick_handler_) {
                    tick_handler_();
                }

                return 0;
            }

            break;

        case WM_GETOBJECT:
            return on_get_object(wparam, lparam);

        case WM_SETTINGCHANGE:
        case WM_THEMECHANGED:
            if (appearance_auto_) {
                appearance_ = detect_appearance();
                refresh_material();
            }

            return 0;

        case WM_CLOSE:
            ::DestroyWindow(hwnd_);
            return 0;

        case WM_DESTROY:
            if (animating_) {
                ::KillTimer(hwnd_, kAnimationTimer);
                animating_ = false;
            }

            hwnd_ = nullptr;
            ::PostQuitMessage(exit_code_);
            return 0;

        default:
            break;
    }

    return ::DefWindowProcW(hwnd, message, wparam, lparam);
}

}  // namespace msixpkg::ui
