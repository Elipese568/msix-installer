#include "gui_wizard.h"

// Win32 原生安装向导：窗口、五页状态机、控件布局与本地化（docs/installer-design.md §8）。
//
// 线程规则的落点：这个文件里的每一行都跑在 UI 线程上。耗时工作一律通过
// gui_worker.cpp 的 start_* 放进后台线程，结果用 PostMessage 回到 handle() 里。
// 「同意页」是唯一的同步点，而等待的一方始终是后台线程。

#include <Windows.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <objbase.h>
#include <gdiplus.h>
#include <shellapi.h>

#include <algorithm>
#include <chrono>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "gui.h"
#include "deployment.h"
#include "internal.h"
#include "msixpkg/errors.h"

namespace msixpkg::gui {
namespace {

/// DWM 的系统背景材质与圆角。值来自 dwmapi.h 的枚举（SDK 10.0.22621 起）；
/// 这里显式写死是因为我们只依赖「返回值」判定支持与否，而不依赖头文件版本。
constexpr DWORD kBackdropMainWindow = 2;   // DWMSBT_MAINWINDOW = Mica
constexpr DWORD kCornerRound = 2;          // DWMWCP_ROUND

// ---- 让 Mica 在一套 GDI 窗口里真正可见 ----
//
// 这套向导的客户区是 GDI 画的（含 id 各不相同的标准控件），而 Mica 是 DWM 合成在
// 窗口**背后**的材质。两者要接上，缺一不可的三步（实测：少了第 2 步客户区永远是不
// 透明的、材质被整片盖住；少了第 3 步材质是浅色的，跟深色窗口不搭）：
//
//   1) DWMWA_SYSTEMBACKDROP_TYPE = DWMSBT_MAINWINDOW
//      告诉 DWM 画哪种材质（Win11 22621+ 才认）。
//   2) DWMWA_REDIRECTIONBITMAP_ALPHA = TRUE                        —— 关键的一条
//      默认 DWM 把窗口的重定向位图当**完全不透明**，材质永远被盖在下面。打开这个
//      属性后才会逐像素看 alpha（Win11 26100+ 才有；更老的系统直接回落纯色）。
//   3) DwmExtendFrameIntoClientArea(-1 边距)
//      把客户区纳入扩展边框的合成：材质才会跟随窗口的深色模式（实测只做 1+2 时
//      客户区拿到的是浅色材质，跟深色窗口不搭）。
//
// 有了这三步，客户区里 alpha=0 的像素就透出材质。剩下的问题是 GDI 的填充改不动
// alpha 通道：FillRect 只会把颜色写进去，alpha 还是 0/不透明——所以底色要用
// GDI+ 的 SourceCopy 去清（见 WM_ERASEBKGND 与 clear_to_transparent）。
// 文字与按钮是 GDI 画的，alpha 会被写成不透明，正好留在材质上面。
constexpr DWORD kRedirectionBitmapAlpha = 39;  // DWMWA_REDIRECTIONBITMAP_ALPHA（Win11 26100+）

/// GDI+ 只在这条路上用得到，所以按需初始化：拿不到材质时连 GDI+ 都不启动。
bool ensure_gdiplus() {
    static Gdiplus::GdiplusStartupInput input;
    static ULONG_PTR token = 0;
    static const bool ready = Gdiplus::GdiplusStartup(&token, &input, nullptr) == Gdiplus::Ok;
    return ready;
}

/// 把一块 DC 区域清成「完全透明」（alpha = 0）。
///
/// 普通 GDI 做不到这件事：FillRect 只写 RGB，alpha 通道要么保持原样、要么被系统当成
/// 不透明。GDI+ 的 SourceCopy 组合模式是唯一能把 alpha 一并写进重定向位图的路子；
/// 用 FillRectangle 而不是 Clear，是为了只碰这一块（Clear 会清整个绘制表面）。
void clear_to_transparent(HDC dc, const RECT& area) {
    Gdiplus::Graphics graphics(dc);
    graphics.SetCompositingMode(Gdiplus::CompositingModeSourceCopy);
    Gdiplus::SolidBrush transparent(Gdiplus::Color(0, 0, 0, 0));
    graphics.FillRectangle(&transparent, static_cast<INT>(area.left), static_cast<INT>(area.top),
                           static_cast<INT>(area.right - area.left), static_cast<INT>(area.bottom - area.top));
}

/// 逻辑基准里的子区域（dp）。
constexpr int kMargin = 18;
constexpr int kButtonHeight = 26;

HBRUSH create_solid_brush(COLORREF color) {
    return ::CreateSolidBrush(color);
}

/// 系统库存画刷（COLOR_WINDOW+1 这类）不能 DeleteObject。nullptr 也当作「没有」。
bool is_stock_brush(HBRUSH brush) {
    return brush == nullptr || brush == reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
}

/// 只读编辑框也会收到 WM_CTLCOLORSTATIC，所以「透明背景」这条要按控件类区分：
/// 编辑框是可滚动的内容区，需要自己的底色；静态文本才是直接画在材质上的那类。
bool is_edit_class(HWND control) {
    if (control == nullptr) {
        return false;
    }

    wchar_t name[16]{};
    constexpr int kNameLength = static_cast<int>(sizeof(name) / sizeof(name[0]));
    if (::GetClassNameW(control, name, kNameLength) == 0) {
        return false;
    }

    return std::wstring_view(name) == L"Edit";
}

bool high_contrast_enabled() {
    HIGHCONTRASTW contrast{};
    contrast.cbSize = sizeof(contrast);
    if (::SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0) == FALSE) {
        return false;
    }

    return (contrast.dwFlags & HCF_HIGHCONTRASTON) != 0;
}

COLORREF read_apps_use_light_theme() {
    DWORD value = 1;
    DWORD size = sizeof(value);
    const LSTATUS status = ::RegGetValueW(HKEY_CURRENT_USER,
                                          L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                                          L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size);
    return (status == ERROR_SUCCESS && value == 0) ? 0 : 1;
}

/// 把一行诊断写到**父进程的控制台**（只有在终端里运行时才看得见），然后立刻返回。
///
/// 为什么不用 MessageBox：本程序的「这是 CLI 子命令」错误几乎只会在一种情况下出现 ——
/// 有人把提权助手指到了 GUI，而助手是用 SW_HIDE 拉起的。模态框在那里就是一个
/// 看不见、又永不消失的窗口，会把父进程的等待硬生生拖到超时。
void write_to_parent_console(const std::wstring& text) {
    ::OutputDebugStringW((text + L"\n").c_str());

    if (::AttachConsole(ATTACH_PARENT_PROCESS) == FALSE) {
        return;
    }

    const HANDLE output = ::GetStdHandle(STD_ERROR_HANDLE);
    if (output != nullptr && output != INVALID_HANDLE_VALUE) {
        std::wstring line = text + L"\r\n";
        DWORD written = 0;
        ::WriteConsoleW(output, line.c_str(), static_cast<DWORD>(line.size()), &written, nullptr);
    }

    ::FreeConsole();
}

/// 深色模式下重画复选框的文字，保留主题画出来的勾选框（NM_CUSTOMDRAW）。
///
/// 为什么不能只靠 WM_CTLCOLORBTN：带视觉样式的按钮类在画文字时用的是主题颜色，
/// 父窗口返回的字体/颜色会被忽略（实测深色底上仍是黑字）。自定义绘制的官方钩子是
/// NM_CUSTOMDRAW；这里只接管 CDDS_PREPAINT，把文字按主题色再画一遍。
LRESULT draw_dark_checkbox(const NMCUSTOMDRAW& draw, COLORREF text_color) {
    const HWND control = draw.hdr.hwndFrom;
    ::SetBkMode(draw.hdc, TRANSPARENT);
    ::SetTextColor(draw.hdc, ::IsWindowEnabled(control) != FALSE ? text_color
                                                                  : ::GetSysColor(COLOR_GRAYTEXT));

    // 主题勾选框的宽度随 DPI 变化；左侧留出「勾选框 + 间隙」再开始画文字。
    const int dpi = ::GetDpiForWindow(control);
    RECT text_rect = draw.rc;
    text_rect.left += ::MulDiv(20, dpi == 0 ? 96 : dpi, 96);

    const int length = ::GetWindowTextLengthW(control);
    std::wstring label(static_cast<size_t>(length) + 1, L'\0');
    ::GetWindowTextW(control, label.data(), length + 1);
    label.resize(static_cast<size_t>(length));

    ::DrawTextW(draw.hdc, label.c_str(), static_cast<int>(label.size()), &text_rect,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    if (::GetFocus() == control) {
        RECT focus = text_rect;
        ::DrawTextW(draw.hdc, label.c_str(), static_cast<int>(label.size()), &focus,
                    DT_CALCRECT | DT_LEFT | DT_SINGLELINE);
        text_rect.right = text_rect.left + (focus.right - focus.left) + 2;
        text_rect.top += 2;
        text_rect.bottom -= 2;
        ::DrawFocusRect(draw.hdc, &text_rect);
    }

    // 主题已经画过勾选框；默认的「再画一遍文字」必须跳过，否则黑字会盖在
    // 我们刚画的白字上面。
    return CDRF_SKIPDEFAULT;
}

}  // namespace

// ============================================================================
// 入口
// ============================================================================

int run(void* instance, const wchar_t* command_line) {
    HINSTANCE module = static_cast<HINSTANCE>(instance);

    INITCOMMONCONTROLSEX controls{};
    controls.dwSize = sizeof(controls);
    controls.dwICC = ICC_STANDARD_CLASSES | ICC_PROGRESS_CLASS | ICC_LISTVIEW_CLASSES;
    ::InitCommonControlsEx(&controls);

    Strings strings;
    strings.load(module);

    const CommandLine parsed = parse_command_line(command_line == nullptr ? L"" : command_line);

    // ---- GUI 不实现的 CLI 子命令：立刻失败，绝不当容器路径 ----
    // 这些子命令（cert-add / cert-remove / list / verify / repair / clean）只有
    // msixpkg-native.exe 实现。把它们当容器路径会一路走到下面的单实例交接：
    // 给已有窗口发 WM_COPYDATA —— 而那个窗口可能正卡在等这次「提权」返回
    // （UI 线程等编排线程、编排线程等子进程、子进程等 UI 线程），三方互等就死锁了。
    if (!parsed.unsupported_subcommand.empty()) {
        // **不用 MessageBox**：这条路最可能是「提权助手指到了 GUI」，而助手是用
        // SW_HIDE 拉起的 —— 一个看不见的模态框会把它变成永不返回的子进程。
        // 从终端运行时把原因写到父控制台；其余场合只在调试器里可见。
        const std::wstring message =
            strings.text(IDS_ERR_NOT_A_GUI_COMMAND) + L" (" + parsed.unsupported_subcommand + L")";
        write_to_parent_console(message);
        return exit_code::kUsage;
    }

    // ---- 单实例 ----
    // Global\ 前缀是有意的：证书存储是机器级的，同一台机器上两个会话同时装同一张
    // 证书会互相覆盖「导入前是否已存在」的判定，而那条判定决定卸载时有没有资格回收。
    HANDLE instance_mutex = ::CreateMutexW(nullptr, FALSE, kSingleInstanceMutex);
    const bool already_running = instance_mutex != nullptr && ::GetLastError() == ERROR_ALREADY_EXISTS;
    if (already_running) {
        int result = exit_code::kSuccess;
        HWND existing = ::FindWindowW(kWindowClass, nullptr);
        if (existing != nullptr) {
            ::SetForegroundWindow(existing);
            ::ShowWindow(existing, SW_RESTORE);

            if (!parsed.container.empty()) {
                // 把新容器交给已有实例。WM_COPYDATA 的缓冲只在接收方处理消息期间有效，
                // 所以接收方必须立刻拷走（见 Wizard::on_copy_data）。
                const std::wstring path = parsed.container.wstring();
                COPYDATASTRUCT data{};
                data.dwData = kCopyDataMagic;
                data.cbData = static_cast<DWORD>((path.size() + 1) * sizeof(wchar_t));
                data.lpData = const_cast<wchar_t*>(path.c_str());

                // 必须带超时：对端要是卡住了，第二个实例不能跟着一起卡死。
                // SMTO_ABORTIFHUNG 让「对端已无响应」立刻返回 0；BUSY 表示对端正在
                // 装东西、明确拒绝接手（kCopyDataBusy 以前定义了却没人回）。
                DWORD_PTR reply = 0;
                const LRESULT delivered = ::SendMessageTimeoutW(
                    existing, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&data),
                    SMTO_ABORTIFHUNG | SMTO_BLOCK, kCopyDataTimeoutMs, &reply);
                if (delivered == 0 || reply == kCopyDataBusy) {
                    ::MessageBoxW(nullptr, strings.get(IDS_PROGRESS_BUSY).data(),
                                  strings.get(IDS_PROGRESS_TITLE).data(), MB_OK | MB_ICONINFORMATION);
                    result = exit_code::kPrecondition;
                }
            }
        }

        if (instance_mutex != nullptr) {
            ::CloseHandle(instance_mutex);
        }

        return result;
    }

    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.style = CS_HREDRAW | CS_VREDRAW;
    window_class.lpfnWndProc = &Wizard::window_proc;
    window_class.hInstance = module;
    window_class.hIcon = ::LoadIconW(module, MAKEINTRESOURCEW(IDI_MSIXPKG));
    window_class.hIconSm = window_class.hIcon;
    window_class.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    window_class.hbrBackground = nullptr;  // 背景由 WM_ERASEBKGND 自己画（跟随深色模式）
    window_class.lpszClassName = kWindowClass;
    if (::RegisterClassExW(&window_class) == 0) {
        if (instance_mutex != nullptr) {
            ::CloseHandle(instance_mutex);
        }

        return exit_code::kUnexpected;
    }

