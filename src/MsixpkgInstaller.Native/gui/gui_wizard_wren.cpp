#include "gui.h"

#include "controls.h"
#include "deployment.h"
#include "gui_strings.h"
#include "gui_ui.h"
#include "gui_worker.h"
#include "gui_widgets.h"
#include "host.h"
#include "internal.h"
#include "msixpkg/errors.h"
#include "theme.h"
#include "widget.h"

#include <Windows.h>
#include <commctrl.h>
#include <shellapi.h>

#include <algorithm>
#include <chrono>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Wren 版安装向导（feature/self-drawn-ui）。
//
// 和老 Win32 控件版（gui_wizard.cpp）的分工完全一致：本文件只跑在 UI 线程上，
// 耗时工作一律经 gui_worker.cpp 投到后台，结果用 PostMessage 回到 subclass_proc。
// 区别是「控件」换成了 Wren 的 retained Widget 树：单 HWND、没有子窗口、没有
// IsDialogMessage / WM_CTLCOLOR / NM_CUSTOMDRAW 补丁，键盘与 MSAA 由 Host 负责。

namespace msixpkg::gui {
namespace {

/// 新窗口类名。老版本是 `...v1`；单实例交接会同时找这两个类名。
constexpr wchar_t kWrenWindowClass[] = L"MsixpkgInstaller.Wizard.v2";
constexpr wchar_t kLegacyWindowClass[] = L"MsixpkgInstaller.Wizard.v1";

/// 客户区固定 760×480 DIP。宽一点是为了管理页四个动作按钮 + 关闭同排；
/// 高度保持 480，才能让 200% DPI 在 1080p 屏幕上仍然完整放得下。
constexpr int kWindowWidth = 760;
constexpr int kWindowHeight = 480;

constexpr float kMargin = 20.0f;
constexpr float kContentTop = 116.0f;
/// 116 → 416。底边再往下 12 DIP 就是按钮行，给同意页三个复选框的回流留足空间。
constexpr float kContentHeight = 300.0f;
constexpr float kNavY = 428.0f;
constexpr float kNavHeight = 32.0f;

/// SetWindowSubclass 的 id，只要求本窗口内唯一。
constexpr UINT_PTR kWizardSubclassId = 0x4D53;  // 'MS'

/// 把一行诊断写到**父进程的控制台**（只有在终端里运行时才看得见），然后立刻返回。
///
/// 不用 MessageBox：这条路最可能是「提权助手指到了 GUI」，而助手是用 SW_HIDE
/// 拉起的 —— 一个看不见的模态框会把它变成永不返回的子进程。
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

const wchar_t* page_name(WizardPage page) {
    switch (page) {
        case WizardPage::Summary: return L"summary";
        case WizardPage::Consent: return L"consent";
        case WizardPage::Progress: return L"progress";
        case WizardPage::Result: return L"result";
        case WizardPage::Manage: return L"manage";
    }

    return L"unknown";
}

bool page_from_name(std::wstring_view name, WizardPage& page) {
    if (name == L"summary") {
        page = WizardPage::Summary;
    } else if (name == L"consent") {
        page = WizardPage::Consent;
    } else if (name == L"progress") {
        page = WizardPage::Progress;
    } else if (name == L"result") {
        page = WizardPage::Result;
    } else if (name == L"manage") {
        page = WizardPage::Manage;
    } else {
        return false;
    }

    return true;
}

const wchar_t* appearance_name(wren::Appearance appearance) {
    switch (appearance) {
        case wren::Appearance::Auto: return L"auto";
        case wren::Appearance::Light: return L"light";
        case wren::Appearance::Dark: return L"dark";
        case wren::Appearance::HighContrast: return L"high-contrast";
    }

    return L"unknown";
}

const wchar_t* role_name(wren::Role role) {
    switch (role) {
        case wren::Role::Panel: return L"pane";
        case wren::Role::Label: return L"text";
        case wren::Role::Button: return L"pushbutton";
        case wren::Role::Checkbox: return L"checkbutton";
        case wren::Role::Progress: return L"progressbar";
        case wren::Role::Text: return L"document";
        case wren::Role::Table: return L"list";
        case wren::Role::Row: return L"listitem";
        case wren::Role::ScrollBar: return L"scrollbar";
    }

    return L"unknown";
}

/// 报告是逐行文本：名字/值里的换行会把记录拆散，这里压成空格。
std::wstring one_line(std::wstring_view value) {
    std::wstring result(value);
    for (wchar_t& character : result) {
        if (character == L'\r' || character == L'\n' || character == L'\t') {
            character = L' ';
        }
    }

    return result;
}

int phase_index(PhaseView phase) {
    switch (phase) {
        case PhaseView::Validate: return 0;
        case PhaseView::Stage: return 1;
        case PhaseView::Certificate: return 2;
        case PhaseView::Dependencies: return 3;
        case PhaseView::MainPackage: return 4;
        case PhaseView::Provision: return 5;
        case PhaseView::Reclaim: return 6;
        case PhaseView::Rollback: return 7;
        case PhaseView::Done: return 8;  // 越过最后一项 → 整条时间线都是「完成」
        default: return -1;
    }
}

// ============================================================================
// Wizard
// ============================================================================

class Wizard {
public:
    Wizard(HINSTANCE module, std::shared_ptr<SharedState> shared, Strings strings);
    ~Wizard();

    Wizard(const Wizard&) = delete;
    Wizard& operator=(const Wizard&) = delete;

    bool create();
    void show(const CommandLine& command);
    void show_uninstall(const std::wstring& family_name, const std::wstring& install_id);
    int run();
    void shutdown();

    [[nodiscard]] int exit_code() const noexcept { return exit_code_.load(); }

private:
    static LRESULT CALLBACK subclass_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR id,
                                          DWORD_PTR data);
    /// 返回 true 表示「已经处理，result 有效」；false 表示回落给 Host 的窗口过程。
    bool handle_message(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam, LRESULT& result);
    bool confirm_close();
    void on_drop_files(HDROP drop);
    LRESULT on_copy_data(const COPYDATASTRUCT* data);

    // ---- 构建 ----
    void build_tree(wren::Panel& root);
    void build_shell(wren::Panel& root);
    void build_summary(wren::Panel& page);
    void build_consent(wren::Panel& page);
    void build_progress(wren::Panel& page);
    void build_result(wren::Panel& page);
    void build_manage(wren::Panel& page);
    void build_manage_actions(wren::Panel& root);

    // ---- 页面 ----
    void show_page(WizardPage page);
    void render_page();
    void render_summary();
    void render_consent();
    void update_consent_gate();
    void render_progress();
    void render_result();
    void render_manage();
    void update_manage_detail();
    void layout_consent();
    void focus_page();
    void update_nav_chrome();

    // ---- 动作 ----
    void on_nav_next();
    void on_nav_back();
    void on_nav_cancel();
    void begin_preview(const std::wstring& container);
    void adopt_container(const std::wstring& container);
    void begin_install(const ConsentAnswers& answers);
    void submit_consent(bool approved);
    void begin_manage_load();
    void begin_uninstall();
    void begin_uninstall_for(const std::string& install_id, const std::wstring& family_name);
    void cancel_install();
    void open_log_file();
    void open_log_directory();
    void launch_application();
    void copy_to_clipboard(const std::wstring& text) const;
    void open_settings_apps();
    void show_repair_plan();

    // ---- 进度动画 ----
    void start_progress_animation();
    void tick_progress();
    void stop_progress_animation();

    // ---- 诊断 ----
    void write_ui_report() const;

    [[nodiscard]] std::wstring result_body(const InstallView& view) const;
    [[nodiscard]] HWND hwnd() const noexcept { return host_.hwnd(); }

    HINSTANCE module_ = nullptr;
    std::shared_ptr<SharedState> shared_;
    Strings strings_;
    wren::Host host_;
    wren::Theme styles_ = wren::make_theme(wren::Appearance::Auto, false);
    ThreadSlot worker_;

    bool shutdown_ = false;
    std::atomic<int> exit_code_{exit_code::kSuccess};

    // ---- 外壳 ----
    wren::Label* heading_ = nullptr;
    wren::Label* subheading_ = nullptr;
    StepRail* steps_ = nullptr;
    Divider* header_rule_ = nullptr;
    wren::Button* nav_back_ = nullptr;
    wren::Button* nav_next_ = nullptr;
    wren::Button* nav_cancel_ = nullptr;
    wren::Button* progress_cancel_ = nullptr;

    // ---- 五个页面（只显示当前页） ----
    wren::Panel* page_summary_ = nullptr;
    wren::Panel* page_consent_ = nullptr;
    wren::Panel* page_progress_ = nullptr;
    wren::Panel* page_result_ = nullptr;
    wren::Panel* page_manage_ = nullptr;

    // ---- 摘要 ----
    Card* summary_card_ = nullptr;
    PackageTile* summary_tile_ = nullptr;
    wren::Label* summary_name_ = nullptr;
    wren::Label* summary_publisher_ = nullptr;
    Divider* summary_rule_ = nullptr;
    wren::Label* summary_labels_[4]{};
    wren::Label* summary_values_[4]{};
    wren::Label* summary_description_label_ = nullptr;
    wren::Label* summary_description_ = nullptr;
    wren::Label* summary_note_ = nullptr;
    Banner* summary_banner_ = nullptr;

    // ---- 同意 ----
    Banner* consent_banner_ = nullptr;
    wren::TextView* consent_details_ = nullptr;
    wren::Checkbox* consent_accept_ = nullptr;
    wren::Checkbox* consent_trust_ = nullptr;
    wren::Label* consent_unpinned_ = nullptr;
    wren::Checkbox* consent_all_users_ = nullptr;

    // ---- 进度 ----
    PhaseTimeline* progress_timeline_ = nullptr;
    wren::ProgressBar* progress_bar_ = nullptr;
    wren::Label* progress_percent_ = nullptr;
    wren::Label* progress_status_ = nullptr;
    wren::TextView* progress_log_ = nullptr;