    auto shared = std::make_shared<SharedState>();
    Wizard wizard(instance, shared, std::move(strings));
    if (!wizard.create()) {
        if (instance_mutex != nullptr) {
            ::CloseHandle(instance_mutex);
        }

        return exit_code::kUnexpected;
    }

    if (parsed.uninstall) {
        // ARP 的 UninstallString 走这里：`<安装器> uninstall --family-name <PFN>`。
        wizard.show_uninstall(parsed.uninstall_family_name, parsed.uninstall_install_id);
    } else {
        wizard.show(parsed.container.wstring(), parsed.manage);
    }

    MSG message{};
    while (::GetMessageW(&message, nullptr, 0, 0) > 0) {
        // IsDialogMessageW 提供标准控件的键盘导航：Tab / Shift+Tab 在 WS_TABSTOP 之间
        // 移动、方向键在单选组内移动、回车触发 BS_DEFPUSHBUTTON。自己实现这套规则
        // 是无障碍缺陷的常见来源，所以直接复用系统实现。
        if (::IsDialogMessageW(wizard.hwnd(), &message) != FALSE) {
            continue;
        }

        ::TranslateMessage(&message);
        ::DispatchMessageW(&message);
    }

    const int code = wizard.exit_code();
    if (instance_mutex != nullptr) {
        ::CloseHandle(instance_mutex);
    }

    return code;
}

// ============================================================================
// Wizard：创建与销毁
// ============================================================================

Wizard::Wizard(void* instance, std::shared_ptr<SharedState> shared, Strings strings)
    : instance_(instance), shared_(std::move(shared)), worker_(shared_), strings_(std::move(strings)) {}

Wizard::~Wizard() {
    // 顺序很重要：先让编排线程退出（此时 hwnd_ 还合法，PostMessage 不会打到野指针），
    // 再让窗口句柄失效。
    stop_worker(worker_);
    worker_.wait_idle();

    shared_->target.store(nullptr);

    if (font_title_ != nullptr) {
        ::DeleteObject(font_title_);
    }

    if (font_heading_ != nullptr) {
        ::DeleteObject(font_heading_);
    }

    if (font_body_ != nullptr) {
        ::DeleteObject(font_body_);
    }

    release_brushes();

    if (hwnd_ != nullptr && ::IsWindow(hwnd_) != FALSE) {
        ::DestroyWindow(hwnd_);
    }

    hwnd_ = nullptr;
    shared_->target.store(nullptr);
}

bool Wizard::create() {
    const HWND hwnd = ::CreateWindowExW(WS_EX_CONTROLPARENT, kWindowClass, strings_.get(IDS_WINDOW_TITLE).data(),
                                        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                                        CW_USEDEFAULT, CW_USEDEFAULT, scale(kBaseWidth), scale(kBaseHeight),
                                        nullptr, nullptr, static_cast<HINSTANCE>(instance_),
                                        this);
    if (hwnd == nullptr) {
        return false;
    }

    // WM_CREATE 里已经完成控件创建、主题与尺寸调整；这里只补一次字体与文案，
    // 因为 WM_CREATE 里 strings_ 还没用到（控件文本要等页面渲染才填）。
    apply_fonts();
    apply_theme();
    apply_texts();
    layout();
    show_page(WizardPage::Summary);
    return true;
}

void Wizard::show(const std::wstring& container, bool manage) {
    ::ShowWindow(hwnd_, SW_SHOWNORMAL);
    ::UpdateWindow(hwnd_);

    // 材质要在窗口上屏之后再申请：DWM 只会为已经显示的窗口建立合成表面。
    apply_backdrop();

    if (manage) {
        // --manage：管理页是入口，不参与安装流程。
        show_page(WizardPage::Manage);
        begin_manage_load();
        return;
    }

    if (!container.empty()) {
        begin_preview(container);
        return;
    }

    // 没有容器：停在摘要页并说明「把 .msixpkg 拖进来」。
    {
        const std::lock_guard<std::mutex> guard(shared_->mutex);
        shared_->preview = PackageView{};
    }

    show_page(WizardPage::Summary);
    render_page();
}

void Wizard::show_uninstall(const std::wstring& family_name, const std::wstring& install_id) {
    ::ShowWindow(hwnd_, SW_SHOWNORMAL);
    ::UpdateWindow(hwnd_);
    apply_backdrop();

    if (family_name.empty() && install_id.empty()) {
        // 没有目标 = 用法错误（ARP 一定会带 --family-name）。用退出码 3 说清楚，
        // 而不是打开一个什么都干不了的窗口。
        exit_code_.store(exit_code::kUsage);
        ::MessageBoxW(hwnd_, strings_.get(IDS_MANAGE_NO_SELECTION).data(), strings_.get(IDS_MANAGE_TITLE).data(),
                      MB_OK | MB_ICONWARNING);
        ::DestroyWindow(hwnd_);
        return;
    }

    begin_uninstall_for(narrow_utf8(install_id), family_name);
}

LRESULT CALLBACK Wizard::window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    Wizard* self = reinterpret_cast<Wizard*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        self = static_cast<Wizard*>(create->lpCreateParams);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }

    // WM_NCCREATE 之前（以及别的进程用同一个类名建的窗口）没有 Wizard：
    // 必须回落给系统，否则会在 nullptr 上调用成员函数。
    if (self == nullptr) {
        return ::DefWindowProcW(hwnd, message, wparam, lparam);
    }

    return self->handle(hwnd, message, wparam, lparam);
}