    // ---- 结果 ----
    StatusMark* result_mark_ = nullptr;
    wren::TextView* result_body_ = nullptr;
    wren::Label* result_log_ = nullptr;
    wren::Button* result_launch_ = nullptr;
    wren::Button* result_open_log_ = nullptr;
    wren::Button* result_copy_ = nullptr;

    // ---- 管理页 ----
    wren::Table* manage_table_ = nullptr;
    wren::TextView* manage_detail_ = nullptr;
    wren::Button* manage_uninstall_ = nullptr;
    wren::Button* manage_repair_ = nullptr;
    wren::Button* manage_logs_ = nullptr;
    wren::Button* manage_settings_ = nullptr;

    // ---- 状态 ----
    WizardPage page_ = WizardPage::Summary;
    bool installing_ = true;
    bool elevation_foregrounded_ = false;
    PhaseView last_phase_ = PhaseView::Idle;
    bool phase_failed_ = false;
    float shown_percent_ = 0.0f;
    int target_percent_ = 0;
    bool progress_animating_ = false;
    std::chrono::steady_clock::time_point last_tick_{};

    std::wstring report_path_;
    int exit_after_ms_ = 0;
};

// ============================================================================
// 构造 / 生命周期
// ============================================================================

Wizard::Wizard(HINSTANCE module, std::shared_ptr<SharedState> shared, Strings strings)
    : module_(module),
      shared_(std::move(shared)),
      strings_(std::move(strings)),
      styles_(wren::make_theme(wren::Appearance::Auto, false)),
      worker_(shared_) {}

Wizard::~Wizard() {
    shutdown();
}

void Wizard::shutdown() {
    if (shutdown_) {
        return;
    }

    shutdown_ = true;
    stop_progress_animation();

    // 先让编排线程退出（此时窗口还活着，PostMessage 不会打到野指针），再放手。
    stop_worker(worker_);
    worker_.wait_idle();
    shared_->target.store(nullptr);
}

int Wizard::run() {
    const int code = host_.run();
    shutdown();
    return code;
}

bool Wizard::create() {
    auto root = std::make_unique<wren::Panel>();
    root->bounds = {0.0f, 0.0f, static_cast<float>(kWindowWidth), static_cast<float>(kWindowHeight)};

    build_tree(*root);

    wren::HostOptions options;
    options.title = strings_.text(IDS_WINDOW_TITLE);
    options.window_class = kWrenWindowClass;
    options.width_dip = kWindowWidth;
    options.height_dip = kWindowHeight;
    options.appearance = wren::Appearance::Auto;
    options.material = wren::MaterialPreference::Auto;
    // Esc 必须走「安装中先确认」这条老语义，不能直接关窗。
    options.on_escape = [this] {
        if (hwnd() != nullptr) {
            ::PostMessageW(hwnd(), WM_CLOSE, 0, 0);
        }
    };

    if (!host_.create(module_, std::move(root), options)) {
        return false;
    }

    // build_tree() 里存的是树的裸指针；root 已经交给 Host，指针仍然有效。
    shared_->target.store(hwnd());

    // Host 的窗口类用的是默认图标；这里换成安装器自己的。
    const HICON icon = static_cast<HICON>(::LoadIconW(module_, MAKEINTRESOURCEW(IDI_MSIXPKG)));
    if (icon != nullptr) {
        ::SendMessageW(hwnd(), WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(icon));
        ::SendMessageW(hwnd(), WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(icon));
    }

    ::DragAcceptFiles(hwnd(), TRUE);
    ::SetWindowSubclass(hwnd(), &Wizard::subclass_proc, kWizardSubclassId, reinterpret_cast<DWORD_PTR>(this));

    // 窗口在 Host::create 里已经上屏；先按空态画一遍摘要，避免第一帧是空白标题。
    render_page();
    return true;
}

void Wizard::show(const CommandLine& command) {
    report_path_ = command.report_path;
    exit_after_ms_ = command.exit_after_ms;

    if (!command.ui_theme.empty()) {
        if (command.ui_theme == L"light") {
            host_.set_appearance(wren::Appearance::Light);
        } else if (command.ui_theme == L"dark") {
            host_.set_appearance(wren::Appearance::Dark);
        } else if (command.ui_theme == L"hc") {
            host_.set_appearance(wren::Appearance::HighContrast);
        } else if (command.ui_theme == L"auto") {
            host_.set_appearance(wren::Appearance::Auto);
        }
    }

    if (!command.ui_material.empty()) {
        if (command.ui_material == L"solid") {
            host_.set_material(wren::MaterialPreference::Solid);
        } else if (command.ui_material == L"mica") {
            host_.set_material(wren::MaterialPreference::Mica);
        } else if (command.ui_material == L"acrylic") {
            host_.set_material(wren::MaterialPreference::Acrylic);
        } else if (command.ui_material == L"auto") {
            host_.set_material(wren::MaterialPreference::Auto);
        }
    }

    if (command.manage) {
        show_page(WizardPage::Manage);
        begin_manage_load();
        render_page();
    } else if (!command.container.empty()) {
        begin_preview(command.container.wstring());
    } else {
        {
            const std::lock_guard<std::mutex> guard(shared_->mutex);
            shared_->preview = PackageView{};
        }

        show_page(WizardPage::Summary);
        render_page();
    }

    // 诊断参数：强制打开某一页（截图 / 无障碍回归用）。
    if (!command.ui_page.empty()) {
        WizardPage forced = WizardPage::Summary;
        if (page_from_name(command.ui_page, forced)) {
            show_page(forced);
            if (forced == WizardPage::Manage) {
                begin_manage_load();
            }

            render_page();
        }
    }

    // 诊断参数：合成一份进度（阶段 + 日志 + 百分比），用于进度页动画/时间线回归。
    if (command.ui_percent >= 0) {
        PhaseView phase = PhaseView::Idle;
        if (command.ui_percent >= 100) {
            phase = PhaseView::Done;
        } else if (command.ui_percent >= 90) {
            phase = PhaseView::Provision;
        } else if (command.ui_percent >= 75) {
            phase = PhaseView::MainPackage;
        } else if (command.ui_percent >= 60) {
            phase = PhaseView::Dependencies;
        } else if (command.ui_percent >= 40) {
            phase = PhaseView::Certificate;
        } else if (command.ui_percent >= 15) {
            phase = PhaseView::Stage;
        } else if (command.ui_percent > 0) {
            phase = PhaseView::Validate;
        }

        {
            const std::lock_guard<std::mutex> guard(shared_->mutex);
            shared_->install = InstallView{};
            shared_->percent = static_cast<uint32_t>(command.ui_percent);
            shared_->phase = phase;
            shared_->waiting_for_elevation = false;
            shared_->phase_log = {L"Validating the container", L"Extracting the payload",
                                  L"Importing the publisher certificate"};
        }

        installing_ = true;
        elevation_foregrounded_ = false;
        last_phase_ = PhaseView::Idle;
        phase_failed_ = false;
        shown_percent_ = 0.0f;
        target_percent_ = command.ui_percent;
        progress_cancel_->enabled = true;
        show_page(WizardPage::Progress);
        render_page();
    }

    // 诊断参数：往结果页塞一份合成结果（截图 / 无障碍回归用，不启动编排线程）。
    if (!command.ui_result.empty()) {
        InstallView view;
        view.present = true;
        const std::wstring& kind = command.ui_result;

        if (kind == L"success") {
            view.status = InstallStatus::Success;
            view.exit_code = exit_code::kSuccess;
            view.installed = true;
            view.certificate_added = true;
            view.family_name = L"Msixpkg.Sample_1.0.0.0_x64__msixpkgtest";
            view.application_id = "App";
        } else if (kind == L"cancelled" || kind == L"declined") {
            view.status = InstallStatus::Cancelled;
            view.exit_code = exit_code::kCancelled;
            view.elevation_declined = kind == L"declined";
        } else {
            view.status = InstallStatus::Failure;
            view.exit_code = exit_code::kDeploymentFailed;
            view.error_code = L"E_DEPLOYMENT_FAILED";
            view.error_message = L"The package could not be deployed.";
            view.error_detail = L"HRESULT 0x80073CF9 (ERROR_INSTALL_FAILED)";
            view.hint_id = IDS_RESULT_HINT_GENERIC;
        }

        view.log_path = L"artifacts\\ui-report\\demo-install.log";
        {
            const std::lock_guard<std::mutex> guard(shared_->mutex);
            shared_->install = view;
        }

        exit_code_.store(view.exit_code);
        show_page(WizardPage::Result);
        render_page();
    }

    if (exit_after_ms_ > 0) {
        host_.schedule_once(exit_after_ms_, [this] {
            write_ui_report();
            host_.quit(exit_code_.load());
        });
    }
}

void Wizard::show_uninstall(const std::wstring& family_name, const std::wstring& install_id) {
    if (family_name.empty() && install_id.empty()) {
        // 没有目标 = 用法错误（ARP 一定会带 --family-name）。用退出码 3 说清楚，
        // 而不是打开一个什么都干不了的窗口。
        exit_code_.store(exit_code::kUsage);
        ::MessageBoxW(hwnd(), strings_.get(IDS_MANAGE_NO_SELECTION).data(), strings_.get(IDS_MANAGE_TITLE).data(),
                      MB_OK | MB_ICONWARNING);
        host_.quit(exit_code::kUsage);
        return;
    }

    begin_uninstall_for(narrow_utf8(install_id), family_name);
}

// ============================================================================
// 窗口消息
// ============================================================================

LRESULT CALLBACK Wizard::subclass_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR id,
                                       DWORD_PTR data) {
    (void)id;
    auto* self = reinterpret_cast<Wizard*>(data);
    if (self != nullptr) {
        LRESULT result = 0;
        if (self->handle_message(hwnd, message, wparam, lparam, result)) {
            return result;
        }
    }

    return ::DefSubclassProc(hwnd, message, wparam, lparam);
}

bool Wizard::handle_message(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam, LRESULT& result) {
    switch (message) {
        case WM_CLOSE:
            if (!confirm_close()) {
                result = 0;  // 用户选择继续安装：绝不能 DestroyWindow
                return true;
            }

            host_.quit(exit_code_.load());
            return false;  // 交给 Host 的 WM_CLOSE（DestroyWindow → WM_DESTROY → PostQuitMessage）

        case WM_DESTROY:
            // 窗口已经没了：让编排线程的 PostMessage 变成空操作。
            shared_->target.store(nullptr);
            return false;

        case WM_DROPFILES:
            on_drop_files(reinterpret_cast<HDROP>(wparam));
            result = 0;
            return true;

        case WM_COPYDATA:
            result = on_copy_data(reinterpret_cast<const COPYDATASTRUCT*>(lparam));
            return true;

        // ---- 编排线程发来的消息（全部只读 SharedState + 刷新控件） ----
        case kMsgPreviewReady:
            render_page();
            result = 0;
            return true;

        case kMsgConsentAsk:
            show_page(WizardPage::Consent);
            render_page();
            result = 0;
            return true;

        case kMsgInstallDone:
            if (page_ == WizardPage::Progress) {
                render_page();
            }

            result = 0;
            return true;

        case kMsgManageLoaded:
            if (page_ == WizardPage::Manage) {
                render_page();
            }

            result = 0;
            return true;

        case kMsgRepairReady: {
            if (page_ != WizardPage::Manage) {
                result = 0;
                return true;
            }

            ManageView view;
            {
                const std::lock_guard<std::mutex> guard(shared_->mutex);
                view = shared_->manage;
            }

            std::wstring text = strings_.text(IDS_MANAGE_REPAIR_INTRO);
            text += L"\r\n\r\n";
            text += view.repair_text == L"\u0001" ? strings_.text(IDS_MANAGE_NO_SUGGESTIONS) : view.repair_text;
            manage_detail_->set_text(text);
            ::MessageBoxW(hwnd, text.c_str(), strings_.get(IDS_MANAGE_REPAIR).data(), MB_OK | MB_ICONINFORMATION);
            result = 0;
            return true;
        }

        case kMsgProgress: {
            if (page_ != WizardPage::Progress) {
                result = 0;
                return true;
            }

            // 提权马上发生：把自己摆到前台并闪任务栏。策略把 UAC 确认框放在普通桌面时，
            // 它可能开在向导后面 —— 用户会以为「什么都没有发生，卡住了」。
            bool waiting = false;
            {
                const std::lock_guard<std::mutex> guard(shared_->mutex);
                waiting = shared_->waiting_for_elevation;
            }

            if (waiting && !elevation_foregrounded_) {
                elevation_foregrounded_ = true;
                ::SetForegroundWindow(hwnd);
                FLASHWINFO flash{};
                flash.cbSize = sizeof(flash);
                flash.hwnd = hwnd;
                flash.dwFlags = FLASHW_TRAY | FLASHW_TIMERNOFG;
                ::FlashWindowEx(&flash);
            }

            render_progress();
            result = 0;
            return true;
        }

        default:
            return false;
    }
}

bool Wizard::confirm_close() {
    // 装到一半不许直接关：那会让用户以为「已经取消了」，而编排线程还在改系统。
    if (page_ == WizardPage::Progress && worker_.busy()) {
        const int answer = ::MessageBoxW(hwnd(), strings_.get(IDS_PROGRESS_SUBHEADING).data(),
                                         strings_.get(IDS_PROGRESS_TITLE).data(), MB_OKCANCEL | MB_ICONWARNING);
        if (answer != IDOK) {
            return false;
        }

        cancel_install();
    }

    return true;
}

void Wizard::on_drop_files(HDROP drop) {
    if (drop == nullptr) {
        return;
    }

    // 装东西的时候不接手新容器：adopt_container 会先把正在跑的任务停干净，
    // 而那是**在 UI 线程上等**编排线程 —— 安装中就是几十秒到几分钟的假死。
    if (worker_.busy()) {
        ::DragFinish(drop);
        ::MessageBeep(MB_ICONWARNING);
        return;
    }

    const UINT count = ::DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
    if (count == 0) {
        ::DragFinish(drop);
        return;
    }

    const UINT length = ::DragQueryFileW(drop, 0, nullptr, 0);
    std::wstring path(static_cast<size_t>(length) + 1, L'\0');
    ::DragQueryFileW(drop, 0, path.data(), length + 1);
    path.resize(length);
    ::DragFinish(drop);
    adopt_container(path);
}

LRESULT Wizard::on_copy_data(const COPYDATASTRUCT* data) {
    if (data == nullptr || data->dwData != kCopyDataMagic || data->lpData == nullptr) {
        return FALSE;
    }

    // WM_COPYDATA 的缓冲只在处理这条消息期间有效：这里立刻拷成 std::wstring。
    const auto* text = static_cast<const wchar_t*>(data->lpData);
    std::wstring container(text, data->cbData / sizeof(wchar_t));
    while (!container.empty() && container.back() == L'\0') {
        container.pop_back();
    }

    if (container.empty()) {
        return TRUE;
    }

    // 装东西的时候绝不能在这里等编排线程：发送方是同步 SendMessage，它要等我们
    // 返回；而我们要等编排线程；编排线程可能正等着那个发送方退出 —— 三方互等。
    if (worker_.busy()) {
        return static_cast<LRESULT>(kCopyDataBusy);
    }

    adopt_container(container);
    return TRUE;
}

// ============================================================================
// 构建控件树
// ============================================================================

void Wizard::build_tree(wren::Panel& root) {
    build_shell(root);

    page_summary_ = root.emplace<wren::Panel>();
    page_consent_ = root.emplace<wren::Panel>();
    page_progress_ = root.emplace<wren::Panel>();
    page_result_ = root.emplace<wren::Panel>();
    page_manage_ = root.emplace<wren::Panel>();

    for (wren::Panel* page : {page_summary_, page_consent_, page_progress_, page_result_, page_manage_}) {
        page->bounds = {0.0f, kContentTop, static_cast<float>(kWindowWidth), kContentHeight};
        page->visible = false;
    }

    build_summary(*page_summary_);
    build_consent(*page_consent_);
    build_progress(*page_progress_);
    build_result(*page_result_);
    build_manage(*page_manage_);
    build_manage_actions(root);

    show_page(WizardPage::Summary);
}

void Wizard::build_shell(wren::Panel& root) {
    wren::TextStyle title = styles_.title;
    title.size = 20.0f;
    title.ellipsis = true;

    wren::TextStyle subtitle = styles_.body;
    subtitle.size = 13.0f;
    subtitle.ellipsis = true;

    heading_ = root.emplace<wren::Label>(L"", title);
    heading_->bounds = {kMargin, 10.0f, 520.0f, 30.0f};
    heading_->control_id = IDC_HEADING;

    subheading_ = root.emplace<wren::Label>(L"", subtitle);
    subheading_->bounds = {kMargin, 42.0f, 640.0f, 20.0f};
    subheading_->control_id = IDC_SUBHEADING;
    subheading_->set_tone(wren::LabelTone::Secondary);

    steps_ = root.emplace<StepRail>();
    steps_->bounds = {kMargin, 68.0f, static_cast<float>(kWindowWidth) - kMargin * 2.0f, 34.0f};
    steps_->set_labels({strings_.text(IDS_SUMMARY_TITLE), strings_.text(IDS_CONSENT_TITLE),
                        strings_.text(IDS_PROGRESS_TITLE), strings_.text(IDS_RESULT_TITLE)});

    header_rule_ = root.emplace<Divider>();
    header_rule_->bounds = {kMargin, 108.0f, static_cast<float>(kWindowWidth) - kMargin * 2.0f, 1.0f};

    nav_back_ = root.emplace<wren::Button>(strings_.text(IDS_COMMON_BACK), [this] { on_nav_back(); });
    nav_back_->bounds = {464.0f, kNavY, 84.0f, kNavHeight};
    nav_back_->control_id = IDC_NAV_BACK;

    nav_next_ = root.emplace<wren::Button>(strings_.text(IDS_COMMON_NEXT), [this] { on_nav_next(); });
    nav_next_->bounds = {556.0f, kNavY, 84.0f, kNavHeight};
    nav_next_->control_id = IDC_NAV_NEXT;
    nav_next_->set_primary(true);
    nav_next_->set_default(true);

    nav_cancel_ = root.emplace<wren::Button>(strings_.text(IDS_COMMON_CANCEL), [this] { on_nav_cancel(); });
    nav_cancel_->bounds = {648.0f, kNavY, 92.0f, kNavHeight};
    nav_cancel_->control_id = IDC_NAV_CANCEL;

    progress_cancel_ = root.emplace<wren::Button>(strings_.text(IDS_COMMON_CANCEL), [this] { cancel_install(); });
    progress_cancel_->bounds = {kMargin, kNavY, 140.0f, kNavHeight};
    progress_cancel_->control_id = IDC_PROGRESS_CANCEL;
    progress_cancel_->visible = false;
}