LRESULT Wizard::handle(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
        case WM_CREATE:
            hwnd_ = hwnd;
            shared_->target.store(hwnd);
            return on_create();

        case WM_COMMAND:
            return on_command(LOWORD(wparam), HIWORD(wparam), reinterpret_cast<HWND>(lparam));

        case WM_NOTIFY:
            return on_notify(reinterpret_cast<NMHDR*>(lparam));

        case WM_DROPFILES: {
            // 装东西的时候不接手新容器：adopt_container() 会先把正在跑的任务停干净，
            // 而那是**在 UI 线程上等**编排线程 —— 安装中就是几十秒到几分钟的假死。
            if (worker_.busy()) {
                ::DragFinish(reinterpret_cast<HDROP>(wparam));
                ::MessageBeep(MB_ICONWARNING);
                return 0;
            }

            // 拖放：只接受一个文件。载入后直接跳同意页（摘要页仍可点「上一步」回看）。
            const HDROP drop = reinterpret_cast<HDROP>(wparam);
            const UINT count = ::DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
            if (count > 0) {
                const UINT length = ::DragQueryFileW(drop, 0, nullptr, 0);
                std::wstring path(length + 1, L'\0');
                ::DragQueryFileW(drop, 0, path.data(), length + 1);
                path.resize(length);
                ::DragFinish(drop);
                adopt_container(path);
            } else {
                ::DragFinish(drop);
            }

            return 0;
        }

        case WM_COPYDATA:
            return on_copy_data(reinterpret_cast<const COPYDATASTRUCT*>(lparam));

        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX:
            return on_ctl_color(message, reinterpret_cast<HDC>(wparam), reinterpret_cast<HWND>(lparam));

        case WM_ERASEBKGND: {
            HDC dc = reinterpret_cast<HDC>(wparam);
            RECT client{};
            ::GetClientRect(hwnd, &client);

            if (backdrop_) {
                // 只清 alpha。铺任何颜色都会把材质盖住；DWM 自己会在底下画材质，
                // 窗口失焦时它还会自动换成 SolidBackgroundFillColorBase 的纯色兜底。
                clear_to_transparent(dc, client);
                return 1;
            }

            ::FillRect(dc, &client, brush_background_);
            return 1;
        }


        case WM_PAINT: {
            // 控件已经把自己画好了；这里只是为了「满足」系统的绘制请求，
            // 不让 ValidateRect 之后的无效区域反复触发 WM_PAINT。
            PAINTSTRUCT paint{};
            ::BeginPaint(hwnd, &paint);
            ::EndPaint(hwnd, &paint);
            return 0;
        }

        case WM_DPICHANGED: {
            const int dpi = HIWORD(wparam);
            on_dpi_changed(dpi, reinterpret_cast<const RECT*>(lparam));
            return 0;
        }

        // ---- 编排线程发来的消息（全部只读 SharedState + 刷新控件） ----
        case kMsgPreviewReady:
            render_page();
            return 0;

        case kMsgProgress:
            if (page_ == WizardPage::Progress) {
                // 提权马上发生（编排线程正要拉起 runas 助手）：把自己摆到前台并闪一下任务栏。
                // 本机策略 `ConsentPromptBehaviorAdmin = 0` 时根本不会有 UAC 窗口（静默提权）；
                // 而策略把确认框放在普通桌面（PromptOnSecureDesktop = 0）时，确认窗口
                // 可能开在我们后面 —— 用户就会以为「什么都没有发生，卡住了」。
                bool waiting = false;
                {
                    const std::lock_guard<std::mutex> guard(shared_->mutex);
                    waiting = shared_->waiting_for_elevation;
                }

                if (waiting && !elevation_foregrounded_) {
                    elevation_foregrounded_ = true;
                    ::SetForegroundWindow(hwnd_);
                    FLASHWINFO flash{};
                    flash.cbSize = sizeof(flash);
                    flash.hwnd = hwnd_;
                    flash.dwFlags = FLASHW_TRAY | FLASHW_TIMERNOFG;
                    ::FlashWindowEx(&flash);
                }

                render_progress();
            }

            return 0;

        case kMsgConsentAsk:
            // 编排线程已经挂在 GuiConsent::Ask 的等待循环里；这里把同意页推上前台。
            show_page(WizardPage::Consent);
            render_page();
            return 0;

        case kMsgInstallDone:
            if (page_ == WizardPage::Progress) {
                render_page();
            }

            return 0;

        case kMsgManageLoaded:
            if (page_ == WizardPage::Manage) {
                render_page();
            }

            return 0;

        case kMsgUninstallDone:
            begin_manage_load();
            if (page_ == WizardPage::Manage) {
                render_page();
            }

            return 0;

        case kMsgRepairReady: {
            if (page_ != WizardPage::Manage) {
                return 0;
            }

            ManageView view;
            {
                const std::lock_guard<std::mutex> guard(shared_->mutex);
                view = shared_->manage;
            }

            // task_repair 用 \u0001 表示「没有需要建议的项」：那条文案属于 UI 层，
            // 编排线程不该去碰字符串表，所以到这里才替换。
            std::wstring text = strings_.text(IDS_MANAGE_REPAIR_INTRO);
            if (view.repair_text == L"\u0001") {
                text += L"\r\n\r\n";
                text += strings_.get(IDS_MANAGE_NO_SUGGESTIONS);
            } else {
                text += L"\r\n\r\n";
                text += view.repair_text;
            }

            set_text(IDC_MANAGE_DETAIL, text);
            ::MessageBoxW(hwnd_, text.c_str(), strings_.get(IDS_MANAGE_REPAIR).data(), MB_OK | MB_ICONINFORMATION);
            return 0;
        }

        case WM_CLOSE:
            return on_close();

        case WM_DESTROY:
            shared_->target.store(nullptr);
            ::PostQuitMessage(exit_code_.load());
            hwnd_ = nullptr;
            return 0;

        default:
            break;
    }

    return ::DefWindowProcW(hwnd, message, wparam, lparam);
}

// ============================================================================
// 窗口创建、控件与主题
// ============================================================================

LRESULT Wizard::on_create() {
    // 控件必须先建齐再谈布局：布局函数只搬动已存在的窗口，不负责创建。
    for (const ControlSpec& spec : control_layout()) {
        const HWND control = create_control(spec);
        if (control != nullptr) {
            controls_.push_back(control);
            ids_.push_back(spec.id);
        }
    }

    ::DragAcceptFiles(hwnd_, TRUE);

    dpi_ = ::GetDpiForWindow(hwnd_);
    if (dpi_ == 0) {
        dpi_ = 96;
    }

    apply_theme();
    apply_fonts();

    // 把客户区调整到 720x480 dp。窗口尺寸用 AdjustWindowRectExForDpi 反推，
    // 这样 150% / 200% 下客户区仍然完整容纳全部控件（不被边框吃掉）。
    const int width = scale(kBaseWidth);
    const int height = scale(kBaseHeight);
    RECT rect{0, 0, width, height};
    ::AdjustWindowRectExForDpi(&rect, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE, 0, dpi_);
    ::SetWindowPos(hwnd_, nullptr, 0, 0, rect.right - rect.left, rect.bottom - rect.top,
                   SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);

    layout();
    show_page(WizardPage::Summary);
    return 0;
}

HWND Wizard::create_control(const ControlSpec& spec) {
    DWORD style = WS_CHILD | spec.style;
    if (spec.klass != nullptr && std::wstring_view(spec.klass) == L"STATIC") {
        style |= WS_VISIBLE;
    }

    const HWND control = ::CreateWindowExW(spec.extended, spec.klass, L"", style, 0, 0, 0, 0, hwnd_,
                                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(spec.id)),
                                           static_cast<HINSTANCE>(instance_), nullptr);
    if (control == nullptr) {
        return nullptr;
    }

    if (spec.klass != nullptr && std::wstring_view(spec.klass) == WC_LISTVIEWW) {
        list_view_ = control;

        // 列表要能被 Tab 聚焦、能被屏幕阅读器读出「第几行第几列」，所以用标准
        // ListView 而不是自绘列表。
        ::SetWindowLongPtrW(control, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));

        const std::wstring_view columns[] = {
            strings_.get(IDS_MANAGE_COL_NAME),
            strings_.get(IDS_MANAGE_COL_VERSION),
            strings_.get(IDS_MANAGE_COL_CERT),
            strings_.get(IDS_MANAGE_COL_STATUS),
        };
        const int widths[] = {236, 84, 250, 100};
        for (int index = 0; index < 4; ++index) {
            LVCOLUMNW column{};
            column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
            column.pszText = const_cast<wchar_t*>(columns[index].data());
            column.cx = scale(widths[index]);
            column.iSubItem = index;
            ::SendMessageW(control, LVM_INSERTCOLUMNW, static_cast<WPARAM>(index),
                           reinterpret_cast<LPARAM>(&column));
        }
    }

    return control;
}

void Wizard::apply_theme() {
    // 深色模式**只靠返回值判定**：DwmSetWindowAttribute 在不支持的 Windows 上返回
    // E_INVALIDARG，这时我们就老老实实画浅色，不去猜系统版本（§8 明确要求）。
    BOOL dark = FALSE;
    const HRESULT dark_result = ::DwmSetWindowAttribute(hwnd_, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    dark_ = SUCCEEDED(dark_result) && read_apps_use_light_theme() == 0;
    if (dark_) {
        dark = TRUE;
        ::DwmSetWindowAttribute(hwnd_, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    }

    // Mica 不在 WM_CREATE 里申请：统一由 apply_backdrop 在窗口上屏后处理（失败要整体
    // 回退纯色，放在这里会变成「属性设了、但没人负责后面的透明清底」的半套状态）。
    DWORD corner = kCornerRound;
    const DWORD corner_size = sizeof(corner);
    const HRESULT corner_result = ::DwmGetWindowAttribute(hwnd_, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, corner_size);
    if (SUCCEEDED(corner_result)) {
        corner = 2;  // DWMWCP_ROUND
        ::DwmSetWindowAttribute(hwnd_, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
    }

    if (dark_) {
        color_background_ = RGB(0x20, 0x20, 0x20);
        color_text_ = RGB(0xF0, 0xF0, 0xF0);
    } else {
        color_background_ = ::GetSysColor(COLOR_WINDOW);
        color_text_ = ::GetSysColor(COLOR_WINDOWTEXT);
    }

    // apply_theme 可能被调用多次（WM_CREATE 后 create() 还会补一次），先放掉上一次的
    // 画刷，否则每调一次就漏一个 GDI 对象。
    release_brushes();
    if (dark_) {
        // 只读编辑框也会收到 WM_CTLCOLORSTATIC，所以它跟背景同色才不刺眼。
        brush_edit_ = create_solid_brush(color_background_);
        brush_background_ = brush_edit_;
    } else {
        brush_background_ = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);  // 系统画刷，不删
        brush_edit_ = brush_background_;
    }

    // 管理页的 ListView 不走 WM_CTLCOLOR*，得单独喂颜色，否则深色窗口里会嵌一块纯白。
    // 只用 comctl32 自己的消息（LVM_SET*COLOR），不引入 uxtheme —— 依赖面保持在
    // 设计文档列出的那几个 DLL 内。
    apply_list_colors();
}

void Wizard::release_brushes() {
    HBRUSH background = brush_background_;
    HBRUSH edit = brush_edit_;
    brush_background_ = nullptr;
    brush_edit_ = nullptr;

    if (!is_stock_brush(background)) {
        ::DeleteObject(background);
    }

    // 深色模式下两个成员是同一个对象，只删一次。
    if (!is_stock_brush(edit) && edit != background) {
        ::DeleteObject(edit);
    }
}

void Wizard::apply_backdrop() {
    backdrop_ = false;

    if (hwnd_ == nullptr) {
        return;
    }

    // 高对比度下 DWM 用系统配色顶替材质：浅色的高对比主题里底色可能是白的，
    // 而这里的文字颜色是按深色主题写死的，会变成白底白字。宁可不启用材质。
    if (high_contrast_enabled()) {
        return;
    }

    DWORD backdrop = kBackdropMainWindow;
    if (FAILED(::DwmSetWindowAttribute(hwnd_, DWMWA_SYSTEMBACKDROP_TYPE, &backdrop, sizeof(backdrop)))) {
        return;  // Win10 / Win11 22621 之前：不认系统背景材质，回落纯色。
    }

    BOOL alpha = TRUE;
    if (FAILED(::DwmSetWindowAttribute(hwnd_, kRedirectionBitmapAlpha, &alpha, sizeof(alpha)))) {
        // Win11 22621–26099：认材质，但不认「重定向位图带 alpha」。这时候客户区
        // 无法透出材质（硬开会变成一层白色玻璃），宁可保持纯色。
        return;
    }

    // 打开 alpha 之后，客户区里没被写成不透明的像素会透出材质 —— 那正是我们要的，
    // 但前提是画底色时真的会写 alpha=0，而这件事只有 GDI+ 做得到。
    if (!ensure_gdiplus()) {
        return;
    }

    MARGINS margins{-1, -1, -1, -1};
    if (FAILED(::DwmExtendFrameIntoClientArea(hwnd_, &margins))) {
        return;
    }

    backdrop_ = true;

    // 客户区原本是不透明的底色，现在要整体重画成透明（见 WM_ERASEBKGND）。
    ::InvalidateRect(hwnd_, nullptr, TRUE);
}

void Wizard::apply_list_colors() {
    if (list_view_ == nullptr) {
        return;
    }

    const COLORREF background = dark_ ? color_background_ : ::GetSysColor(COLOR_WINDOW);
    const COLORREF text = dark_ ? color_text_ : ::GetSysColor(COLOR_WINDOWTEXT);

    ::SendMessageW(list_view_, LVM_SETBKCOLOR, 0, static_cast<LPARAM>(background));
    ::SendMessageW(list_view_, LVM_SETTEXTBKCOLOR, 0, static_cast<LPARAM>(background));
    ::SendMessageW(list_view_, LVM_SETTEXTCOLOR, 0, static_cast<LPARAM>(text));
}

void Wizard::apply_fonts() {
    const auto make_font = [this](int point, bool bold) {
        LOGFONTW description{};
        description.lfHeight = point_to_pixels(point);
        description.lfWeight = bold ? FW_SEMIBOLD : FW_NORMAL;
        description.lfCharSet = DEFAULT_CHARSET;
        description.lfQuality = CLEARTYPE_QUALITY;
        ::wcscpy_s(description.lfFaceName, L"Segoe UI");
        return ::CreateFontIndirectW(&description);
    };

    const HFONT title = make_font(15, true);
    const HFONT body = make_font(9, false);

    if (font_title_ != nullptr) {
        ::DeleteObject(font_title_);
    }

    if (font_heading_ != nullptr) {
        ::DeleteObject(font_heading_);
    }

    if (font_body_ != nullptr) {
        ::DeleteObject(font_body_);
    }

    font_title_ = title;
    font_body_ = body;
    // 标题与小标题共用同一个字体对象：它们只是尺寸/字重相近，
    // 分成两个对象不会带来任何好处，反而多一份要释放的资源。
    font_heading_ = title;

    for (const HWND control : controls_) {
        ::SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font_body_), TRUE);
    }

    const auto set_font = [this](int id, HFONT font) {
        const auto position = std::find(ids_.begin(), ids_.end(), id);
        if (position != ids_.end()) {
            ::SendMessageW(controls_[static_cast<size_t>(position - ids_.begin())], WM_SETFONT,
                           reinterpret_cast<WPARAM>(font), TRUE);
        }
    };

    set_font(IDC_HEADING, font_title_);
}