void Wizard::build_summary(wren::Panel& page) {
    wren::TextStyle title = styles_.title;
    title.size = 18.0f;
    title.ellipsis = true;

    wren::TextStyle secondary = styles_.body;
    secondary.size = 13.0f;
    secondary.ellipsis = true;

    wren::TextStyle row_label = styles_.body;
    row_label.size = 13.0f;
    row_label.align = wren::TextAlign::Right;
    row_label.valign = wren::TextVAlign::Center;
    row_label.ellipsis = true;

    wren::TextStyle row_value = styles_.body;
    row_value.size = 13.0f;
    row_value.valign = wren::TextVAlign::Center;
    row_value.ellipsis = true;

    wren::TextStyle wrapped = styles_.body;
    wrapped.size = 13.0f;
    wrapped.wrap = true;
    wrapped.valign = wren::TextVAlign::Top;

    summary_card_ = page.emplace<Card>();
    summary_card_->bounds = {kMargin, kContentTop, static_cast<float>(kWindowWidth) - kMargin * 2.0f, kContentHeight};

    summary_tile_ = summary_card_->emplace<PackageTile>();
    summary_tile_->bounds = {44.0f, 138.0f, 56.0f, 56.0f};

    summary_name_ = summary_card_->emplace<wren::Label>(L"", title);
    summary_name_->bounds = {116.0f, 138.0f, 570.0f, 28.0f};
    summary_name_->control_id = IDC_SUMMARY_NAME;

    summary_publisher_ = summary_card_->emplace<wren::Label>(L"", secondary);
    summary_publisher_->bounds = {116.0f, 168.0f, 570.0f, 18.0f};
    summary_publisher_->control_id = IDC_SUMMARY_PUBLISHER;
    summary_publisher_->set_tone(wren::LabelTone::Secondary);

    summary_rule_ = summary_card_->emplace<Divider>();
    summary_rule_->bounds = {44.0f, 196.0f, 672.0f, 1.0f};

    const int label_ids[] = {IDS_SUMMARY_LABEL_VERSION, IDS_SUMMARY_LABEL_ARCH, IDS_SUMMARY_LABEL_SIZE,
                             IDS_SUMMARY_LABEL_DEPS};
    const int label_control_ids[] = {IDC_SUMMARY_LABEL_VERSION, IDC_SUMMARY_LABEL_ARCH, IDC_SUMMARY_LABEL_SIZE,
                                     IDC_SUMMARY_LABEL_DEPS};
    const int value_control_ids[] = {IDC_SUMMARY_VERSION, IDC_SUMMARY_ARCH, IDC_SUMMARY_SIZE, IDC_SUMMARY_DEPS};
    for (int index = 0; index < 4; ++index) {
        const float y = 206.0f + static_cast<float>(index) * 24.0f;

        summary_labels_[index] = summary_card_->emplace<wren::Label>(strings_.text(label_ids[index]), row_label);
        summary_labels_[index]->bounds = {44.0f, y, 96.0f, 20.0f};
        summary_labels_[index]->control_id = label_control_ids[index];
        summary_labels_[index]->set_tone(wren::LabelTone::Secondary);

        summary_values_[index] = summary_card_->emplace<wren::Label>(L"", row_value);
        summary_values_[index]->bounds = {152.0f, y, 564.0f, 20.0f};
        summary_values_[index]->control_id = value_control_ids[index];
    }

    summary_description_label_ =
        summary_card_->emplace<wren::Label>(strings_.text(IDS_SUMMARY_LABEL_DESCRIPTION), row_label);
    summary_description_label_->bounds = {44.0f, 302.0f, 96.0f, 20.0f};
    summary_description_label_->control_id = IDC_SUMMARY_LABEL_DESCRIPTION;
    summary_description_label_->set_tone(wren::LabelTone::Secondary);

    summary_description_ = summary_card_->emplace<wren::Label>(L"", wrapped);
    summary_description_->bounds = {152.0f, 302.0f, 564.0f, 36.0f};
    summary_description_->control_id = IDC_SUMMARY_DESCRIPTION;

    wren::TextStyle note = styles_.body;
    note.size = 13.0f;
    note.ellipsis = true;
    summary_note_ = summary_card_->emplace<wren::Label>(L"", note);
    summary_note_->bounds = {44.0f, 346.0f, 672.0f, 22.0f};
    summary_note_->control_id = IDC_SUMMARY_NOTE;
    summary_note_->set_tone(wren::LabelTone::Accent);
    summary_note_->visible = false;

    summary_banner_ = summary_card_->emplace<Banner>();
    summary_banner_->bounds = {44.0f, 204.0f, 672.0f, 150.0f};
    summary_banner_->set_kind(Banner::Kind::Warning);
    summary_banner_->visible = false;
}

void Wizard::build_consent(wren::Panel& page) {
    wren::TextStyle body = styles_.body;
    body.size = 13.0f;

    wren::TextStyle warning = styles_.body;
    warning.size = 12.0f;
    warning.wrap = true;
    warning.valign = wren::TextVAlign::Top;

    consent_banner_ = page.emplace<Banner>();
    consent_banner_->bounds = {kMargin, kContentTop, static_cast<float>(kWindowWidth) - kMargin * 2.0f, 52.0f};
    consent_banner_->control_id = IDC_CONSENT_WARNING;
    consent_banner_->set_kind(Banner::Kind::Warning);
    consent_banner_->set_message(strings_.text(IDS_CONSENT_WARNING));

    consent_details_ = page.emplace<wren::TextView>(L"", body);
    consent_details_->bounds = {kMargin, 176.0f, static_cast<float>(kWindowWidth) - kMargin * 2.0f, 108.0f};
    consent_details_->control_id = IDC_CONSENT_DETAILS;
    consent_details_->set_show_scrollbar(true);

    consent_accept_ = page.emplace<wren::Checkbox>(strings_.text(IDS_CONSENT_ACCEPT));
    consent_accept_->bounds = {kMargin, 288.0f, static_cast<float>(kWindowWidth) - kMargin * 2.0f, 28.0f};
    consent_accept_->control_id = IDC_CONSENT_ACCEPT;
    consent_accept_->set_on_changed([this](bool) { update_consent_gate(); });

    consent_trust_ = page.emplace<wren::Checkbox>(strings_.text(IDS_CONSENT_TRUST_PUBLISHER));
    consent_trust_->bounds = {kMargin, 322.0f, static_cast<float>(kWindowWidth) - kMargin * 2.0f, 28.0f};
    consent_trust_->control_id = IDC_CONSENT_TRUST_PUBLISHER;
    consent_trust_->set_on_changed([this](bool) { update_consent_gate(); });
    consent_trust_->visible = false;

    consent_unpinned_ = page.emplace<wren::Label>(strings_.text(IDS_CONSENT_UNPINNED_WARNING), warning);
    consent_unpinned_->bounds = {kMargin, 354.0f, static_cast<float>(kWindowWidth) - kMargin * 2.0f, 34.0f};
    consent_unpinned_->control_id = IDC_CONSENT_UNPINNED_WARNING;
    consent_unpinned_->set_tone(wren::LabelTone::Warning);
    consent_unpinned_->visible = false;

    consent_all_users_ = page.emplace<wren::Checkbox>(strings_.text(IDS_CONSENT_ALL_USERS));
    consent_all_users_->bounds = {kMargin, 390.0f, static_cast<float>(kWindowWidth) - kMargin * 2.0f, 24.0f};
    consent_all_users_->control_id = IDC_CONSENT_ALL_USERS;
    consent_all_users_->visible = false;
}

void Wizard::build_progress(wren::Panel& page) {
    wren::TextStyle caption = styles_.caption;
    caption.size = 13.0f;

    wren::TextStyle secondary = styles_.body;
    secondary.size = 13.0f;
    secondary.ellipsis = true;

    wren::TextStyle log = styles_.mono;
    log.size = 12.0f;

    progress_timeline_ = page.emplace<PhaseTimeline>();
    progress_timeline_->bounds = {kMargin, kContentTop, static_cast<float>(kWindowWidth) - kMargin * 2.0f, 132.0f};
    progress_timeline_->control_id = IDC_PROGRESS_PHASES;
    progress_timeline_->set_items({strings_.text(IDS_PROGRESS_PHASE_VALIDATE),
                                   strings_.text(IDS_PROGRESS_PHASE_STAGE),
                                   strings_.text(IDS_PROGRESS_PHASE_CERTIFICATE),
                                   strings_.text(IDS_PROGRESS_PHASE_DEPENDENCIES),
                                   strings_.text(IDS_PROGRESS_PHASE_MAIN),
                                   strings_.text(IDS_PROGRESS_PHASE_PROVISION),
                                   strings_.text(IDS_PROGRESS_PHASE_RECLAIM),
                                   strings_.text(IDS_PROGRESS_PHASE_DONE)});
    progress_timeline_->set_state_labels(strings_.text(IDS_PROGRESS_COMPLETE), strings_.text(IDS_PROGRESS_ACTIVE),
                                         strings_.text(IDS_PROGRESS_PENDING), strings_.text(IDS_PROGRESS_FAILED));

    progress_bar_ = page.emplace<wren::ProgressBar>();
    progress_bar_->bounds = {kMargin, 256.0f, 600.0f, 10.0f};
    progress_bar_->control_id = IDC_PROGRESS_BAR;

    progress_percent_ = page.emplace<wren::Label>(L"", caption);
    progress_percent_->bounds = {628.0f, 250.0f, 112.0f, 22.0f};
    progress_percent_->control_id = IDC_PROGRESS_PERCENT;
    progress_percent_->set_align(wren::TextAlign::Right);
    progress_percent_->set_tone(wren::LabelTone::Secondary);

    progress_status_ = page.emplace<wren::Label>(L"", secondary);
    progress_status_->bounds = {kMargin, 274.0f, static_cast<float>(kWindowWidth) - kMargin * 2.0f, 22.0f};
    progress_status_->control_id = IDC_PROGRESS_STATUS;
    progress_status_->set_tone(wren::LabelTone::Secondary);

    progress_log_ = page.emplace<wren::TextView>(L"", log);
    progress_log_->bounds = {kMargin, 300.0f, static_cast<float>(kWindowWidth) - kMargin * 2.0f, 108.0f};
    progress_log_->control_id = IDC_PROGRESS_LOG;
    progress_log_->set_show_scrollbar(true);
}

void Wizard::build_result(wren::Panel& page) {
    wren::TextStyle body = styles_.body;
    body.size = 13.0f;

    wren::TextStyle caption = styles_.caption;
    caption.size = 12.0f;
    caption.ellipsis = true;

    result_mark_ = page.emplace<StatusMark>();
    result_mark_->bounds = {kMargin, 122.0f, 56.0f, 56.0f};
    result_mark_->set_kind(StatusMark::Kind::Info);

    result_body_ = page.emplace<wren::TextView>(L"", body);
    result_body_->bounds = {kMargin, 190.0f, static_cast<float>(kWindowWidth) - kMargin * 2.0f, 140.0f};
    result_body_->control_id = IDC_RESULT_BODY;
    result_body_->set_show_scrollbar(true);

    result_log_ = page.emplace<wren::Label>(L"", caption);
    result_log_->bounds = {kMargin, 338.0f, static_cast<float>(kWindowWidth) - kMargin * 2.0f, 20.0f};
    result_log_->control_id = IDC_RESULT_LOG;
    result_log_->set_tone(wren::LabelTone::Secondary);

    result_launch_ = page.emplace<wren::Button>(strings_.text(IDS_RESULT_LAUNCH), [this] { launch_application(); });
    result_launch_->bounds = {kMargin, 372.0f, 170.0f, 32.0f};
    result_launch_->control_id = IDC_RESULT_LAUNCH;
    result_launch_->set_primary(true);

    result_open_log_ = page.emplace<wren::Button>(strings_.text(IDS_RESULT_OPEN_LOG), [this] { open_log_file(); });
    result_open_log_->bounds = {200.0f, 372.0f, 150.0f, 32.0f};
    result_open_log_->control_id = IDC_RESULT_OPEN_LOG;

    result_copy_ = page.emplace<wren::Button>(strings_.text(IDS_RESULT_COPY_DETAILS), [this] {
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
    });
    result_copy_->bounds = {360.0f, 372.0f, 150.0f, 32.0f};
    result_copy_->control_id = IDC_RESULT_COPY_DETAILS;
}

void Wizard::build_manage(wren::Panel& page) {
    wren::TextStyle body = styles_.body;
    body.size = 13.0f;

    manage_table_ = page.emplace<wren::Table>();
    manage_table_->bounds = {kMargin, kContentTop, static_cast<float>(kWindowWidth) - kMargin * 2.0f, 216.0f};
    manage_table_->control_id = IDC_MANAGE_LIST;
    manage_table_->set_columns({{strings_.text(IDS_MANAGE_COL_NAME), 260.0f},
                                {strings_.text(IDS_MANAGE_COL_VERSION), 96.0f},
                                {strings_.text(IDS_MANAGE_COL_CERT), 224.0f},
                                {strings_.text(IDS_MANAGE_COL_STATUS), 140.0f}});
    manage_table_->set_on_selection_changed([this](int row) {
        if (manage_uninstall_ != nullptr) {
            manage_uninstall_->enabled = row >= 0;
            manage_uninstall_->invalidate();
        }

        update_manage_detail();
    });

    manage_detail_ = page.emplace<wren::TextView>(L"", body);
    manage_detail_->bounds = {kMargin, 340.0f, static_cast<float>(kWindowWidth) - kMargin * 2.0f, 64.0f};
    manage_detail_->control_id = IDC_MANAGE_DETAIL;
    manage_detail_->set_show_scrollbar(true);
}

void Wizard::build_manage_actions(wren::Panel& root) {
    manage_uninstall_ =
        root.emplace<wren::Button>(strings_.text(IDS_MANAGE_UNINSTALL), [this] { begin_uninstall(); });
    manage_uninstall_->bounds = {kMargin, kNavY, 110.0f, kNavHeight};
    manage_uninstall_->control_id = IDC_MANAGE_UNINSTALL;
    manage_uninstall_->visible = false;

    manage_repair_ = root.emplace<wren::Button>(strings_.text(IDS_MANAGE_REPAIR), [this] { show_repair_plan(); });
    manage_repair_->bounds = {136.0f, kNavY, 120.0f, kNavHeight};
    manage_repair_->control_id = IDC_MANAGE_REPAIR;
    manage_repair_->visible = false;

    manage_logs_ = root.emplace<wren::Button>(strings_.text(IDS_MANAGE_OPEN_LOGS), [this] { open_log_directory(); });
    manage_logs_->bounds = {262.0f, kNavY, 130.0f, kNavHeight};
    manage_logs_->control_id = IDC_MANAGE_OPEN_LOGS;
    manage_logs_->visible = false;

    manage_settings_ =
        root.emplace<wren::Button>(strings_.text(IDS_MANAGE_OPEN_SETTINGS), [this] { open_settings_apps(); });
    manage_settings_->bounds = {398.0f, kNavY, 180.0f, kNavHeight};
    manage_settings_->control_id = IDC_MANAGE_OPEN_SETTINGS;
    manage_settings_->visible = false;
}

// ============================================================================
// 页面切换与渲染
// ============================================================================

void Wizard::show_page(WizardPage page) {
    page_ = page;

    page_summary_->visible = page == WizardPage::Summary;
    page_consent_->visible = page == WizardPage::Consent;
    page_progress_->visible = page == WizardPage::Progress;
    page_result_->visible = page == WizardPage::Result;
    page_manage_->visible = page == WizardPage::Manage;

    steps_->visible = page != WizardPage::Manage;
    steps_->set_active(static_cast<int>(page));

    nav_back_->visible = page == WizardPage::Consent;
    nav_next_->visible = page == WizardPage::Summary || page == WizardPage::Consent;
    nav_cancel_->visible = page != WizardPage::Progress;
    progress_cancel_->visible = page == WizardPage::Progress;
    if (page == WizardPage::Progress) {
        // 上一次取消会把它禁用；重新进入进度页（新的安装/卸载）必须恢复。
        progress_cancel_->enabled = true;
        progress_cancel_->invalidate();
    }

    const bool manage = page == WizardPage::Manage;
    manage_uninstall_->visible = manage;
    manage_repair_->visible = manage;
    manage_logs_->visible = manage;
    manage_settings_->visible = manage;

    if (page == WizardPage::Consent) {
        layout_consent();
    }

    // 离开进度页就停帧：结果页不需要再为进度条跑合成器节拍。
    if (page != WizardPage::Progress) {
        stop_progress_animation();
    }

    update_nav_chrome();

    if (hwnd() != nullptr) {
        focus_page();
        host_.layout_changed();
    }
}

void Wizard::update_nav_chrome() {
    nav_cancel_->text = strings_.text(page_ == WizardPage::Result || page_ == WizardPage::Manage ? IDS_COMMON_CLOSE
                                                                                                  : IDS_COMMON_CANCEL);
    nav_cancel_->invalidate();

    // 一页只能有一个默认按钮：结果页由 render_result 在「启动应用 / 关闭」之间二选一。
    if (page_ == WizardPage::Manage) {
        nav_cancel_->set_default(true);
    } else if (page_ != WizardPage::Result) {
        nav_cancel_->set_default(false);
    }

    nav_next_->set_default(page_ == WizardPage::Summary || page_ == WizardPage::Consent);
}

void Wizard::focus_page() {
    wren::Widget* target = nullptr;
    switch (page_) {
        case WizardPage::Summary:
            target = nav_next_->enabled ? static_cast<wren::Widget*>(nav_next_) : static_cast<wren::Widget*>(nav_cancel_);
            break;
        case WizardPage::Consent:
            target = consent_accept_;
            break;
        case WizardPage::Progress:
            target = progress_cancel_;
            break;
        case WizardPage::Result:
            target = result_launch_->enabled ? static_cast<wren::Widget*>(result_launch_)
                                             : static_cast<wren::Widget*>(nav_cancel_);
            break;
        case WizardPage::Manage:
            target = manage_table_;
            break;
    }

    if (target != nullptr) {
        host_.focus_widget(target);
    }
}

void Wizard::render_page() {
    PackageView preview;
    InstallView install;
    {
        const std::lock_guard<std::mutex> guard(shared_->mutex);
        preview = shared_->preview;
        install = shared_->install;
    }

    // 安装已经出结果 → 一律进结果页（通知消息与页面切换交叉时也要收敛）。
    if (page_ == WizardPage::Progress && install.present) {
        exit_code_.store(install.exit_code);
        show_page(WizardPage::Result);
    }

    // 容器校验失败时把退出码也定下来：与 CLI 的「2 校验失败 / 4 I/O」一致。
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

    if (hwnd() != nullptr) {
        if (host_.focused_widget() == nullptr) {
            focus_page();
        }

        host_.layout_changed();
    }
}