int Wizard::scale(int dp) const {
    return ::MulDiv(dp, dpi_, 96);
}

int Wizard::point_to_pixels(int point) const {
    // 负高度表示「字符高度」，这是 CreateFont 的惯例；用 DPI 换算而不是固定 -12，
    // 否则 200% 下字体相对控件会偏小。
    return -::MulDiv(point, dpi_, 72);
}

void Wizard::on_dpi_changed(int dpi, const RECT* suggested) {
    if (dpi == 0) {
        return;
    }

    dpi_ = dpi;

    // PerMonitorV2 的约定：按系统给的建议矩形放置窗口（否则跨屏拖动后会显得错位）。
    if (suggested != nullptr) {
        ::SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                       suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
    }

    apply_fonts();
    layout();
    render_page();
}

void Wizard::layout() {
    const PageLayout current = layout_group(page_);

    for (size_t index = 0; index < controls_.size(); ++index) {
        const ControlSpec& spec = control_layout()[index];
        // 同意页的控件按「可见性」纵向回流（见 layout_consent）：它比固定坐标多一层
        // 逻辑，不能让后面的通用摆放再把它覆盖回静态位置。
        if (spec.group == PageLayout::Consent) {
            continue;
        }

        if (spec.group != PageLayout::Shell && spec.group != current) {
            continue;
        }

        ::SetWindowPos(controls_[index], nullptr, scale(spec.x), scale(spec.y), scale(spec.width),
                       scale(spec.height), SWP_NOZORDER | SWP_NOACTIVATE);
    }

    if (current == PageLayout::Consent) {
        layout_consent();
    }
}

void Wizard::move_control(int id, int x, int y, int width, int height) {
    const auto position = std::find(ids_.begin(), ids_.end(), id);
    if (position == ids_.end()) {
        return;
    }

    ::SetWindowPos(controls_[static_cast<size_t>(position - ids_.begin())], nullptr, scale(x), scale(y),
                   scale(width), scale(height), SWP_NOZORDER | SWP_NOACTIVATE);
}

void Wizard::layout_consent() {
    // 固定区域（证书详情 + 警告）仍以控件表为准；复选框区按可见性纵向回流。
    // 不这样做时，「我了解并同意」与「同时让本机所有用户都可用」之间会出现
    // 一百多 dp 的空白（截图里那两个孤零零的复选框就是这么来的）。
    const auto spec_of = [](int id) -> const ControlSpec* {
        for (const ControlSpec& spec : control_layout()) {
            if (spec.id == id) {
                return &spec;
            }
        }

        return nullptr;
    };

    const ControlSpec* details = spec_of(IDC_CONSENT_DETAILS);
    const ControlSpec* warning = spec_of(IDC_CONSENT_WARNING);
    const ControlSpec* accept = spec_of(IDC_CONSENT_ACCEPT);
    const ControlSpec* trust = spec_of(IDC_CONSENT_TRUST_PUBLISHER);
    const ControlSpec* unpinned = spec_of(IDC_CONSENT_UNPINNED_WARNING);
    const ControlSpec* all_users = spec_of(IDC_CONSENT_ALL_USERS);
    if (details == nullptr || warning == nullptr || accept == nullptr || trust == nullptr ||
        unpinned == nullptr || all_users == nullptr) {
        return;
    }

    move_control(details->id, details->x, details->y, details->width, details->height);
    move_control(warning->id, warning->x, warning->y, warning->width, warning->height);

    constexpr int kGap = 8;
    int y = accept->y;
    move_control(accept->id, accept->x, y, accept->width, accept->height);
    y += accept->height + kGap;

    if (is_visible(trust->id)) {
        move_control(trust->id, trust->x, y, trust->width, trust->height);
        y += trust->height + kGap;
    }

    if (is_visible(unpinned->id)) {
        move_control(unpinned->id, unpinned->x, y, unpinned->width, unpinned->height);
        y += unpinned->height + kGap;
    }

    if (is_visible(all_users->id)) {
        move_control(all_users->id, all_users->x, y, all_users->width, all_users->height);
    }
}

void Wizard::apply_texts() {
    ::SetWindowTextW(hwnd_, strings_.get(IDS_WINDOW_TITLE).data());

    // 只给「不随页面变化」的控件填一次固定文案；页面相关的文案由 render_* 负责。
    // 导航条的三个按钮也在这里兜一次底：即便某一页忘了设（或不该显示），
    // 也绝不会出现「有按钮没文字」的状态。
    set_text(IDC_NAV_BACK, strings_.get(IDS_COMMON_BACK));
    set_text(IDC_NAV_NEXT, strings_.get(IDS_COMMON_NEXT));
    set_text(IDC_NAV_CANCEL, strings_.get(IDS_COMMON_CANCEL));
    set_text(IDC_PROGRESS_CANCEL, strings_.get(IDS_COMMON_CANCEL));
    set_text(IDC_RESULT_OPEN_LOG, strings_.get(IDS_RESULT_OPEN_LOG));
    set_text(IDC_RESULT_COPY_DETAILS, strings_.get(IDS_RESULT_COPY_DETAILS));
    set_text(IDC_RESULT_LAUNCH, strings_.get(IDS_RESULT_LAUNCH));
    set_text(IDC_MANAGE_REPAIR, strings_.get(IDS_MANAGE_REPAIR));
    set_text(IDC_MANAGE_OPEN_LOGS, strings_.get(IDS_MANAGE_OPEN_LOGS));
    set_text(IDC_MANAGE_OPEN_SETTINGS, strings_.get(IDS_MANAGE_OPEN_SETTINGS));
    set_text(IDC_MANAGE_UNINSTALL, strings_.get(IDS_MANAGE_UNINSTALL));
    set_text(IDC_CONSENT_ACCEPT, strings_.get(IDS_CONSENT_ACCEPT));
    set_text(IDC_CONSENT_TRUST_PUBLISHER, strings_.get(IDS_CONSENT_TRUST_PUBLISHER));
    set_text(IDC_CONSENT_WARNING, strings_.get(IDS_CONSENT_WARNING));
    set_text(IDC_CONSENT_UNPINNED_WARNING, strings_.get(IDS_CONSENT_UNPINNED_WARNING));
    // 这一条曾经漏掉：复选框显示了、但标签是空的（截图验证抓到）。
    set_text(IDC_CONSENT_ALL_USERS, strings_.get(IDS_CONSENT_ALL_USERS));
}

void Wizard::set_text(int id, std::wstring_view text) {
    const auto position = std::find(ids_.begin(), ids_.end(), id);
    if (position == ids_.end()) {
        return;
    }

    const HWND control = controls_[static_cast<size_t>(position - ids_.begin())];
    ::SetWindowTextW(control, text.data());
    repaint_behind(control);
}

void Wizard::set_visible(int id, bool visible) {
    const auto position = std::find(ids_.begin(), ids_.end(), id);
    if (position == ids_.end()) {
        return;
    }

    const HWND control = controls_[static_cast<size_t>(position - ids_.begin())];
    ::ShowWindow(control, visible ? SW_SHOW : SW_HIDE);
    repaint_behind(control);
}