void Wizard::render_summary() {
    PackageView view;
    {
        const std::lock_guard<std::mutex> guard(shared_->mutex);
        view = shared_->preview;
    }

    heading_->set_text(strings_.text(IDS_SUMMARY_HEADING));
    subheading_->visible = true;
    subheading_->set_text(strings_.text(IDS_SUMMARY_SUBHEADING));

    const bool load_failed = !view.ok && !view.error_code.empty();
    const auto set_rows_visible = [this](bool visible) {
        for (int index = 0; index < 4; ++index) {
            summary_labels_[index]->visible = visible;
            summary_values_[index]->visible = visible;
        }

        summary_description_label_->visible = visible;
        summary_description_->visible = visible;
    };

    if (!view.ok) {
        summary_name_->set_text(strings_.text(load_failed ? IDS_ERROR_LOAD_FAILED : IDS_ERROR_NO_FILE));
        summary_publisher_->set_text(L"");
        set_rows_visible(false);
        summary_note_->visible = false;

        // 「没给容器」标题已经说清楚了：藏掉包名/图标，让整段提示直接占住卡片顶部；
        // 真的校验失败时保留包槽位，再把错误码与原因放在下面。
        const bool no_file = !load_failed;
        summary_tile_->visible = !no_file;
        summary_name_->visible = !no_file;
        summary_publisher_->visible = !no_file;
        summary_rule_->visible = !no_file;

        summary_banner_->visible = true;
        if (no_file) {
            summary_banner_->set_kind(Banner::Kind::Info);
            summary_banner_->set_message(strings_.text(IDS_ERROR_NO_FILE));
            summary_banner_->bounds = {44.0f, 140.0f, 672.0f, 72.0f};
        } else {
            const bool cancelled = view.error_message == L"cancelled";
            const bool has_detail = !view.error_message.empty() && !cancelled;
            summary_banner_->set_kind(has_detail ? Banner::Kind::Warning : Banner::Kind::Info);
            if (has_detail) {
                std::wstring message = view.error_code;
                message += L"\r\n";
                message += view.error_message;
                summary_banner_->set_message(std::move(message));
                summary_banner_->bounds = {44.0f, 204.0f, 672.0f, 112.0f};
            } else {
                summary_banner_->set_message(
                    strings_.text(cancelled ? IDS_ERR_CANCELLED : IDS_ERROR_LOAD_FAILED));
                summary_banner_->bounds = {44.0f, 204.0f, 672.0f, 64.0f};
            }
        }

        summary_banner_->invalidate();
        nav_next_->enabled = false;
        nav_next_->invalidate();
        return;
    }

    summary_tile_->visible = true;
    summary_name_->visible = true;
    summary_publisher_->visible = true;
    summary_rule_->visible = true;

    summary_name_->set_text(view.display_name.empty()
                                ? (view.name.empty() ? strings_.text(IDS_COMMON_UNTITLED) : widen_utf8(view.name))
                                : widen_utf8(view.display_name));
    summary_publisher_->set_text(widen_utf8(view.publisher));

    summary_values_[0]->set_text(widen_utf8(view.version));
    summary_values_[1]->set_text(widen_utf8(view.architecture));
    summary_values_[2]->set_text(format_size(view.total_size));

    if (view.dependencies.empty()) {
        summary_values_[3]->set_text(strings_.text(IDS_SUMMARY_DEPS_NONE));
    } else {
        std::wstring joined;
        for (size_t index = 0; index < view.dependencies.size(); ++index) {
            if (index != 0) {
                joined += L"; ";
            }

            joined += widen_utf8(view.dependencies[index]);
        }

        summary_values_[3]->set_text(std::move(joined));
    }

    summary_description_->set_text(view.description.empty() ? strings_.text(IDS_COMMON_NONE)
                                                             : widen_utf8(view.description));

    set_rows_visible(true);
    summary_banner_->visible = false;

    summary_note_->text = strings_.text(IDS_SUMMARY_UPDATING);
    summary_note_->visible = view.already_installed;
    summary_note_->invalidate();

    nav_next_->enabled = true;
    nav_next_->invalidate();

    // 摘要页刚打开时预览还没回来，焦点只能落在「取消」；预览成功后把焦点交给
    // 主操作「下一步」，否则默认焦点会停在取消上（Enter 仍走默认按钮，但视觉不对）。
    if (hwnd() != nullptr && host_.focused_widget() == nav_cancel_) {
        host_.focus_widget(nav_next_);
    }
}

void Wizard::render_consent() {
    PackageView view;
    {
        const std::lock_guard<std::mutex> guard(shared_->mutex);
        view = shared_->preview;
    }

    heading_->set_text(strings_.text(IDS_CONSENT_HEADING));
    subheading_->visible = true;
    subheading_->set_text(strings_.text(IDS_CONSENT_SUBHEADING));

    // 「将要信任谁」讲清楚：每一条都是用户做出决定所必需的事实，而不是装饰。
    std::wstring details;
    const auto line = [&details](std::wstring_view label, const std::wstring& value) {
        details += label;
        details += L": ";
        details += value;
        details += L"\r\n";
    };

    line(strings_.get(IDS_CONSENT_LABEL_SUBJECT),
         view.subject.empty() ? strings_.text(IDS_COMMON_NONE) : widen_utf8(view.subject));

    if (view.has_certificate) {
        // 指纹按字节分组显示：64 个字符连成一片时人眼几乎无法核对。
        line(strings_.get(IDS_CONSENT_LABEL_SHA256), format_fingerprint(widen_utf8(view.thumbprint_sha256)));
        line(strings_.get(IDS_CONSENT_LABEL_SHA1), format_fingerprint(widen_utf8(view.thumbprint_sha1)));

        std::wstring validity = widen_utf8(view.not_before_utc);
        if (!view.not_after_utc.empty()) {
            validity += L"  \u2192  " + widen_utf8(view.not_after_utc);
        }

        line(strings_.get(IDS_CONSENT_LABEL_VALID), validity.empty() ? strings_.text(IDS_COMMON_NONE) : validity);

        // 「是否受信任」与「是否命中钉扎」合成一行：这两条是同一组结论，
        // 分开写会让 108 DIP 的详情框滚出第二屏，把安全关键的一条藏起来。
        details += strings_.get(IDS_CONSENT_LABEL_TRUSTED);
        details += L": ";
        details += strings_.text(view.already_trusted ? IDS_CONSENT_TRUSTED_YES : IDS_CONSENT_TRUSTED_NO);
        details += L"    ";
        details += strings_.get(IDS_CONSENT_LABEL_PINNED);
        details += L": ";
        details += strings_.text(view.pinned ? IDS_CONSENT_PINNED_YES : IDS_CONSENT_PINNED_NO);
        details += L"\r\n";
    } else {
        // 没有内嵌证书：安装器完全不碰证书存储，这一点必须说清楚。
        line(strings_.get(IDS_CONSENT_LABEL_SHA256), strings_.text(IDS_COMMON_NONE));
        line(strings_.get(IDS_CONSENT_LABEL_SHA1), strings_.text(IDS_COMMON_NONE));
        details += strings_.get(IDS_CONSENT_LABEL_TRUSTED);
        details += L": ";
        details += strings_.text(IDS_CONSENT_TRUSTED_YES);
        details += L"    ";
        details += strings_.get(IDS_CONSENT_LABEL_PINNED);
        details += L": ";
        details += strings_.text(IDS_CONSENT_PINNED_NO);
        details += L"\r\n";
    }

    consent_details_->set_text(std::move(details));

    // --all-users 只在清单声明允许时可用；每次进页都清空，上一次的勾选不能替这一次做决定。
    consent_all_users_->visible = view.allow_all_users;
    consent_all_users_->enabled = view.allow_all_users;
    consent_all_users_->set_checked(false);

    // 「信任此发布者」只在未命中钉扎时出现：命中白名单的发布者不需要额外信任决定。
    consent_trust_->visible = view.unpinned_publisher;
    consent_trust_->set_checked(false);
    consent_unpinned_->visible = view.unpinned_publisher;

    consent_accept_->set_checked(false);
    layout_consent();
    update_consent_gate();
}

void Wizard::update_consent_gate() {
    PackageView view;
    {
        const std::lock_guard<std::mutex> guard(shared_->mutex);
        view = shared_->preview;
    }

    const bool accepted = consent_accept_->checked();
    // 未命中钉扎时必须**额外**显式信任：白名单之外的发布者绝不能靠一个
    // 「同意安装」的动作顺带被信任。
    const bool trusted = view.unpinned_publisher ? consent_trust_->checked() : true;

    nav_next_->enabled = accepted && trusted;
    nav_next_->invalidate();

    // 「信任此发布者」本身也要先勾「我了解并同意」。
    consent_trust_->enabled = accepted;
    consent_trust_->invalidate();
}

void Wizard::layout_consent() {
    float y = 288.0f;

    consent_accept_->bounds.y = y;
    consent_accept_->invalidate();
    y += 28.0f + 6.0f;

    if (consent_trust_->visible) {
        consent_trust_->bounds.y = y;
        consent_trust_->invalidate();
        y += 28.0f + 6.0f;
    }

    if (consent_unpinned_->visible) {
        consent_unpinned_->bounds.y = y;
        consent_unpinned_->invalidate();
        y += 34.0f + 4.0f;
    }

    if (consent_all_users_->visible) {
        consent_all_users_->bounds.y = y;
        consent_all_users_->invalidate();
    }

    page_consent_->invalidate();
}

void Wizard::render_progress() {
    heading_->set_text(strings_.text(installing_ ? IDS_PROGRESS_HEADING : IDS_PROGRESS_UNINSTALL_HEADING));
    subheading_->visible = true;
    subheading_->set_text(strings_.text(IDS_PROGRESS_SUBHEADING));

    uint32_t percent = 0;
    bool waiting = false;
    PhaseView phase = PhaseView::Idle;
    std::vector<std::wstring> log_entries;
    {
        const std::lock_guard<std::mutex> guard(shared_->mutex);
        percent = shared_->percent;
        waiting = shared_->waiting_for_elevation;
        phase = shared_->phase;
        log_entries = shared_->phase_log;
    }

    if (phase == PhaseView::Failed) {
        phase_failed_ = true;
    } else if (phase != PhaseView::Idle) {
        last_phase_ = phase;
        phase_failed_ = false;
    }

    target_percent_ = static_cast<int>(std::min<uint32_t>(percent, 100));
    if (target_percent_ > static_cast<int>(shown_percent_)) {
        start_progress_animation();
    } else {
        shown_percent_ = static_cast<float>(target_percent_);
        progress_bar_->set_value_f(shown_percent_);
    }

    progress_percent_->set_text(
        format(strings_.get(IDS_PROGRESS_PERCENT).data(), static_cast<unsigned>(shown_percent_)));
    progress_status_->set_text(waiting ? strings_.text(IDS_PROGRESS_WAITING_ELEVATION)
                                       : strings_.text(IDS_PROGRESS_SUBHEADING));
    progress_timeline_->set_state(phase_index(last_phase_), phase_failed_);

    // 日志按「最新的在上面」排：TextView 没有公开的 scroll_to，倒序是让用户
    // 不用滚动就能看到当前事件的最简单可靠办法。
    std::wstring log;
    constexpr size_t kMaxLogLines = 12;
    const size_t start = log_entries.size() > kMaxLogLines ? log_entries.size() - kMaxLogLines : 0;
    for (size_t index = log_entries.size(); index > start; --index) {
        log += log_entries[index - 1];
        log += L"\r\n";
    }

    progress_log_->set_text(std::move(log));
    progress_log_->visible = !progress_log_->text.empty();
    progress_cancel_->invalidate();
}

std::wstring Wizard::result_body(const InstallView& view) const {
    std::wstring body;
    if (view.status == InstallStatus::Success) {
        body = strings_.text(installing_ ? IDS_RESULT_SUCCESS_BODY : IDS_RESULT_UNINSTALL_SUCCESS_BODY);
    } else if (view.status == InstallStatus::Cancelled) {
        body = strings_.text(view.elevation_declined ? IDS_RESULT_ELEVATION_DECLINED_BODY : IDS_RESULT_CANCELLED_BODY);
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
    StatusMark::Kind mark = StatusMark::Kind::Success;
    bool can_launch = false;
    std::wstring body;

    // 安装与卸载共用结果页；标题和成功正文必须按「这次干了什么」分开，
    // 否则卸载成功会显示「安装完成」。
    const bool uninstall = !installing_;
    if (!view.present) {
        heading = IDS_RESULT_TITLE;
        mark = StatusMark::Kind::Info;
        body = strings_.text(IDS_COMMON_NONE);
    } else if (view.status == InstallStatus::Success) {
        heading = uninstall ? IDS_RESULT_UNINSTALL_SUCCESS_HEADING : IDS_RESULT_SUCCESS_HEADING;
        body = result_body(view);
        can_launch = !uninstall && !view.family_name.empty() && !view.application_id.empty();
    } else if (view.status == InstallStatus::Cancelled) {
        heading = IDS_RESULT_CANCELLED_HEADING;
        mark = StatusMark::Kind::Info;
        body = result_body(view);
    } else {
        heading = uninstall ? IDS_RESULT_UNINSTALL_FAILURE_HEADING : IDS_RESULT_FAILURE_HEADING;
        mark = StatusMark::Kind::Failure;
        body = result_body(view);
    }

    heading_->set_text(strings_.text(heading));
    subheading_->visible = false;
    result_mark_->set_kind(mark);
    result_body_->set_text(std::move(body));

    if (view.log_path.empty()) {
        result_log_->set_text(strings_.text(IDS_COMMON_NONE));
    } else {
        result_log_->set_text(strings_.text(IDS_RESULT_LOG_PATH) + L": " + view.log_path);
    }

    result_launch_->enabled = can_launch;
    result_launch_->set_default(can_launch);
    result_launch_->invalidate();

    result_open_log_->enabled = !view.log_path.empty();
    result_open_log_->invalidate();

    result_copy_->enabled = view.present;
    result_copy_->invalidate();

    // 结果页只有一个默认按钮：能启动应用时是「启动应用」，否则是「关闭」。
    nav_cancel_->set_default(!can_launch);

    // 焦点可能在 render 之前就落到了此时才被禁用的「启动应用」上；挪回关闭，
    // 否则 MSAA 会同时报告 focused + unavailable。
    if (!can_launch && hwnd() != nullptr && host_.focused_widget() == result_launch_) {
        host_.focus_widget(nav_cancel_);
    }
}

void Wizard::render_manage() {
    heading_->set_text(strings_.text(IDS_MANAGE_HEADING));
    subheading_->visible = true;
    subheading_->set_text(strings_.text(IDS_MANAGE_SUBHEADING));

    ManageView view;
    {
        const std::lock_guard<std::mutex> guard(shared_->mutex);
        view = shared_->manage;
    }

    std::vector<std::vector<std::wstring>> rows;
    rows.reserve(view.entries.size());
    for (const ManageEntry& entry : view.entries) {
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
            certificate = certificate.substr(0, 16) + L"\u2026";
        }

        if (certificate.empty()) {
            certificate = strings_.get(IDS_COMMON_NONE);
        }

        rows.push_back({entry.name, entry.version, certificate, strings_.text(status_id)});
    }

    manage_table_->set_rows(std::move(rows));
    manage_uninstall_->enabled = false;
    manage_uninstall_->invalidate();
    update_manage_detail();
}

void Wizard::update_manage_detail() {
    ManageView view;
    {
        const std::lock_guard<std::mutex> guard(shared_->mutex);
        view = shared_->manage;
    }

    const int row = manage_table_->selected_row();
    std::wstring text;
    if (row >= 0 && row < static_cast<int>(view.entries.size())) {
        text = view.entries[static_cast<size_t>(row)].detail;
    }

    if (text.empty()) {
        text = view.notice;
    }

    if (text.empty() && view.entries.empty()) {
        text = strings_.text(IDS_MANAGE_EMPTY);
    }

    manage_detail_->set_text(std::move(text));
}

// ============================================================================
// 动作
// ============================================================================

void Wizard::on_nav_next() {
    if (page_ == WizardPage::Summary) {
        show_page(WizardPage::Consent);
        render_page();
        return;
    }

    if (page_ != WizardPage::Consent) {
        return;
    }

    // 未勾选时按钮本来就是禁用的；这里再挡一次，防止程序化触发。
    if (!consent_accept_->checked()) {
        ::MessageBoxW(hwnd(), strings_.get(IDS_CONSENT_NEED_ACCEPT).data(), strings_.get(IDS_CONSENT_TITLE).data(),
                      MB_OK | MB_ICONWARNING);
        return;
    }

    PackageView view;
    {
        const std::lock_guard<std::mutex> guard(shared_->mutex);
        view = shared_->preview;
    }

    if (view.unpinned_publisher && !consent_trust_->checked()) {
        ::MessageBoxW(hwnd(), strings_.get(IDS_CONSENT_NEED_TRUST).data(), strings_.get(IDS_CONSENT_TITLE).data(),
                      MB_OK | MB_ICONWARNING);
        return;
    }

    submit_consent(true);
}

void Wizard::on_nav_back() {
    if (page_ != WizardPage::Consent) {
        return;
    }

    // 还没开始装：撤回本次同意请求（编排线程会拿到「拒绝」并正常退出），
    // 然后回到摘要页。这样不会留下任何需要回滚的东西。
    stop_worker(worker_);
    submit_consent(false);
    show_page(WizardPage::Summary);
    render_page();
}

void Wizard::on_nav_cancel() {
    if (page_ == WizardPage::Progress) {
        cancel_install();
        return;
    }

    if (hwnd() != nullptr) {
        ::PostMessageW(hwnd(), WM_CLOSE, 0, 0);
    }
}

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

    exit_code_.store(exit_code::kSuccess);
    shown_percent_ = 0.0f;
    target_percent_ = 0;
    stop_progress_animation();
    progress_bar_->set_value_f(0.0f);
    progress_percent_->set_text(format(strings_.get(IDS_PROGRESS_PERCENT).data(), 0u));

    show_page(WizardPage::Summary);
    render_page();
    start_preview(worker_, hwnd(), container);
}

void Wizard::adopt_container(const std::wstring& container) {
    begin_preview(container);
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
        shared_->preapproved_consent = answers;
    }

    installing_ = true;
    elevation_foregrounded_ = false;
    last_phase_ = PhaseView::Idle;
    phase_failed_ = false;
    shown_percent_ = 0.0f;
    target_percent_ = 0;
    stop_progress_animation();
    progress_bar_->set_value_f(0.0f);
    progress_percent_->set_text(format(strings_.get(IDS_PROGRESS_PERCENT).data(), 0u));
    progress_timeline_->set_state(-1, false);

    show_page(WizardPage::Progress);
    render_page();
    start_install(worker_, hwnd());
}