void Wizard::repaint_behind(HWND control) {
    // 材质生效时子控件的背景是透明的：控件自己重画不会擦掉上一次留下的字形，
    // 必须让父窗口先把那块区域重新清成 alpha=0（材质透出来），子控件再画一遍。
    // 少了这一步的典型症状：换了文案的静态文本新旧两行叠在一起。
    if (!backdrop_ || control == nullptr || hwnd_ == nullptr) {
        return;
    }

    RECT rect{};
    if (::GetWindowRect(control, &rect) == FALSE) {
        return;
    }

    ::MapWindowPoints(HWND_DESKTOP, hwnd_, reinterpret_cast<POINT*>(&rect), 2);
    ::RedrawWindow(hwnd_, &rect, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

bool Wizard::is_visible(int id) const {
    const auto position = std::find(ids_.begin(), ids_.end(), id);
    if (position == ids_.end()) {
        return false;
    }

    return ::IsWindowVisible(controls_[static_cast<size_t>(position - ids_.begin())]) != FALSE;
}

void Wizard::enable(int id, bool enabled) {
    const auto position = std::find(ids_.begin(), ids_.end(), id);
    if (position == ids_.end()) {
        return;
    }

    ::EnableWindow(controls_[static_cast<size_t>(position - ids_.begin())], enabled ? TRUE : FALSE);
}

bool Wizard::is_checked(int id) const {
    const auto position = std::find(ids_.begin(), ids_.end(), id);
    if (position == ids_.end()) {
        return false;
    }

    return ::SendMessageW(controls_[static_cast<size_t>(position - ids_.begin())], BM_GETCHECK, 0, 0) == BST_CHECKED;
}

int Wizard::selected_row() const {
    if (list_view_ == nullptr) {
        return -1;
    }

    return static_cast<int>(::SendMessageW(list_view_, LVM_GETNEXTITEM, static_cast<WPARAM>(-1), LVNI_SELECTED));
}

LRESULT Wizard::on_ctl_color(UINT message, HDC dc, HWND control) {
    // 材质生效时静态文本直接画在材质上：给它铺一块不透明底色，等于把 Mica 又盖回去
    // （每个标签变成一条纯色横条）。这里交回空心画刷 + 透明背景模式，只画字形。
    //
    // 只读编辑框也走 WM_CTLCOLORSTATIC，但它是可滚动的内容区：透明背景在滚动时
    // 会留下残影，所以按控件类把它排除在外，继续用不透明的底色。
    if (backdrop_ && message == WM_CTLCOLORSTATIC && !is_edit_class(control)) {
        ::SetTextColor(dc, color_text_);
        ::SetBkMode(dc, TRANSPARENT);
        return reinterpret_cast<LRESULT>(::GetStockObject(HOLLOW_BRUSH));
    }

    if (!dark_) {
        // 浅色模式交给系统处理：它知道编辑框 / 静态文本 / 列表的默认配色，
        // 我们插手只会把某些状态（如禁用文本）涂错。
        return ::DefWindowProcW(hwnd_, message, reinterpret_cast<WPARAM>(dc), 0);
    }

    ::SetTextColor(dc, color_text_);
    ::SetBkColor(dc, color_background_);
    return reinterpret_cast<LRESULT>(brush_edit_);
}

// ============================================================================
// 页面状态机
// ============================================================================

void Wizard::show_page(WizardPage page) {
    page_ = page;

    const PageLayout current = layout_group(page_);

    // 导航条不是「每页都一样」：
    //   摘要 / 同意 —— 上一步 + 下一步
    //   进度       —— 两个都不显示（进度页有自己的「取消」，否则会出现两个同名按钮）
    //   结果 / 管理 —— 只留「关闭」
    // 必须显式隐藏不用的按钮：控件是用 WS_CHILD（不带 WS_VISIBLE）建的，而文案只在
    // 需要它的页面上设置 —— 漏掉这一步的表现是界面上多出**空白按钮**（管理页曾出现两个）。
    const bool wants_back = page_ == WizardPage::Consent;
    const bool wants_next = page_ == WizardPage::Summary || page_ == WizardPage::Consent;
    const bool wants_cancel = page_ != WizardPage::Progress;

    for (size_t index = 0; index < controls_.size(); ++index) {
        const ControlSpec& spec = control_layout()[index];

        // 外壳与当前页的控件才显示；其余页的控件一律隐藏，避免上一页的状态
        // （例如列表里选中的行）泄漏到别的页面。
        bool show = spec.group == PageLayout::Shell || spec.group == current;
        if (spec.id == IDC_NAV_BACK) {
            show = show && wants_back;
        } else if (spec.id == IDC_NAV_NEXT) {
            show = show && wants_next;
        } else if (spec.id == IDC_NAV_CANCEL) {
            show = show && wants_cancel;
        }

        ::ShowWindow(controls_[index], show ? SW_SHOW : SW_HIDE);
    }

    ::InvalidateRect(hwnd_, nullptr, TRUE);
}

void Wizard::render_page() {
    PackageView preview;
    InstallView install;
    {
        const std::lock_guard<std::mutex> guard(shared_->mutex);
        preview = shared_->preview;
        install = shared_->install;
    }

    // 安装已经出结果 → 一律进结果页（编排线程用 kMsgInstallDone 通知，
    // 这里再判一次是为了覆盖「通知消息与页面切换交叉」的时序）。
    if (page_ == WizardPage::Progress && install.present) {
        exit_code_.store(install.exit_code);
        show_page(WizardPage::Result);
    }

    // 容器校验失败时把退出码也定下来：与 CLI 的「2 校验失败 / 4 I/O」一致。
    // 只记「最后一次载入的结果」，之后若真的装了，安装结果会覆盖它。
    if (!preview.ok && !preview.error_code.empty() && preview.exit_code != 0) {
        exit_code_.store(preview.exit_code);
    }

    switch (page_) {
        case WizardPage::Summary:
            render_summary();
            break;
        case WizardPage::Consent:
            render_consent();
            break;
        case WizardPage::Progress:
            render_progress();
            break;
        case WizardPage::Result:
            render_result();
            break;
        case WizardPage::Manage:
            render_manage();
            break;
    }

    layout();
}

// ---------- 页头 / 导航条 ----------

namespace {

struct PageChrome {
    int title;
    int heading;
    int subheading;
    bool show_back;
    bool show_next;
    bool show_cancel;  // 导航条上的取消
};

PageChrome chrome_for(WizardPage page) {
    switch (page) {
        case WizardPage::Summary: return {IDS_SUMMARY_TITLE, IDS_SUMMARY_HEADING, IDS_SUMMARY_SUBHEADING, false, true, true};
        case WizardPage::Consent: return {IDS_CONSENT_TITLE, IDS_CONSENT_HEADING, IDS_CONSENT_SUBHEADING, true, true, true};
        case WizardPage::Progress:
            return {IDS_PROGRESS_TITLE, IDS_PROGRESS_HEADING, IDS_PROGRESS_SUBHEADING, false, false, false};
        case WizardPage::Result: return {IDS_RESULT_TITLE, IDS_RESULT_SUCCESS_HEADING, IDS_RESULT_LOG_PATH, false, false, true};
        case WizardPage::Manage: return {IDS_MANAGE_TITLE, IDS_MANAGE_HEADING, IDS_MANAGE_SUBHEADING, false, false, true};
    }

    return {IDS_WINDOW_TITLE, IDS_WINDOW_TITLE, IDS_WINDOW_TITLE, false, false, true};
}

}  // namespace

// ============================================================================
// 摘要页
// ============================================================================

void Wizard::render_summary() {
    const PageChrome chrome = chrome_for(WizardPage::Summary);
    set_text(IDC_HEADING, strings_.get(chrome.heading));
    set_text(IDC_SUBHEADING, strings_.get(chrome.subheading));
    set_text(IDC_NAV_BACK, strings_.get(IDS_COMMON_BACK));
    set_text(IDC_NAV_NEXT, strings_.get(IDS_COMMON_NEXT));

    PackageView view;
    {
        const std::lock_guard<std::mutex> guard(shared_->mutex);
        view = shared_->preview;
    }

    set_text(IDC_SUMMARY_LABEL_NAME, strings_.get(IDS_SUMMARY_LABEL_NAME));
    set_text(IDC_SUMMARY_LABEL_VERSION, strings_.get(IDS_SUMMARY_LABEL_VERSION));
    set_text(IDC_SUMMARY_LABEL_ARCH, strings_.get(IDS_SUMMARY_LABEL_ARCH));
    set_text(IDC_SUMMARY_LABEL_PUBLISHER, strings_.get(IDS_SUMMARY_LABEL_PUBLISHER));
    set_text(IDC_SUMMARY_LABEL_SIZE, strings_.get(IDS_SUMMARY_LABEL_SIZE));
    set_text(IDC_SUMMARY_LABEL_DEPS, strings_.get(IDS_SUMMARY_LABEL_DEPS));
    set_text(IDC_SUMMARY_LABEL_DESCRIPTION, strings_.get(IDS_SUMMARY_LABEL_DESCRIPTION));

    // 还没有容器，或者容器没能载入：两种情况的文案不同 ——
    // 「没给路径」要提示把文件拖进来，「路径给了但坏了」要直接给错误码与原因。
    // 让用户在信息不全的情况下点进同意页是安全页面上最不该做的事，所以下一步一并禁用。
    if (!view.ok) {
        const bool load_failed = !view.error_code.empty();
        set_text(IDC_SUMMARY_NAME,
                 strings_.get(load_failed ? IDS_ERROR_LOAD_FAILED : IDS_ERROR_NO_FILE));
        set_text(IDC_SUMMARY_VERSION, strings_.get(IDS_COMMON_NONE));
        set_text(IDC_SUMMARY_ARCH, strings_.get(IDS_COMMON_NONE));
        set_text(IDC_SUMMARY_PUBLISHER, strings_.get(IDS_COMMON_NONE));
        set_text(IDC_SUMMARY_SIZE, strings_.get(IDS_COMMON_NONE));
        set_text(IDC_SUMMARY_DEPS, strings_.get(IDS_COMMON_NONE));
        set_text(IDC_SUMMARY_DESCRIPTION, strings_.get(IDS_COMMON_NONE));

        if (load_failed && !view.error_message.empty() && view.error_message != L"cancelled") {
            std::wstring line = view.error_code;
            line += L"\r\n";
            line += view.error_message;
            set_text(IDC_SUMMARY_NOTE, line);
            set_visible(IDC_SUMMARY_NOTE, true);
        } else {
            set_visible(IDC_SUMMARY_NOTE, false);
        }

        enable(IDC_NAV_NEXT, false);
        return;
    }

    set_text(IDC_SUMMARY_NAME, msixpkg::widen_utf8(view.display_name));
    set_text(IDC_SUMMARY_VERSION, msixpkg::widen_utf8(view.version));
    set_text(IDC_SUMMARY_ARCH, msixpkg::widen_utf8(view.architecture));
    set_text(IDC_SUMMARY_PUBLISHER, msixpkg::widen_utf8(view.publisher));
    set_text(IDC_SUMMARY_SIZE, msixpkg::gui::format_size(view.total_size));

    if (view.dependencies.empty()) {
        set_text(IDC_SUMMARY_DEPS, strings_.get(IDS_SUMMARY_DEPS_NONE));
    } else {
        std::wstring joined;
        for (size_t index = 0; index < view.dependencies.size(); ++index) {
            if (index != 0) {
                joined += L"; ";
            }

            joined += msixpkg::widen_utf8(view.dependencies[index]);
        }

        set_text(IDC_SUMMARY_DEPS, joined);
    }

    set_text(IDC_SUMMARY_DESCRIPTION, view.description.empty() ? strings_.get(IDS_COMMON_NONE)
                                                              : msixpkg::widen_utf8(view.description));

    if (view.already_installed) {
        set_text(IDC_SUMMARY_NOTE, strings_.get(IDS_SUMMARY_UPDATING));
        set_visible(IDC_SUMMARY_NOTE, true);
    } else {
        set_visible(IDC_SUMMARY_NOTE, false);
    }

    enable(IDC_NAV_NEXT, true);
}

// ============================================================================
// 同意页（安全关键）
// ============================================================================

void Wizard::render_consent() {
    const PageChrome chrome = chrome_for(WizardPage::Consent);
    set_text(IDC_HEADING, strings_.get(chrome.heading));
    set_text(IDC_SUBHEADING, strings_.get(chrome.subheading));
    set_text(IDC_NAV_BACK, strings_.get(IDS_COMMON_BACK));
    set_text(IDC_NAV_NEXT, strings_.get(IDS_COMMON_NEXT));

    PackageView view;
    {
        const std::lock_guard<std::mutex> guard(shared_->mutex);
        view = shared_->preview;
    }

    // 「将要信任谁」讲清楚：每一条都是用户做出决定所必需的事实，而不是装饰。
    std::wstring details;
    const auto line = [&details](std::wstring_view label, const std::wstring& value) {
        details += label;
        details += L": ";
        details += value;
        details += L"\r\n";
    };

    line(strings_.get(IDS_CONSENT_LABEL_SUBJECT), view.subject.empty() ? strings_.text(IDS_COMMON_NONE)
                                                                     : msixpkg::widen_utf8(view.subject));

    if (view.has_certificate) {
        // 指纹按字节分组显示：64 个字符连成一片时人眼几乎无法核对。
        line(strings_.get(IDS_CONSENT_LABEL_SHA256),
             msixpkg::gui::format_fingerprint(msixpkg::widen_utf8(view.thumbprint_sha256)));
        line(strings_.get(IDS_CONSENT_LABEL_SHA1),
             msixpkg::gui::format_fingerprint(msixpkg::widen_utf8(view.thumbprint_sha1)));

        std::wstring validity = msixpkg::widen_utf8(view.not_before_utc);
        if (!view.not_after_utc.empty()) {
            validity += L"  \u2192  " + msixpkg::widen_utf8(view.not_after_utc);
        }

        line(strings_.get(IDS_CONSENT_LABEL_VALID), validity.empty() ? strings_.text(IDS_COMMON_NONE) : validity);
        line(strings_.get(IDS_CONSENT_LABEL_TRUSTED),
             strings_.text(view.already_trusted ? IDS_CONSENT_TRUSTED_YES : IDS_CONSENT_TRUSTED_NO));
        line(strings_.get(IDS_CONSENT_LABEL_PINNED),
             strings_.text(view.pinned ? IDS_CONSENT_PINNED_YES : IDS_CONSENT_PINNED_NO));
    } else {
        // 没有内嵌证书：安装器完全不碰证书存储（安全模型 §1），这一点必须说清楚，
        // 否则用户会以为「没有证书 = 不安全」而放弃安装。
        line(strings_.get(IDS_CONSENT_LABEL_SHA256), strings_.text(IDS_COMMON_NONE));
        line(strings_.get(IDS_CONSENT_LABEL_SHA1), strings_.text(IDS_COMMON_NONE));
        line(strings_.get(IDS_CONSENT_LABEL_TRUSTED), strings_.text(IDS_CONSENT_TRUSTED_YES));
        line(strings_.get(IDS_CONSENT_LABEL_PINNED), strings_.text(IDS_CONSENT_PINNED_NO));
    }

    set_text(IDC_CONSENT_DETAILS, details);

    // --all-users 只在清单声明允许时可用；默认不勾。
    set_visible(IDC_CONSENT_ALL_USERS, view.allow_all_users);
    enable(IDC_CONSENT_ALL_USERS, view.allow_all_users);
    const auto uncheck = [this](int id) {
        const auto position = std::find(ids_.begin(), ids_.end(), id);
        if (position != ids_.end()) {
            ::SendMessageW(controls_[static_cast<size_t>(position - ids_.begin())], BM_SETCHECK, BST_UNCHECKED, 0);
        }
    };
    uncheck(IDC_CONSENT_ALL_USERS);

    // 「信任此发布者」只在未命中钉扎时出现：命中白名单的发布者不需要用户为它承担
    // 额外的信任决定（那正是钉扎的意义）。
    set_visible(IDC_CONSENT_TRUST_PUBLISHER, view.unpinned_publisher);
    set_visible(IDC_CONSENT_UNPINNED_WARNING, view.unpinned_publisher);

    // 每次进同意页都把两个复选框清空：上一次的勾选不能替这一次做决定。
    uncheck(IDC_CONSENT_ACCEPT);
    uncheck(IDC_CONSENT_TRUST_PUBLISHER);

    update_consent_gate();
}

void Wizard::update_consent_gate() {
    PackageView view;
    {
        const std::lock_guard<std::mutex> guard(shared_->mutex);
        view = shared_->preview;
    }

    const bool accepted = is_checked(IDC_CONSENT_ACCEPT);
    // 未命中钉扎时必须**额外**显式信任，这是红线 2：白名单之外的发布者绝不能靠
    // 一个「同意安装」的动作顺带被信任。
    const bool trusted = view.unpinned_publisher ? is_checked(IDC_CONSENT_TRUST_PUBLISHER) : true;

    // 安装按钮（同意页上的「下一步」）未勾选就禁用。用 EnableWindow 而不是直接
    // 隐藏：控件仍在 Tab 顺序里，屏幕阅读器也能读到「不可用」，比藏起来更可理解。
    enable(IDC_NAV_NEXT, accepted && trusted);

    // 「信任此发布者」本身也要先勾「我了解并同意」——否则用户可能在没读警告的情况下
    // 直接勾上第二个框。
    enable(IDC_CONSENT_TRUST_PUBLISHER, accepted);
}

// ============================================================================
// 进度页
// ============================================================================

namespace {

struct PhaseName {
    PhaseView phase;
    int name_id;
};

/// 阶段列表：安装流程里真正会出现的阶段（顺序即执行顺序）。
const PhaseName kPhaseNames[] = {
    {PhaseView::Validate, IDS_PROGRESS_PHASE_VALIDATE},
    {PhaseView::Stage, IDS_PROGRESS_PHASE_STAGE},
    {PhaseView::Certificate, IDS_PROGRESS_PHASE_CERTIFICATE},
    {PhaseView::Dependencies, IDS_PROGRESS_PHASE_DEPENDENCIES},
    {PhaseView::MainPackage, IDS_PROGRESS_PHASE_MAIN},
    {PhaseView::Provision, IDS_PROGRESS_PHASE_PROVISION},
    {PhaseView::Reclaim, IDS_PROGRESS_PHASE_RECLAIM},
    {PhaseView::Done, IDS_PROGRESS_PHASE_DONE},
};

int state_text_id(PhaseView phase, PhaseView current) {
    if (phase == PhaseView::Failed) {
        return IDS_PROGRESS_FAILED;
    }

    // 执行顺序就是枚举顺序；回滚与收尾排在最后两档。
    const auto order = [](PhaseView value) -> int {
        if (value == PhaseView::Rollback) {
            return 10;
        }

        if (value == PhaseView::Done) {
            return 9;
        }

        return static_cast<int>(value);
    };

    const int here = order(current);
    const int there = order(phase);
    if (there < here) {
        return IDS_PROGRESS_COMPLETE;
    }

    if (there == here) {
        return IDS_PROGRESS_ACTIVE;
    }

    return IDS_PROGRESS_PENDING;
}

}  // namespace

void Wizard::build_phase_list() {
    PhaseView current = PhaseView::Idle;
    std::wstring log;
    {
        const std::lock_guard<std::mutex> guard(shared_->mutex);
        current = shared_->phase;
        for (const std::wstring& entry : shared_->phase_log) {
            log += entry;
            log += L"\r\n";
        }
    }

    if (current == PhaseView::Failed) {
        // 失败时把整张表都按「已完成/失败」标出，用户能看到停在哪一步。
        current = PhaseView::Failed;
    }

    std::wstring text;
    for (const PhaseName& entry : kPhaseNames) {
        const int state = state_text_id(entry.phase, current);
        text += strings_.get(state);
        text += L"\t";
        text += strings_.get(entry.name_id);
        text += L"\r\n";
    }

    text += L"\r\n";
    if (!log.empty()) {
        text += log;
    }

    set_text(IDC_PROGRESS_PHASES, text);
}

void Wizard::render_progress() {
    const PageChrome chrome = chrome_for(WizardPage::Progress);
    set_text(IDC_HEADING, installing_ ? strings_.get(chrome.heading) : strings_.get(IDS_PROGRESS_UNINSTALL_HEADING));
    set_text(IDC_SUBHEADING, strings_.get(chrome.subheading));

    uint32_t percent = 0;
    bool waiting = false;
    {
        const std::lock_guard<std::mutex> guard(shared_->mutex);
        percent = shared_->percent;
        waiting = shared_->waiting_for_elevation;
    }

    // 进度条的 PBM_SETRANGE 是 16 位；PBM_SETRANGE32 才是 32 位值。
    const HWND bar = controls_[static_cast<size_t>(std::find(ids_.begin(), ids_.end(), IDC_PROGRESS_BAR) -
                                                   ids_.begin())];
    ::SendMessageW(bar, PBM_SETRANGE32, 0, 100);
    ::SendMessageW(bar, PBM_SETPOS, static_cast<WPARAM>(std::min<uint32_t>(percent, 100)), 0);

    set_text(IDC_PROGRESS_PERCENT, msixpkg::gui::format(strings_.get(IDS_PROGRESS_PERCENT).data(), percent));
    set_text(IDC_PROGRESS_STATUS,
             waiting ? strings_.get(IDS_PROGRESS_WAITING_ELEVATION) : strings_.get(IDS_PROGRESS_SUBHEADING));

    build_phase_list();
}

// ============================================================================
// 结果页
// ============================================================================

/// 结果页正文：成功 / 失败 / 取消三种形态各说清楚「系统现在是什么状态」。
std::wstring Wizard::result_body(const InstallView& view) const {
    std::wstring body;
    if (view.status == InstallStatus::Success) {
        body = strings_.text(IDS_RESULT_SUCCESS_BODY);
    } else if (view.status == InstallStatus::Cancelled) {
        body = strings_.text(view.elevation_declined ? IDS_RESULT_ELEVATION_DECLINED_BODY
                                                     : IDS_RESULT_CANCELLED_BODY);
    } else {
        body = view.error_code;
        body += L": ";
        body += view.error_message;
        if (!view.error_detail.empty()) {
            body += L"\r\n";
            body += view.error_detail;
        }

        if (view.hint_id != 0) {
            body += L"\r\n\r\n";
            body += strings_.get(view.hint_id);
        }
    }

    return body;
}

void Wizard::render_result() {
    InstallView view;
    {
        const std::lock_guard<std::mutex> guard(shared_->mutex);
        view = shared_->install;
    }

    int heading = IDS_RESULT_SUCCESS_HEADING;
    if (view.status == InstallStatus::Cancelled) {
        heading = IDS_RESULT_CANCELLED_HEADING;
    } else if (view.status == InstallStatus::Failure) {
        heading = IDS_RESULT_FAILURE_HEADING;
    }

    set_text(IDC_HEADING, strings_.get(heading));
    set_text(IDC_SUBHEADING, strings_.get(IDS_RESULT_LOG_PATH));
    set_text(IDC_NAV_CANCEL, strings_.get(IDS_COMMON_CLOSE));
    set_text(IDC_RESULT_BODY, result_body(view));

    if (view.log_path.empty()) {
        set_text(IDC_RESULT_LOG, strings_.get(IDS_COMMON_NONE));
    } else {
        set_text(IDC_RESULT_LOG, strings_.text(IDS_RESULT_LOG_PATH) + L": " + view.log_path);
    }

    // 「启动应用」需要 AUMID 的两半；缺任何一半就禁用（而不是点了没反应）。
    const bool can_launch = view.status == InstallStatus::Success && !view.family_name.empty() &&
                            !view.application_id.empty();
    enable(IDC_RESULT_LAUNCH, can_launch);

    enable(IDC_RESULT_OPEN_LOG, !view.log_path.empty());
    enable(IDC_RESULT_COPY_DETAILS, true);
}

// ============================================================================
// 管理页
// ============================================================================

void Wizard::render_manage() {
    const PageChrome chrome = chrome_for(WizardPage::Manage);
    set_text(IDC_HEADING, strings_.get(chrome.heading));
    set_text(IDC_SUBHEADING, strings_.get(chrome.subheading));
    set_text(IDC_NAV_CANCEL, strings_.get(IDS_COMMON_CLOSE));

    ManageView view;
    {
        const std::lock_guard<std::mutex> guard(shared_->mutex);
        view = shared_->manage;
    }

    if (list_view_ == nullptr) {
        return;
    }

    ::SendMessageW(list_view_, WM_SETREDRAW, FALSE, 0);
    ::SendMessageW(list_view_, LVM_DELETEALLITEMS, 0, 0);

    for (size_t index = 0; index < view.entries.size(); ++index) {
        const ManageEntry& entry = view.entries[index];
        int status_id = IDS_MANAGE_STATUS_MISSING;
        if (entry.status == L"present") {
            status_id = IDS_MANAGE_STATUS_PRESENT;
        } else if (entry.status == L"partial") {
            status_id = IDS_MANAGE_STATUS_PARTIAL;
        } else if (entry.status == L"failed") {
            status_id = IDS_MANAGE_STATUS_FAILED;
        }

        // 证书指纹只显示前 16 个字符：完整 40 个字符会把列挤爆，
        // 而这一列的作用是「分清哪几个包共用一张证书」。
        std::wstring certificate = entry.certificate_sha1;
        if (certificate.size() > 16) {
            certificate = certificate.substr(0, 16) + L"…";
        }

        if (certificate.empty()) {
            certificate = strings_.get(IDS_COMMON_NONE);
        }

        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = static_cast<int>(index);
        item.iSubItem = 0;
        item.pszText = const_cast<wchar_t*>(entry.name.c_str());
        const int row = static_cast<int>(::SendMessageW(list_view_, LVM_INSERTITEMW, 0,
                                                        reinterpret_cast<LPARAM>(&item)));
        if (row < 0) {
            continue;
        }

        const auto set_column = [this, row](int column, const std::wstring& text) {
            LVITEMW sub{};
            sub.mask = LVIF_TEXT;
            sub.iItem = row;
            sub.iSubItem = column;
            sub.pszText = const_cast<wchar_t*>(text.c_str());
            ::SendMessageW(list_view_, LVM_SETITEMW, 0, reinterpret_cast<LPARAM>(&sub));
        };

        set_column(1, entry.version);
        set_column(2, certificate);
        set_column(3, strings_.text(status_id));
    }

    ::SendMessageW(list_view_, WM_SETREDRAW, TRUE, 0);
    ::InvalidateRect(list_view_, nullptr, TRUE);

    if (view.entries.empty()) {
        set_text(IDC_MANAGE_DETAIL, view.notice.empty() ? strings_.get(IDS_MANAGE_EMPTY) : view.notice);
        enable(IDC_MANAGE_UNINSTALL, false);
    } else {
        if (!view.notice.empty()) {
            set_text(IDC_MANAGE_DETAIL, view.notice);
        }

        enable(IDC_MANAGE_UNINSTALL, selected_row() >= 0);
    }
}

// ============================================================================
// 命令路由
// ============================================================================

LRESULT Wizard::on_command(WORD id, WORD notification, HWND control) {
    (void)control;

    // 三个安全门的实时联动：勾选状态一变就重新判定安装按钮是否可用。
    if (id == IDC_CONSENT_ACCEPT && notification == BN_CLICKED) {
        update_consent_gate();
        return 0;
    }

    if (id == IDC_CONSENT_TRUST_PUBLISHER && notification == BN_CLICKED) {
        update_consent_gate();
        return 0;
    }

    if (notification != BN_CLICKED) {
        return 0;
    }

    switch (id) {
        case IDC_NAV_NEXT:
            if (page_ == WizardPage::Summary) {
                show_page(WizardPage::Consent);
                render_page();
                // 键盘用户直接从第一个复选框开始，不用先 Tab 过只读的详情框。
                const auto accept = std::find(ids_.begin(), ids_.end(), IDC_CONSENT_ACCEPT);
                if (accept != ids_.end()) {
                    ::SetFocus(controls_[static_cast<size_t>(accept - ids_.begin())]);
                }
            } else if (page_ == WizardPage::Consent) {
                // 未勾选时按钮本来就是禁用的；这里再挡一次，防止程序化触发。
                if (!is_checked(IDC_CONSENT_ACCEPT)) {
                    ::MessageBoxW(hwnd_, strings_.get(IDS_CONSENT_NEED_ACCEPT).data(),
                                  strings_.get(IDS_CONSENT_TITLE).data(), MB_OK | MB_ICONWARNING);
                    return 0;
                }

                PackageView view;
                {
                    const std::lock_guard<std::mutex> guard(shared_->mutex);
                    view = shared_->preview;
                }

                if (view.unpinned_publisher && !is_checked(IDC_CONSENT_TRUST_PUBLISHER)) {
                    ::MessageBoxW(hwnd_, strings_.get(IDS_CONSENT_NEED_TRUST).data(),
                                  strings_.get(IDS_CONSENT_TITLE).data(), MB_OK | MB_ICONWARNING);
                    return 0;
                }

                submit_consent(true);
            }

            return 0;

        case IDC_NAV_BACK:
            if (page_ == WizardPage::Consent) {
                // 还没开始装：撤回本次同意请求（编排线程会拿到「拒绝」并正常退出），
                // 然后回到摘要页。这样不会留下任何需要回滚的东西。
                stop_worker(worker_);
                submit_consent(false);
                show_page(WizardPage::Summary);
                render_page();
            }

            return 0;

        case IDC_NAV_CANCEL:
            if (page_ == WizardPage::Progress) {
                cancel_install();
            } else if (page_ == WizardPage::Result) {
                ::DestroyWindow(hwnd_);
            } else {
                // 摘要是「还没做任何事」，同意是「还没开始装」：都可以直接关。
                ::DestroyWindow(hwnd_);
            }

            return 0;

        case IDC_PROGRESS_CANCEL:
            cancel_install();
            return 0;

        case IDC_RESULT_LAUNCH:
            launch_application();
            return 0;

        case IDC_RESULT_OPEN_LOG:
            open_log_file();
            return 0;

        case IDC_RESULT_COPY_DETAILS: {
            InstallView view;
            {
                const std::lock_guard<std::mutex> guard(shared_->mutex);
                view = shared_->install;
            }

            std::wstring details = result_body(view);
            if (!view.log_path.empty()) {
                details += L"\r\n";
                details += strings_.get(IDS_RESULT_LOG_PATH);
                details += L": ";
                details += view.log_path;
            }

            copy_to_clipboard(details);
            return 0;
        }

        case IDC_MANAGE_UNINSTALL:
            begin_uninstall();
            return 0;

        case IDC_MANAGE_REPAIR:
            show_repair_plan();
            return 0;

        case IDC_MANAGE_OPEN_LOGS:
            open_log_directory();
            return 0;

        case IDC_MANAGE_OPEN_SETTINGS:
            open_settings_apps();
            return 0;

        default:
            return 0;
    }
}

LRESULT Wizard::on_notify(NMHDR* header) {
    if (header == nullptr) {
        return 0;
    }

    // 带主题的复选框由 comctl32 自己画文字，WM_CTLCOLOR* 的 SetTextColor 对它无效
    // （深色模式下就是深底黑字）。NM_CUSTOMDRAW 是官方给出的改写点：保留主题画出来的
    // 勾选框，只把文字换成跟随主题的颜色。浅色模式不接管，避免破坏系统默认状态。
    if (header->code == NM_CUSTOMDRAW && dark_) {
        const LONG_PTR style = header->hwndFrom != nullptr ? ::GetWindowLongPtrW(header->hwndFrom, GWL_STYLE) : 0;
        // 复选框的文字由主题画成黑色，且 WM_CTLCOLOR* 改不动它；这里只接管这一类控件，
        // 普通按钮、进度条、列表的 NM_CUSTOMDRAW 仍旧走默认实现。
        if ((style & BS_TYPEMASK) == BS_AUTOCHECKBOX) {
            auto* draw = reinterpret_cast<NMCUSTOMDRAW*>(header);
            if (draw->dwDrawStage == CDDS_PREPAINT) {
                return draw_dark_checkbox(*draw, color_text_);
            }

            return CDRF_DODEFAULT;
        }
    }

    if (header->hwndFrom == list_view_ && header->code == LVN_ITEMCHANGED) {
        enable(IDC_MANAGE_UNINSTALL, selected_row() >= 0);
        return 0;
    }

    return 0;
}

LRESULT Wizard::on_copy_data(const COPYDATASTRUCT* data) {
    if (data == nullptr || data->dwData != kCopyDataMagic || data->lpData == nullptr) {
        return FALSE;
    }

    // WM_COPYDATA 的缓冲只在处理这条消息期间有效：这里立刻拷成 std::wstring。
    // 发送方（run()）保证是 NUL 结尾的宽字符串。
    const auto* text = static_cast<const wchar_t*>(data->lpData);
    const size_t length = data->cbData / sizeof(wchar_t);
    std::wstring container(text, length);
    while (!container.empty() && container.back() == L'\0') {
        container.pop_back();
    }

    if (container.empty()) {
        return TRUE;
    }

    // 装东西的时候绝不能在这里等编排线程：发送方是同步 SendMessage，它要等我们
    // 返回；而我们要等编排线程；编排线程可能正等着那个发送方退出 —— 三方互等。
    // 回 BUSY 让发送方立刻知道结果（它自己会提示用户、并以 kPrecondition 退出）。
    if (worker_.busy()) {
        return static_cast<LRESULT>(kCopyDataBusy);
    }

    adopt_container(container);
    return TRUE;
}

LRESULT Wizard::on_close() {
    // 装到一半不许直接关：那会让用户以为「已经取消了」，而编排线程还在改系统。
    if (page_ == WizardPage::Progress && worker_.busy()) {
        const int answer = ::MessageBoxW(hwnd_, strings_.get(IDS_PROGRESS_SUBHEADING).data(),
                                         strings_.get(IDS_PROGRESS_TITLE).data(), MB_OKCANCEL | MB_ICONWARNING);
        if (answer != IDOK) {
            return 0;
        }

        cancel_install();
    }

    ::DestroyWindow(hwnd_);
    return 0;
}

// ============================================================================
// 动作
// ============================================================================

void Wizard::begin_preview(const std::wstring& container) {
    {
        const std::lock_guard<std::mutex> guard(shared_->mutex);
        shared_->container_path = container;
        // 换容器时把上一次的结果清掉，否则结果页会显示上一个包的结论。
        shared_->preview = PackageView{};
        shared_->install = InstallView{};
        shared_->percent = 0;
        shared_->phase = PhaseView::Idle;
        shared_->waiting_for_elevation = false;
        shared_->phase_log.clear();
        shared_->preapproved_consent.reset();
    }

    // 换容器就把上一次的结论清掉：新的一次载入会自己给出退出码
    // （校验失败 → 2/4；成功则保持 0，直到安装给出真正的结果）。
    exit_code_.store(exit_code::kSuccess);
    show_page(WizardPage::Summary);
    render_page();
    start_preview(worker_, hwnd_, container);
}

void Wizard::adopt_container(const std::wstring& container) {
    begin_preview(container);
    dropped_in_ = true;
}

void Wizard::begin_install(const ConsentAnswers& answers) {
    {
        const std::lock_guard<std::mutex> guard(shared_->mutex);
        shared_->install = InstallView{};
        shared_->percent = 0;
        shared_->phase = PhaseView::Validate;
        shared_->waiting_for_elevation = false;
        shared_->phase_log.clear();
        // 先放进信箱再启动线程：编排线程可能在 UI 还没返回时就已经走到 Ask。
        // GuiConsent::Ask 会优先消费它，因此同意页不会被第二次推上来。
        shared_->preapproved_consent = answers;
    }

    installing_ = true;
    elevation_foregrounded_ = false;
    show_page(WizardPage::Progress);
    render_page();
    start_install(worker_, hwnd_);
}

void Wizard::submit_consent(bool approved) {
    std::shared_ptr<PendingConsent> pending;
    {
        const std::lock_guard<std::mutex> guard(shared_->mutex);
        pending = shared_->consent;
    }

    if (pending == nullptr) {
        // 没有待决问题（例如用户是在摘要页按「下一步」直接走到这里的）：
        // 把这次决定随安装线程一起带过去，而不是先启动安装、再由安装线程
        // 发一次同意请求 —— 那正是「要点两次下一步」的来源。
        if (!approved) {
            {
                const std::lock_guard<std::mutex> guard(shared_->mutex);
                shared_->preapproved_consent.reset();
            }
            exit_code_.store(exit_code::kCancelled);
        } else {
            ConsentAnswers answers;
            answers.approved = is_checked(IDC_CONSENT_ACCEPT);
            answers.remember_publisher = is_checked(IDC_CONSENT_TRUST_PUBLISHER);
            answers.all_users = is_checked(IDC_CONSENT_ALL_USERS);
            begin_install(answers);
        }

        return;
    }

    if (!approved) {
        pending->approved = false;
        pending->remember_publisher = false;
        pending->all_users = false;
        pending->answered.store(true);
        // 用户在同意页上撤回：这就是 CLI 里的「取消」（退出码 6）。
        exit_code_.store(exit_code::kCancelled);
        return;
    }

    // 「信任此发布者」是 ConsentDecision.remember_publisher 的唯一来源：
    // 只有用户手勾了它，未命中钉扎的发布者才会被继续处理（并写审计日志）。
    pending->approved = is_checked(IDC_CONSENT_ACCEPT);
    pending->remember_publisher = is_checked(IDC_CONSENT_TRUST_PUBLISHER);
    pending->all_users = is_checked(IDC_CONSENT_ALL_USERS);
    pending->answered.store(true);

    // 编排线程此刻还在等这个答案，安装将在它那边继续。
    installing_ = true;
    show_page(WizardPage::Progress);
    render_page();
}

void Wizard::cancel_install() {
    shared_->cancel.store(true);
    // 关窗 / 点取消都走这里：编排线程会以「被取消」收场，但用户可能没等到它
    // 给出结果就把窗口关了，所以退出码在这里先定下来。
    exit_code_.store(exit_code::kCancelled);
    set_text(IDC_PROGRESS_STATUS, strings_.get(IDS_COMMON_CANCEL));
    enable(IDC_PROGRESS_CANCEL, false);
}

void Wizard::begin_manage_load() {
    {
        const std::lock_guard<std::mutex> guard(shared_->mutex);
        shared_->manage = ManageView{};
    }

    start_manage_load(worker_, hwnd_);
}

void Wizard::begin_uninstall() {
    const int row = selected_row();
    if (row < 0) {
        ::MessageBoxW(hwnd_, strings_.get(IDS_MANAGE_NO_SELECTION).data(), strings_.get(IDS_MANAGE_TITLE).data(),
                      MB_OK | MB_ICONINFORMATION);
        return;
    }

    const int answer = ::MessageBoxW(hwnd_, strings_.get(IDS_MANAGE_CONFIRM_UNINSTALL).data(),
                                     strings_.get(IDS_MANAGE_TITLE).data(), MB_YESNO | MB_ICONWARNING);
    if (answer != IDYES) {
        return;
    }

    ManageView view;
    {
        const std::lock_guard<std::mutex> guard(shared_->mutex);
        view = shared_->manage;
    }

    if (row >= static_cast<int>(view.entries.size())) {
        return;
    }

    const ManageEntry& entry = view.entries[static_cast<size_t>(row)];
    begin_uninstall_for(entry.install_id, entry.name);
}

void Wizard::begin_uninstall_for(const std::string& install_id, const std::wstring& family_name) {
    {
        const std::lock_guard<std::mutex> guard(shared_->mutex);
        shared_->install = InstallView{};
        shared_->percent = 0;
        shared_->phase = PhaseView::Idle;
        shared_->waiting_for_elevation = false;
        shared_->phase_log.clear();
    }

    installing_ = false;
    elevation_foregrounded_ = false;
    show_page(WizardPage::Progress);
    render_page();
    start_uninstall(worker_, hwnd_, install_id, family_name);
}

void Wizard::open_log_file() {
    InstallView view;
    {
        const std::lock_guard<std::mutex> guard(shared_->mutex);
        view = shared_->install;
    }

    if (view.log_path.empty()) {
        return;
    }

    // ShellExecute 的 lpFile 用宽字符路径；动作是 open（让用户配置的编辑器接手）。
    const HINSTANCE result = ::ShellExecuteW(hwnd_, L"open", view.log_path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(result) <= 32) {
        ::MessageBoxW(hwnd_, strings_.get(IDS_ERROR_NOT_FOUND).data(), strings_.get(IDS_RESULT_OPEN_LOG).data(),
                      MB_OK | MB_ICONWARNING);
    }
}

void Wizard::open_log_directory() {
    ManageView view;
    {
        const std::lock_guard<std::mutex> guard(shared_->mutex);
        view = shared_->manage;
    }

    // 直接开数据根目录：日志文件名带时间戳，逐个列举没有意义，让用户自己在
    // 资源管理器里挑最新的一份。
    const std::wstring target = view.data_root.empty() ? std::wstring() : view.data_root + L"\\logs";
    if (target.empty()) {
        return;
    }

    const HINSTANCE result = ::ShellExecuteW(hwnd_, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(result) <= 32) {
        ::MessageBoxW(hwnd_, strings_.get(IDS_ERROR_NOT_FOUND).data(), strings_.get(IDS_MANAGE_OPEN_LOGS).data(),
                      MB_OK | MB_ICONWARNING);
    }
}

void Wizard::launch_application() {
    InstallView view;
    {
        const std::lock_guard<std::mutex> guard(shared_->mutex);
        view = shared_->install;
    }

    if (view.family_name.empty() || view.application_id.empty()) {
        return;
    }

    // AUMID = <PackageFamilyName>!<ApplicationId>（docs/installer-design.md §8）。
    const std::wstring aumid = view.family_name + L"!" + msixpkg::widen_utf8(view.application_id);
    try {
        msixpkg::activate_application(msixpkg::narrow_utf8(aumid));
    } catch (const msixpkg::Error&) {
        ::MessageBoxW(hwnd_, strings_.get(IDS_RESULT_LAUNCH_FAILED).data(), strings_.get(IDS_RESULT_LAUNCH).data(),
                      MB_OK | MB_ICONWARNING);
    } catch (const std::exception&) {
        ::MessageBoxW(hwnd_, strings_.get(IDS_RESULT_LAUNCH_FAILED).data(), strings_.get(IDS_RESULT_LAUNCH).data(),
                      MB_OK | MB_ICONWARNING);
    }
}

void Wizard::copy_to_clipboard(const std::wstring& text) const {
    if (text.empty() || ::OpenClipboard(hwnd_) == FALSE) {
        return;
    }

    ::EmptyClipboard();
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL block = ::GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (block != nullptr) {
        void* target = ::GlobalLock(block);
        if (target != nullptr) {
            ::memcpy(target, text.c_str(), bytes);
            ::GlobalUnlock(block);
            // 所有权移交给剪贴板：SetClipboardData 成功后不能再 GlobalFree。
            if (::SetClipboardData(CF_UNICODETEXT, block) == nullptr) {
                ::GlobalFree(block);
            }
        } else {
            ::GlobalFree(block);
        }
    }

    ::CloseClipboard();
}

void Wizard::open_settings_apps() {
    // ms-settings 协议由 shell 处理：它是「设置 → 应用」的标准入口，
    // 比自己去枚举已装包更符合用户预期。
    const HINSTANCE result =
        ::ShellExecuteW(hwnd_, L"open", L"ms-settings:appsfeatures", nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(result) <= 32) {
        ::MessageBoxW(hwnd_, strings_.get(IDS_ERROR_NOT_FOUND).data(), strings_.get(IDS_MANAGE_OPEN_SETTINGS).data(),
                      MB_OK | MB_ICONWARNING);
    }
}

void Wizard::show_repair_plan() {
    // 修复只输出建议，绝不改动系统（ops.h 的硬规则）。计算放在后台线程上：
    // 它会去查包仓库与证书存储，可能慢到几百毫秒 —— 那在 UI 线程上就是肉眼可见的卡顿。
    set_text(IDC_MANAGE_DETAIL, strings_.get(IDS_MANAGE_REPAIR_INTRO));
    start_repair(worker_, hwnd_);
}

}  // namespace msixpkg::gui

// ============================================================================
// 入口
// ============================================================================

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, PWSTR command_line, int show) {
    (void)previous;
    (void)show;

    return msixpkg::gui::run(instance, command_line);
}