void Wizard::submit_consent(bool approved) {
    std::shared_ptr<PendingConsent> pending;
    {
        const std::lock_guard<std::mutex> guard(shared_->mutex);
        pending = shared_->consent;
    }

    if (pending == nullptr) {
        // 没有待决问题（用户是在摘要页按「下一步」直接走到这里的）：
        // 把这次决定随安装线程一起带过去，而不是先启动安装、再由安装线程
        // 发一次同意请求 —— 那正是「要点两次下一步」的来源。
        if (!approved) {
            {
                const std::lock_guard<std::mutex> guard(shared_->mutex);
                shared_->preapproved_consent.reset();
            }

            exit_code_.store(exit_code::kCancelled);
            return;
        }

        ConsentAnswers answers;
        answers.approved = consent_accept_->checked();
        answers.remember_publisher = consent_trust_->checked();
        answers.all_users = consent_all_users_->checked();
        begin_install(answers);
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

    // 「信任此发布者」是 ConsentDecision.remember_publisher 的唯一来源。
    pending->approved = consent_accept_->checked();
    pending->remember_publisher = consent_trust_->checked();
    pending->all_users = consent_all_users_->checked();
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
    progress_status_->set_text(strings_.text(IDS_COMMON_CANCEL));
    progress_cancel_->enabled = false;
    progress_cancel_->invalidate();
}

void Wizard::begin_manage_load() {
    {
        const std::lock_guard<std::mutex> guard(shared_->mutex);
        shared_->manage = ManageView{};
    }

    start_manage_load(worker_, hwnd());
}

void Wizard::begin_uninstall() {
    const int row = manage_table_->selected_row();
    if (row < 0) {
        ::MessageBoxW(hwnd(), strings_.get(IDS_MANAGE_NO_SELECTION).data(), strings_.get(IDS_MANAGE_TITLE).data(),
                      MB_OK | MB_ICONINFORMATION);
        return;
    }

    const int answer = ::MessageBoxW(hwnd(), strings_.get(IDS_MANAGE_CONFIRM_UNINSTALL).data(),
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
    last_phase_ = PhaseView::Idle;
    phase_failed_ = false;
    shown_percent_ = 0.0f;
    target_percent_ = 0;
    stop_progress_animation();
    progress_bar_->set_value_f(0.0f);
    progress_percent_->set_text(format(strings_.get(IDS_PROGRESS_PERCENT).data(), 0u));
    progress_timeline_->set_state(-1, false);

    show_page(WizardPage::Progress);
    render_page();
    start_uninstall(worker_, hwnd(), install_id, family_name);
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

    const HINSTANCE result = ::ShellExecuteW(hwnd(), L"open", view.log_path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(result) <= 32) {
        ::MessageBoxW(hwnd(), strings_.get(IDS_ERROR_NOT_FOUND).data(), strings_.get(IDS_RESULT_OPEN_LOG).data(),
                      MB_OK | MB_ICONWARNING);
    }
}

void Wizard::open_log_directory() {
    ManageView view;
    {
        const std::lock_guard<std::mutex> guard(shared_->mutex);
        view = shared_->manage;
    }

    // 直接开数据根目录：日志文件名带时间戳，逐个列举没有意义。
    const std::wstring target = view.data_root.empty() ? std::wstring() : view.data_root + L"\\logs";
    if (target.empty()) {
        return;
    }

    const HINSTANCE result = ::ShellExecuteW(hwnd(), L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(result) <= 32) {
        ::MessageBoxW(hwnd(), strings_.get(IDS_ERROR_NOT_FOUND).data(), strings_.get(IDS_MANAGE_OPEN_LOGS).data(),
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

    // AUMID = <PackageFamilyName>!<ApplicationId>。
    const std::wstring aumid = view.family_name + L"!" + widen_utf8(view.application_id);
    try {
        msixpkg::activate_application(narrow_utf8(aumid));
    } catch (const msixpkg::Error&) {
        ::MessageBoxW(hwnd(), strings_.get(IDS_RESULT_LAUNCH_FAILED).data(), strings_.get(IDS_RESULT_LAUNCH).data(),
                      MB_OK | MB_ICONWARNING);
    } catch (const std::exception&) {
        ::MessageBoxW(hwnd(), strings_.get(IDS_RESULT_LAUNCH_FAILED).data(), strings_.get(IDS_RESULT_LAUNCH).data(),
                      MB_OK | MB_ICONWARNING);
    }
}

void Wizard::copy_to_clipboard(const std::wstring& text) const {
    if (text.empty() || ::OpenClipboard(hwnd()) == FALSE) {
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
    const HINSTANCE result =
        ::ShellExecuteW(hwnd(), L"open", L"ms-settings:appsfeatures", nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(result) <= 32) {
        ::MessageBoxW(hwnd(), strings_.get(IDS_ERROR_NOT_FOUND).data(), strings_.get(IDS_MANAGE_OPEN_SETTINGS).data(),
                      MB_OK | MB_ICONWARNING);
    }
}

void Wizard::show_repair_plan() {
    // 修复只输出建议，绝不改动系统（ops.h 的硬规则）。计算放在后台线程上。
    manage_detail_->set_text(strings_.text(IDS_MANAGE_REPAIR_INTRO));
    start_repair(worker_, hwnd());
}

// ============================================================================
// 进度动画
// ============================================================================

void Wizard::start_progress_animation() {
    if (progress_animating_) {
        return;
    }

    progress_animating_ = true;
    last_tick_ = std::chrono::steady_clock::now();
    host_.set_tick_handler([this] { tick_progress(); });
    host_.set_animating(true, 16);  // 进度条 60fps 就够了，没必要吃满刷新率
}

void Wizard::tick_progress() {
    const auto now = std::chrono::steady_clock::now();
    const double seconds = std::chrono::duration<double>(now - last_tick_).count();
    last_tick_ = now;

    const float target = static_cast<float>(target_percent_);
    const float delta = target - shown_percent_;
    if (delta <= 0.05f) {
        shown_percent_ = target;
        progress_bar_->set_value_f(shown_percent_);
        progress_percent_->set_text(
            format(strings_.get(IDS_PROGRESS_PERCENT).data(), static_cast<unsigned>(shown_percent_)));
        stop_progress_animation();
        return;
    }

    // 指数逼近 + 最小步长：百分比要「跟得上真实进度」，但也要真的收敛，
    // 不能因为最后 0.2% 一直停在 99%。
    const float step = std::max(0.6f, delta * std::min(1.0f, static_cast<float>(seconds) * 10.0f));
    shown_percent_ = std::min(target, shown_percent_ + step);
    progress_bar_->set_value_f(shown_percent_);
    progress_percent_->set_text(
        format(strings_.get(IDS_PROGRESS_PERCENT).data(), static_cast<unsigned>(shown_percent_)));
}

void Wizard::stop_progress_animation() {
    if (!progress_animating_) {
        return;
    }

    progress_animating_ = false;
    if (hwnd() != nullptr) {
        host_.set_tick_handler({});
        host_.set_animating(false);
    }
}

// ============================================================================
// 诊断报告（--ui-report）
// ============================================================================

void Wizard::write_ui_report() const {
    if (report_path_.empty()) {
        return;
    }

    std::wstring report;
    report += L"wizard=wren\n";
    report += L"page=";
    report += page_name(page_);
    report += L"\n";
    report += L"appearance=";
    report += appearance_name(host_.appearance());
    report += L"\n";
    report += L"material=";
    report += wren::material_name(host_.material_kind());
    report += L"\n";
    report += L"presenter=";
    report += host_.presenter_name();
    report += L"\n";
    report += L"dpi=";
    report += std::to_wstring(host_.dpi());
    report += L"\n";
    report += L"client=";
    report += std::to_wstring(host_.client_width_px());
    report += L"x";
    report += std::to_wstring(host_.client_height_px());
    report += L"\n";

    const wren::FrameStats stats = host_.frame_stats();
    report += L"frames=";
    report += std::to_wstring(stats.frames);
    report += L"\n";
    report += L"refresh_hz=";
    report += std::to_wstring(stats.refresh_hz);
    report += L"\n";
    report += L"interval_ms=";
    report += std::to_wstring(stats.interval_ms);
    report += L"\n";
    report += L"msaa.requests=";
    report += std::to_wstring(host_.accessibility_request_count());
    report += L"\n";
    report += L"heading=\"";
    report += one_line(heading_->text);
    report += L"\"\n";
    report += L"subheading=\"";
    report += one_line(subheading_->text);
    report += L"\"\n";

    const auto entries = host_.accessible_entries();
    report += L"childCount=";
    report += std::to_wstring(entries.size());
    report += L"\n";

    for (size_t index = 0; index < entries.size(); ++index) {
        const auto& entry = entries[index];
        const wren::Widget* widget = entry.widget;
        if (widget == nullptr) {
            continue;
        }

        wchar_t state[32]{};
        ::swprintf(state, 32, L"0x%08X", widget->accessible_state());

        report += L"child[";
        report += std::to_wstring(index + 1);
        report += L"] id=";
        report += std::to_wstring(widget->control_id);
        report += L" role=";
        report += role_name(widget->accessible_role());
        report += L" row=";
        report += std::to_wstring(entry.row);
        report += L" name=\"";
        report += one_line(widget->accessible_name());
        report += L"\" value=\"";
        report += one_line(widget->accessible_value());
        report += L"\" state=";
        report += state;
        report += L" bounds=(";
        report += std::to_wstring(static_cast<int>(widget->bounds.x));
        report += L",";
        report += std::to_wstring(static_cast<int>(widget->bounds.y));
        report += L",";
        report += std::to_wstring(static_cast<int>(widget->bounds.width));
        report += L",";
        report += std::to_wstring(static_cast<int>(widget->bounds.height));
        report += L")\n";
    }

    const std::filesystem::path report_file(report_path_);
    std::error_code ignored;
    if (report_file.has_parent_path()) {
        std::filesystem::create_directories(report_file.parent_path(), ignored);
    }

    std::ofstream file(report_file, std::ios::binary | std::ios::trunc);
    if (!file) {
        return;
    }

    file << msixpkg::narrow_utf8(report);
}

}  // namespace

// ============================================================================
// 入口
// ============================================================================

int run(void* instance, const wchar_t* command_line) {
    const HINSTANCE module = static_cast<HINSTANCE>(instance);

    INITCOMMONCONTROLSEX controls{};
    controls.dwSize = sizeof(controls);
    controls.dwICC = ICC_STANDARD_CLASSES | ICC_PROGRESS_CLASS | ICC_LISTVIEW_CLASSES;
    ::InitCommonControlsEx(&controls);

    Strings strings;
    strings.load(module);

    const CommandLine parsed = parse_command_line(command_line == nullptr ? L"" : command_line);

    // ---- GUI 不实现的 CLI 子命令：立刻失败，绝不当容器路径 ----
    // 这些子命令只有 msixpkg-native.exe 实现。把它们当容器路径会一路走到
    // 单实例交接 —— 那个窗口可能正卡在等这次「提权」返回，三方互等就死锁了。
    if (!parsed.unsupported_subcommand.empty()) {
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
        HWND existing = ::FindWindowW(kWrenWindowClass, nullptr);
        if (existing == nullptr) {
            existing = ::FindWindowW(kLegacyWindowClass, nullptr);
        }

        if (existing != nullptr) {
            ::SetForegroundWindow(existing);
            ::ShowWindow(existing, SW_RESTORE);

            if (!parsed.container.empty()) {
                // WM_COPYDATA 的缓冲只在接收方处理消息期间有效，接收方必须立刻拷走。
                const std::wstring path = parsed.container.wstring();
                COPYDATASTRUCT data{};
                data.dwData = kCopyDataMagic;
                data.cbData = static_cast<DWORD>((path.size() + 1) * sizeof(wchar_t));
                data.lpData = const_cast<wchar_t*>(path.c_str());

                // 必须带超时：对端要是卡住了，第二个实例不能跟着一起卡死。
                DWORD_PTR reply = 0;
                const LRESULT delivered = ::SendMessageTimeoutW(
                    existing, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&data), SMTO_ABORTIFHUNG | SMTO_BLOCK,
                    kCopyDataTimeoutMs, &reply);
                if (delivered == 0 || reply == kCopyDataBusy) {
                    ::MessageBoxW(nullptr, strings.get(IDS_PROGRESS_BUSY).data(), strings.get(IDS_PROGRESS_TITLE).data(),
                                  MB_OK | MB_ICONINFORMATION);
                    result = exit_code::kPrecondition;
                }
            }
        }

        if (instance_mutex != nullptr) {
            ::CloseHandle(instance_mutex);
        }

        return result;
    }

    auto shared = std::make_shared<SharedState>();
    Wizard wizard(module, shared, std::move(strings));
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
        wizard.show(parsed);
    }

    const int code = wizard.run();
    if (instance_mutex != nullptr) {
        ::CloseHandle(instance_mutex);
    }

    return code;
}

}  // namespace msixpkg::gui

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, PWSTR command_line, int show) {
    (void)previous;
    (void)show;

    return msixpkg::gui::run(instance, command_line);
}
