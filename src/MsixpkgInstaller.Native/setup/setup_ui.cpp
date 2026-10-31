#include "setup_ui.h"

#include "../res/resource.h"
#include "controls.h"
#include "gui_widgets.h"
#include "host.h"
#include "setup_engine.h"
#include "setup_registry.h"
#include "setup_resources.h"
#include "setup_strings.h"
#include "setup_util.h"
#include "setup_version.h"
#include "setup_widgets.h"

#include <objbase.h>
#include <ShObjIdl.h>
#include <commctrl.h>
#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace msixpkg::setup {
namespace {

constexpr UINT WM_SETUP_PROGRESS = WM_APP + 0x71;
constexpr UINT_PTR kSubclassId = 0x5E7;
constexpr int kWindowWidth = 760;
constexpr int kWindowHeight = 560;

enum class Mode { Install, Uninstall };
enum class Page { Welcome, Options, Progress, Finish, Confirm };

const wchar_t* page_name(Page page) {
    switch (page) {
        case Page::Welcome:
            return L"welcome";
        case Page::Options:
            return L"options";
        case Page::Progress:
            return L"progress";
        case Page::Finish:
            return L"finish";
        case Page::Confirm:
            return L"confirm";
    }
    return L"unknown";
}

const wchar_t* appearance_name(wren::Appearance appearance) {
    switch (appearance) {
        case wren::Appearance::Light:
            return L"light";
        case wren::Appearance::Dark:
            return L"dark";
        case wren::Appearance::HighContrast:
            return L"hc";
        case wren::Appearance::Auto:
            return L"auto";
    }
    return L"unknown";
}

const wchar_t* role_name(wren::Role role) {
    switch (role) {
        case wren::Role::Panel:
            return L"panel";
        case wren::Role::Label:
            return L"label";
        case wren::Role::Button:
            return L"button";
        case wren::Role::Checkbox:
            return L"checkbox";
        case wren::Role::Progress:
            return L"progress";
        case wren::Role::Text:
            return L"text";
        case wren::Role::Table:
            return L"table";
        case wren::Role::Row:
            return L"row";
        case wren::Role::ScrollBar:
            return L"scrollbar";
    }
    return L"unknown";
}

std::wstring one_line(std::wstring_view text) {
    std::wstring result;
    for (const wchar_t ch : text) {
        result.push_back(ch == L'\r' || ch == L'\n' ? L' ' : ch);
    }
    return result;
}

std::wstring replace_token(std::wstring text, std::wstring_view token, const std::wstring& value) {
    size_t position = 0;
    while ((position = text.find(token, position)) != std::wstring::npos) {
        text.replace(position, token.size(), value);
        position += value.size();
    }
    return text;
}

bool pick_folder(HWND owner, const std::filesystem::path& initial, std::filesystem::path& result) {
    IFileOpenDialog* dialog = nullptr;
    HRESULT hr = ::CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_IFileOpenDialog, reinterpret_cast<void**>(&dialog));
    if (FAILED(hr) || dialog == nullptr) {
        return false;
    }

    DWORD options = 0;
    if (SUCCEEDED(dialog->GetOptions(&options))) {
        dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    }

    if (!initial.empty()) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(::SHCreateItemFromParsingName(initial.c_str(), nullptr, IID_IShellItem,
                                                    reinterpret_cast<void**>(&item)))) {
            dialog->SetFolder(item);
            item->Release();
        }
    }

    bool picked = false;
    if (SUCCEEDED(dialog->Show(owner))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item)) && item != nullptr) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path != nullptr) {
                result = std::filesystem::path(path);
                ::CoTaskMemFree(path);
                picked = true;
            }
            item->Release();
        }
    }
    dialog->Release();
    return picked;
}

class Wizard {
public:
    Wizard(HINSTANCE instance, Mode mode, const Options& options, const Strings& strings, Log& log)
        : instance_(instance),
          mode_(mode),
          options_(options),
          strings_(strings),
          log_(log),
          styles_(wren::make_theme(wren::Appearance::Auto, false)) {
        if (mode_ == Mode::Install) {
            plan_ = plan_install(options_, log_);
            // 命令行显式给了 /D= 时，切换安装范围不能把它覆盖掉。
            dir_user_chosen_ = options_.install_dir_explicit;
        } else {
            plan_ = plan_uninstall(options_, log_);
        }
    }

    ~Wizard() { shutdown(); }

    int run();

private:
    static LRESULT CALLBACK subclass_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam,
                                          UINT_PTR id, DWORD_PTR ref);

    bool create();
    void build_tree(wren::Panel& root);
    void build_shell(wren::Panel& root);
    void build_welcome(wren::Panel& page);
    void build_options(wren::Panel& page);
    void build_progress(wren::Panel& page);
    void build_finish(wren::Panel& page);
    void build_confirm(wren::Panel& page);
    void layout();
    void apply_diagnostics();
    void synthesize_result(std::wstring_view kind);
    void synthesize_progress(int percent);
    std::wstring phase_label(int phase) const;
    void show_page(Page page);
    void update_chrome();
    void apply_mode(InstallMode mode, bool update_dir);
    std::filesystem::path default_dir(InstallMode mode) const;
    void browse();
    void start_install();
    void start_uninstall();
    void on_progress(const ProgressUpdate& update);
    void finish_with(const InstallResult& result);
    void on_close();
    void on_next();
    void on_back();
    void on_cancel();
    void launch_wizard();
    void open_log();
    void append_log_line(const std::wstring& line);
    void post(ProgressUpdate update);
    void shutdown();
    void write_report();

    HWND hwnd() const { return host_.hwnd(); }

    HINSTANCE instance_ = nullptr;
    Mode mode_ = Mode::Install;
    Options options_;
    const Strings& strings_;
    Log& log_;
    Plan plan_;
    wren::Theme styles_;
    wren::Host host_;

    Page page_ = Page::Welcome;
    InstallMode selected_mode_ = InstallMode::CurrentUser;
    bool dir_user_chosen_ = false;
    bool busy_ = false;
    bool diagnostics_ = false;
    std::atomic<bool> alive_{true};
    bool self_delete_pending_ = false;
    bool report_written_ = false;
    int exit_code_ = kExitPrecondition;
    InstallResult result_{};
    std::mutex result_mutex_;
    std::jthread worker_;
    std::wstring heading_text_;
    std::wstring progress_text_;

    // shell
    wren::Label* heading_ = nullptr;
    wren::Label* subheading_ = nullptr;
    msixpkg::gui::StepRail* steps_ = nullptr;
    msixpkg::gui::Divider* divider_ = nullptr;
    wren::Button* nav_back_ = nullptr;
    wren::Button* nav_next_ = nullptr;
    wren::Button* nav_cancel_ = nullptr;

    // pages
    wren::Panel* page_welcome_ = nullptr;
    wren::Panel* page_options_ = nullptr;
    wren::Panel* page_progress_ = nullptr;
    wren::Panel* page_finish_ = nullptr;
    wren::Panel* page_confirm_ = nullptr;

    // welcome
    msixpkg::gui::Card* welcome_card_ = nullptr;
    wren::Label* welcome_body_ = nullptr;
    msixpkg::gui::Card* welcome_list_card_ = nullptr;
    wren::Label* welcome_list_ = nullptr;

    // options
    ChoiceCard* choice_all_ = nullptr;
    ChoiceCard* choice_current_ = nullptr;
    msixpkg::gui::Card* dir_card_ = nullptr;
    wren::Label* dir_value_ = nullptr;
    wren::Button* browse_ = nullptr;
    wren::Label* components_label_ = nullptr;
    wren::Checkbox* check_assoc_ = nullptr;
    wren::Checkbox* check_path_ = nullptr;
    wren::Checkbox* check_desktop_ = nullptr;
    wren::Label* assoc_detail_ = nullptr;
    wren::Label* path_detail_ = nullptr;
    wren::Label* desktop_detail_ = nullptr;
    msixpkg::gui::Banner* banner_ = nullptr;

    // progress
    wren::Label* progress_status_ = nullptr;
    wren::Label* progress_percent_ = nullptr;
    wren::ProgressBar* progress_bar_ = nullptr;
    msixpkg::gui::PhaseTimeline* progress_timeline_ = nullptr;
    wren::TextView* progress_log_ = nullptr;
    std::vector<std::wstring> progress_lines_;
    std::atomic<bool> waiting_for_elevation_{false};

    // finish
    msixpkg::gui::StatusMark* result_mark_ = nullptr;
    wren::Label* result_title_ = nullptr;
    wren::TextView* result_body_ = nullptr;
    wren::Checkbox* run_after_ = nullptr;
    wren::Button* open_log_ = nullptr;
    wren::Label* result_log_ = nullptr;

    // confirm
    msixpkg::gui::Banner* confirm_banner_ = nullptr;
    wren::TextView* confirm_body_ = nullptr;
};

int Wizard::run() {
    if (!create()) {
        return kExitPrecondition;
    }
    const int code = host_.run();
    shutdown();
    return code;
}

LRESULT CALLBACK Wizard::subclass_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam,
                                       UINT_PTR id, DWORD_PTR ref) {
    (void)id;
    auto* self = reinterpret_cast<Wizard*>(ref);
    if (self == nullptr) {
        return ::DefSubclassProc(hwnd, message, wparam, lparam);
    }

    if (message == WM_SETUP_PROGRESS) {
        auto* update = reinterpret_cast<ProgressUpdate*>(lparam);
        if (update != nullptr) {
            self->on_progress(*update);
            delete update;
        }
        return 0;
    }
    if (message == WM_CLOSE) {
        self->on_close();
        return 1;
    }
    return ::DefSubclassProc(hwnd, message, wparam, lparam);
}

bool Wizard::create() {
    auto root = std::make_unique<wren::Panel>();
    root->bounds = {0.0f, 0.0f, static_cast<float>(kWindowWidth), static_cast<float>(kWindowHeight)};
    build_tree(*root);

    wren::HostOptions host_options;
    host_options.title = strings_.text(mode_ == Mode::Install ? Str::WindowTitleInstall
                                                              : Str::WindowTitleUninstall);
    host_options.window_class = L"MsixpkgSetupWindow";
    host_options.width_dip = kWindowWidth;
    host_options.height_dip = kWindowHeight;
    host_options.appearance = wren::Appearance::Auto;
    host_options.material = wren::MaterialPreference::Auto;
    host_options.on_escape = [this] {
        if (hwnd() != nullptr) {
            ::PostMessageW(hwnd(), WM_CLOSE, 0, 0);
        }
    };

    if (!host_.create(instance_, std::move(root), host_options)) {
        return false;
    }

    const HICON icon = static_cast<HICON>(::LoadIconW(instance_, MAKEINTRESOURCEW(IDI_MSIXPKG)));
    if (icon != nullptr) {
        ::SendMessageW(hwnd(), WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(icon));
        ::SendMessageW(hwnd(), WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(icon));
    }

    ::SetWindowSubclass(hwnd(), &Wizard::subclass_proc, kSubclassId, reinterpret_cast<DWORD_PTR>(this));
    apply_diagnostics();
    return true;
}

void Wizard::build_tree(wren::Panel& root) {
    build_shell(root);

    page_welcome_ = root.emplace<wren::Panel>();
    page_options_ = root.emplace<wren::Panel>();
    page_progress_ = root.emplace<wren::Panel>();
    page_finish_ = root.emplace<wren::Panel>();
    page_confirm_ = root.emplace<wren::Panel>();

    build_welcome(*page_welcome_);
    build_options(*page_options_);
    build_progress(*page_progress_);
    build_finish(*page_finish_);
    build_confirm(*page_confirm_);

    page_welcome_->visible = false;
    page_options_->visible = false;
    page_progress_->visible = false;
    page_finish_->visible = false;
    page_confirm_->visible = false;

    layout();
}

void Wizard::layout() {
    heading_->bounds = {28.0f, 18.0f, 640.0f, 30.0f};
    subheading_->bounds = {28.0f, 48.0f, 704.0f, 20.0f};
    steps_->bounds = {28.0f, 78.0f, 704.0f, 38.0f};
    divider_->bounds = {28.0f, 122.0f, 704.0f, 1.0f};

    // 页面 Panel 只覆盖内容区，**不能**盖住底部按钮栏：
    // Panel::hit_test 先问子节点，再问自己的 bounds；如果页面铺满整个客户区，
    // 后添加的页面会抢走 502..536 的点击，底部按钮就「看得到、点不到」。
    // 底部按钮从 y=502 开始，页面留到 496（最靠下的 banner 到 494）。
    for (wren::Panel* page : {page_welcome_, page_options_, page_progress_, page_finish_, page_confirm_}) {
        page->bounds = {0.0f, 0.0f, static_cast<float>(kWindowWidth), 496.0f};
    }

    welcome_card_->bounds = {28.0f, 140.0f, 704.0f, 160.0f};
    welcome_body_->bounds = {48.0f, 158.0f, 664.0f, 124.0f};
    welcome_list_card_->bounds = {28.0f, 316.0f, 704.0f, 116.0f};
    welcome_list_->bounds = {48.0f, 332.0f, 664.0f, 86.0f};

    choice_all_->bounds = {28.0f, 140.0f, 336.0f, 72.0f};
    choice_current_->bounds = {368.0f, 140.0f, 364.0f, 72.0f};
    dir_value_->bounds = {40.0f, 252.0f, 536.0f, 26.0f};
    dir_card_->bounds = {28.0f, 246.0f, 560.0f, 38.0f};
    browse_->bounds = {600.0f, 246.0f, 132.0f, 38.0f};
    components_label_->bounds = {28.0f, 300.0f, 400.0f, 20.0f};
    check_assoc_->bounds = {28.0f, 324.0f, 704.0f, 22.0f};
    assoc_detail_->bounds = {52.0f, 346.0f, 680.0f, 18.0f};
    check_path_->bounds = {28.0f, 368.0f, 704.0f, 22.0f};
    path_detail_->bounds = {52.0f, 390.0f, 680.0f, 18.0f};
    check_desktop_->bounds = {28.0f, 412.0f, 704.0f, 22.0f};
    desktop_detail_->bounds = {52.0f, 434.0f, 680.0f, 18.0f};
    banner_->bounds = {28.0f, 458.0f, 704.0f, 36.0f};

    progress_status_->bounds = {28.0f, 136.0f, 600.0f, 24.0f};
    progress_percent_->bounds = {640.0f, 136.0f, 92.0f, 24.0f};
    progress_bar_->bounds = {28.0f, 166.0f, 704.0f, 6.0f};
    progress_timeline_->bounds = {28.0f, 190.0f, 704.0f, 96.0f};
    progress_log_->bounds = {28.0f, 300.0f, 704.0f, 170.0f};

    result_mark_->bounds = {28.0f, 140.0f, 52.0f, 52.0f};
    result_title_->bounds = {96.0f, 150.0f, 600.0f, 32.0f};
    result_body_->bounds = {28.0f, 210.0f, 704.0f, 120.0f};
    result_log_->bounds = {28.0f, 338.0f, 704.0f, 20.0f};
    run_after_->bounds = {28.0f, 370.0f, 400.0f, 26.0f};
    open_log_->bounds = {28.0f, 404.0f, 160.0f, 32.0f};

    confirm_banner_->bounds = {28.0f, 140.0f, 704.0f, 72.0f};
    confirm_body_->bounds = {28.0f, 226.0f, 704.0f, 220.0f};

    nav_cancel_->bounds = {28.0f, 502.0f, 120.0f, 34.0f};
    nav_back_->bounds = {520.0f, 502.0f, 100.0f, 34.0f};
    nav_next_->bounds = {632.0f, 502.0f, 100.0f, 34.0f};
}

void Wizard::build_shell(wren::Panel& root) {
    heading_ = root.emplace<wren::Label>(L"", styles_.title);
    heading_->control_id = IDC_SETUP_HEADING;
    subheading_ = root.emplace<wren::Label>(L"", styles_.body);
    subheading_->set_tone(wren::LabelTone::Secondary);
    subheading_->control_id = IDC_SETUP_SUBHEADING;

    steps_ = root.emplace<msixpkg::gui::StepRail>();
    steps_->control_id = IDC_SETUP_STEPS;
    if (mode_ == Mode::Install) {
        steps_->set_labels({strings_.text(Str::StepOptions), strings_.text(Str::StepProgress),
                            strings_.text(Str::StepFinish)});
    } else {
        steps_->set_labels({strings_.text(Str::StepConfirm), strings_.text(Str::StepProgress),
                            strings_.text(Str::StepFinish)});
    }

    divider_ = root.emplace<msixpkg::gui::Divider>();

    nav_back_ = root.emplace<wren::Button>(strings_.text(Str::Back), [this] { on_back(); });
    nav_back_->control_id = IDC_SETUP_NAV_BACK;
    nav_next_ = root.emplace<wren::Button>(strings_.text(Str::Next), [this] { on_next(); });
    nav_next_->control_id = IDC_SETUP_NAV_NEXT;
    nav_next_->set_primary(true);
    nav_cancel_ = root.emplace<wren::Button>(strings_.text(Str::Cancel), [this] { on_cancel(); });
    nav_cancel_->control_id = IDC_SETUP_NAV_CANCEL;
}

void Wizard::build_welcome(wren::Panel& page) {
    welcome_card_ = page.emplace<msixpkg::gui::Card>();
    welcome_card_->set_tone(msixpkg::gui::Card::Tone::Surface);

    wren::TextStyle body = styles_.body;
    body.wrap = true;
    welcome_body_ = page.emplace<wren::Label>(strings_.text(Str::WelcomeBody), body);
    welcome_body_->set_tone(wren::LabelTone::Secondary);

    welcome_list_card_ = page.emplace<msixpkg::gui::Card>();
    welcome_list_card_->set_tone(msixpkg::gui::Card::Tone::Accent);
    wren::TextStyle list_style = styles_.body;
    list_style.size = 13.0f;
    list_style.wrap = true;
    welcome_list_ = page.emplace<wren::Label>(strings_.text(Str::WelcomeList), list_style);
    welcome_list_->set_tone(wren::LabelTone::Secondary);
    welcome_list_->control_id = IDC_SETUP_WELCOME_LIST;
}

void Wizard::build_options(wren::Panel& page) {
    choice_all_ = page.emplace<ChoiceCard>(strings_.text(Str::ScopeAllUsers),
                                           strings_.text(Str::ScopeAllUsersDetail),
                                           [this] { apply_mode(InstallMode::AllUsers, true); });
    choice_all_->control_id = IDC_SETUP_CHOICE_ALL;
    choice_current_ = page.emplace<ChoiceCard>(strings_.text(Str::ScopeCurrentUser),
                                               strings_.text(Str::ScopeCurrentUserDetail),
                                               [this] { apply_mode(InstallMode::CurrentUser, true); });
    choice_current_->control_id = IDC_SETUP_CHOICE_CURRENT;

    dir_card_ = page.emplace<msixpkg::gui::Card>();
    dir_value_ = page.emplace<wren::Label>(L"", styles_.body);
    dir_value_->set_ellipsis(true);
    dir_value_->control_id = IDC_SETUP_DIR_VALUE;

    browse_ = page.emplace<wren::Button>(strings_.text(Str::Browse), [this] { browse(); });
    browse_->control_id = IDC_SETUP_BROWSE;

    components_label_ = page.emplace<wren::Label>(strings_.text(Str::ComponentsLabel), styles_.body);
    components_label_->set_style(styles_.caption);
    components_label_->set_tone(wren::LabelTone::Secondary);
    components_label_->control_id = IDC_SETUP_COMPONENTS;

    check_assoc_ = page.emplace<wren::Checkbox>(strings_.text(Str::AssocCheck));
    check_assoc_->control_id = IDC_SETUP_CHECK_ASSOC;
    check_assoc_->set_checked(!options_.no_assoc);

    check_path_ = page.emplace<wren::Checkbox>(strings_.text(Str::PathCheck));
    check_path_->control_id = IDC_SETUP_CHECK_PATH;
    check_path_->set_checked(!options_.no_path);

    check_desktop_ = page.emplace<wren::Checkbox>(strings_.text(Str::DesktopCheck));
    check_desktop_->control_id = IDC_SETUP_CHECK_DESKTOP;
    check_desktop_->set_checked(options_.desktop);

    assoc_detail_ = page.emplace<wren::Label>(strings_.text(Str::AssocCheckDetail), styles_.caption);
    assoc_detail_->set_tone(wren::LabelTone::Secondary);
    path_detail_ = page.emplace<wren::Label>(strings_.text(Str::PathCheckDetail), styles_.caption);
    path_detail_->set_tone(wren::LabelTone::Secondary);
    desktop_detail_ = page.emplace<wren::Label>(strings_.text(Str::DesktopCheckDetail), styles_.caption);
    desktop_detail_->set_tone(wren::LabelTone::Secondary);

    banner_ = page.emplace<msixpkg::gui::Banner>();
    banner_->set_kind(msixpkg::gui::Banner::Kind::Warning);
    banner_->control_id = IDC_SETUP_BANNER;
}

void Wizard::build_progress(wren::Panel& page) {
    progress_status_ = page.emplace<wren::Label>(L"", styles_.body);
    progress_status_->control_id = IDC_SETUP_PROGRESS_STATUS;
    progress_percent_ = page.emplace<wren::Label>(L"0%", styles_.body);
    progress_percent_->set_align(wren::TextAlign::Right);
    progress_percent_->control_id = IDC_SETUP_PROGRESS_PERCENT;

    progress_bar_ = page.emplace<wren::ProgressBar>();
    progress_bar_->control_id = IDC_SETUP_PROGRESS_BAR;

    progress_timeline_ = page.emplace<msixpkg::gui::PhaseTimeline>();
    progress_timeline_->control_id = IDC_SETUP_PROGRESS_PHASES;
    if (mode_ == Mode::Install) {
        progress_timeline_->set_items({strings_.text(Str::PhaseFiles), strings_.text(Str::PhaseShortcuts),
                                       strings_.text(Str::PhaseRecords), strings_.text(Str::PhaseAssoc),
                                       strings_.text(Str::PhasePath)});
    } else {
        progress_timeline_->set_items(
            {strings_.text(Str::PhaseRemoveShortcuts), strings_.text(Str::PhaseRemoveRegistry),
             strings_.text(Str::PhaseRemovePath), strings_.text(Str::PhaseRemoveFiles),
             strings_.text(Str::PhaseCleanup)});
    }
    progress_timeline_->set_state(-1, false);

    progress_log_ = page.emplace<wren::TextView>(L"", styles_.mono);
    progress_log_->set_read_only(true);
    progress_log_->set_show_scrollbar(true);
    progress_log_->control_id = IDC_SETUP_PROGRESS_LOG;
}

void Wizard::build_finish(wren::Panel& page) {
    result_mark_ = page.emplace<msixpkg::gui::StatusMark>();
    result_mark_->set_kind(msixpkg::gui::StatusMark::Kind::Info);
    result_mark_->control_id = IDC_SETUP_RESULT_MARK;

    result_title_ = page.emplace<wren::Label>(L"", styles_.heading);
    result_title_->control_id = IDC_SETUP_RESULT_TITLE;

    result_body_ = page.emplace<wren::TextView>(L"", styles_.body);
    result_body_->set_read_only(true);
    result_body_->set_show_scrollbar(false);
    result_body_->control_id = IDC_SETUP_RESULT_BODY;

    result_log_ = page.emplace<wren::Label>(L"", styles_.caption);
    result_log_->set_tone(wren::LabelTone::Secondary);
    result_log_->set_ellipsis(true);

    run_after_ = page.emplace<wren::Checkbox>(strings_.text(Str::RunAfter));
    run_after_->control_id = IDC_SETUP_RUN_AFTER;
    run_after_->set_checked(true);

    open_log_ = page.emplace<wren::Button>(strings_.text(Str::OpenLog), [this] { open_log(); });
    open_log_->control_id = IDC_SETUP_OPEN_LOG;
}

void Wizard::build_confirm(wren::Panel& page) {
    confirm_banner_ = page.emplace<msixpkg::gui::Banner>();
    confirm_banner_->set_kind(msixpkg::gui::Banner::Kind::Warning);

    const std::filesystem::path state = find_state_records();
    if (!state.empty()) {
        confirm_banner_->set_message(
            strings_.format(Str::StateKeptNote, {{L"{state}", state.wstring()}}));
    } else {
        confirm_banner_->visible = false;
    }

    confirm_body_ = page.emplace<wren::TextView>(strings_.text(Str::ConfirmUninstallBody), styles_.body);
    confirm_body_->set_read_only(true);
    confirm_body_->set_show_scrollbar(true);
    confirm_body_->control_id = IDC_SETUP_CONFIRM_BODY;
}

void Wizard::apply_mode(InstallMode mode, bool update_dir) {
    selected_mode_ = mode;
    options_.mode = mode;
    choice_all_->set_selected(mode == InstallMode::AllUsers);
    choice_current_->set_selected(mode == InstallMode::CurrentUser);

    if (update_dir && !dir_user_chosen_) {
        options_.install_dir = default_dir(mode);
    }
    dir_value_->set_text(options_.install_dir.wstring());

    if (mode == InstallMode::AllUsers && !is_process_elevated()) {
        banner_->set_kind(msixpkg::gui::Banner::Kind::Warning);
        if (plan_.existing_machine) {
            banner_->set_message(
                strings_.format(Str::ExistingMachineNote, {{L"{dir}", options_.install_dir.wstring()}}));
        } else {
            banner_->set_message(strings_.text(Str::AdminNote));
        }
        banner_->visible = true;
    } else if (mode == InstallMode::AllUsers && plan_.existing_machine) {
        banner_->set_kind(msixpkg::gui::Banner::Kind::Info);
        banner_->set_message(strings_.text(Str::ExistingInstallNote));
        banner_->visible = true;
    } else if (mode == InstallMode::CurrentUser && plan_.existing_user) {
        banner_->set_kind(msixpkg::gui::Banner::Kind::Info);
        banner_->set_message(
            strings_.format(Str::ExistingUserNote, {{L"{dir}", options_.install_dir.wstring()}}));
        banner_->visible = true;
    } else {
        banner_->visible = false;
    }

    host_.invalidate();
}

std::filesystem::path Wizard::default_dir(InstallMode mode) const {
    if (plan_.mode == mode && !plan_.install_dir.empty()) {
        return plan_.install_dir;
    }

    const HKEY root = mode == InstallMode::AllUsers ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER;
    std::wstring existing;
    bool exists = false;
    if (reg_read_string(root, kCfgKey, L"InstallDir", existing, exists) && exists && !existing.empty()) {
        return std::filesystem::path(existing);
    }

    if (mode == InstallMode::AllUsers) {
        return program_files_x64() / kProductFolder;
    }
    return local_app_data() / L"Programs" / kProductFolder;
}

void Wizard::browse() {
    std::filesystem::path chosen;
    if (pick_folder(hwnd(), options_.install_dir, chosen)) {
        options_.install_dir = chosen;
        options_.install_dir_explicit = true;
        dir_user_chosen_ = true;
        dir_value_->set_text(chosen.wstring());
        host_.invalidate();
    }
}

void Wizard::start_install() {
    Options install = options_;
    install.silent = false;
    install.mode = selected_mode_;
    install.install_dir = options_.install_dir;
    install.install_dir_explicit = true;
    install.no_assoc = !check_assoc_->checked();
    install.no_path = !check_path_->checked();
    install.desktop = check_desktop_->checked();
    install.ui_page = UiPage::None;
    install.ui_result.clear();
    install.report_path.clear();
    install.exit_after_ms = 0;

    progress_lines_.clear();
    progress_log_->set_text(L"");
    progress_bar_->set_marquee(false);
    progress_bar_->set_value(0);
    progress_percent_->set_text(L"0%");
    progress_status_->set_text(strings_.text(Str::ProgressTitleInstall));
    progress_timeline_->set_state(-1, false);
    waiting_for_elevation_.store(false);
    busy_ = true;
    host_.set_animating(true, 16);
    show_page(Page::Progress);

    worker_ = std::jthread([this, install] {
        // 编排线程要自己初始化 COM：快捷方式（IShellLink）与提权（ShellExecuteEx）
        // 都在这个线程上发生，不能借用 UI 线程的 STA。
        const HRESULT com = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        const InstallResult result =
            run_install(install, strings_, log_, [this](const ProgressUpdate& update) { post(update); });
        {
            const std::lock_guard<std::mutex> guard(result_mutex_);
            result_ = result;
        }
        if (SUCCEEDED(com)) {
            ::CoUninitialize();
        }
    });
}

void Wizard::start_uninstall() {
    Options uninstall = options_;
    uninstall.silent = false;
    uninstall.mode = plan_.mode == InstallMode::AllUsers ? InstallMode::AllUsers : InstallMode::CurrentUser;
    uninstall.install_dir = plan_.install_dir;
    uninstall.install_dir_explicit = true;
    uninstall.ui_page = UiPage::None;
    uninstall.ui_result.clear();
    uninstall.report_path.clear();
    uninstall.exit_after_ms = 0;

    progress_lines_.clear();
    progress_log_->set_text(L"");
    progress_bar_->set_marquee(false);
    progress_bar_->set_value(0);
    progress_percent_->set_text(L"0%");
    progress_status_->set_text(strings_.text(Str::Uninstalling));
    progress_timeline_->set_state(-1, false);
    waiting_for_elevation_.store(false);
    busy_ = true;
    host_.set_animating(true, 16);
    show_page(Page::Progress);

    worker_ = std::jthread([this, uninstall] {
        const HRESULT com = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        const InstallResult result =
            run_uninstall(uninstall, strings_, log_, [this](const ProgressUpdate& update) { post(update); });
        {
            const std::lock_guard<std::mutex> guard(result_mutex_);
            result_ = result;
        }
        if (SUCCEEDED(com)) {
            ::CoUninitialize();
        }
    });
}

void Wizard::post(ProgressUpdate update) {
    if (!alive_ || hwnd() == nullptr) {
        return;
    }
    auto* copy = new ProgressUpdate(std::move(update));
    if (::PostMessageW(hwnd(), WM_SETUP_PROGRESS, 0, reinterpret_cast<LPARAM>(copy)) == FALSE) {
        delete copy;
    }
}

void Wizard::on_progress(const ProgressUpdate& update) {
    switch (update.kind) {
        case ProgressKind::Phase: {
            if (update.phase >= 0) {
                progress_timeline_->set_state(update.phase, false);
            }
            progress_bar_->set_value(update.percent);
            progress_percent_->set_text(std::to_wstring(update.percent) + L"%");
            progress_status_->set_text(phase_label(update.phase));
            if (!update.detail.empty()) {
                append_log_line(update.detail);
            }
            if (update.percent >= 100) {
                progress_timeline_->set_state(5, false);
                progress_status_->set_text(strings_.text(Str::PhaseDone));
                progress_bar_->set_marquee(false);
            }
            break;
        }
        case ProgressKind::Percent:
            progress_bar_->set_value(update.percent);
            progress_percent_->set_text(std::to_wstring(update.percent) + L"%");
            break;
        case ProgressKind::Detail:
            append_log_line(update.detail);
            break;
        case ProgressKind::WaitingForElevation:
            waiting_for_elevation_.store(true);
            progress_bar_->set_marquee(true);
            progress_status_->set_text(strings_.text(Str::ProgressWaitingElevation));
            append_log_line(update.detail);
            break;
        case ProgressKind::Finished: {
            // 引擎的 Finished 是在 run_install 返回前发出的；结果对象稍后由工作线程写入。
            // 先 join 再读，否则会拿到默认值（成功的安装被当成 0，失败的也被当成 0）。
            if (worker_.joinable()) {
                worker_.join();
            }
            InstallResult result;
            {
                const std::lock_guard<std::mutex> guard(result_mutex_);
                result = result_;
            }
            finish_with(result);
            break;
        }
    }
}

std::wstring Wizard::phase_label(int phase) const {
    if (mode_ == Mode::Install) {
        switch (phase) {
            case 0:
                return strings_.text(Str::PhaseFiles);
            case 1:
                return strings_.text(Str::PhaseShortcuts);
            case 2:
                return strings_.text(Str::PhaseRecords);
            case 3:
                return strings_.text(Str::PhaseAssoc);
            case 4:
                return strings_.text(Str::PhasePath);
            default:
                return {};
        }
    }

    switch (phase) {
        case 0:
            return strings_.text(Str::PhaseRemoveShortcuts);
        case 1:
            return strings_.text(Str::PhaseRemoveRegistry);
        case 2:
            return strings_.text(Str::PhaseRemovePath);
        case 3:
            return strings_.text(Str::PhaseRemoveFiles);
        case 4:
            return strings_.text(Str::PhaseCleanup);
        default:
            return {};
    }
}

void Wizard::append_log_line(const std::wstring& line) {
    if (line.empty()) {
        return;
    }
    progress_lines_.push_back(line);
    if (progress_lines_.size() > 200) {
        progress_lines_.erase(progress_lines_.begin());
    }

    std::wstring text;
    for (const std::wstring& item : progress_lines_) {
        text += item;
        text += L"\r\n";
    }
    progress_log_->set_text(std::move(text));
}

void Wizard::finish_with(const InstallResult& result) {
    if (busy_ == false && page_ == Page::Finish) {
        return;
    }
    if (worker_.joinable()) {
        worker_.join();
    }
    busy_ = false;
    exit_code_ = result.exit_code;
    waiting_for_elevation_.store(false);
    progress_bar_->set_marquee(false);
    host_.set_animating(false);

    msixpkg::gui::StatusMark::Kind mark = msixpkg::gui::StatusMark::Kind::Info;
    std::wstring title;
    std::wstring body;

    if (mode_ == Mode::Install) {
        if (result.exit_code == kExitSuccess && !result.warnings) {
            mark = msixpkg::gui::StatusMark::Kind::Success;
            title = strings_.text(Str::FinishSuccessTitle);
            body = strings_.text(Str::FinishSuccessBody);
        } else if (result.exit_code == kExitSuccess) {
            mark = msixpkg::gui::StatusMark::Kind::Info;
            title = strings_.text(Str::FinishWarningTitle);
            body = strings_.text(Str::FinishWarningBody);
        } else if (result.exit_code == kExitPrecondition) {
            title = strings_.text(Str::FinishCancelledTitle);
            body = strings_.text(Str::FinishCancelledBody);
        } else if (result.exit_code == kExitNeedAdmin) {
            mark = msixpkg::gui::StatusMark::Kind::Failure;
            title = strings_.text(Str::FinishFailureTitle);
            body = strings_.text(Str::ElevationDeclined);
        } else if (result.exit_code == kExitBusy) {
            mark = msixpkg::gui::StatusMark::Kind::Failure;
            title = strings_.text(Str::FinishFailureTitle);
            body = strings_.text(Str::TargetBusy);
        } else {
            mark = msixpkg::gui::StatusMark::Kind::Failure;
            title = strings_.text(Str::FinishFailureTitle);
            body = result.error.empty() ? strings_.text(Str::UnknownFailure) : result.error;
        }
    } else {
        if (result.exit_code == kExitSuccess) {
            mark = msixpkg::gui::StatusMark::Kind::Success;
            title = strings_.text(Str::UninstallDoneTitle);
            body = strings_.text(Str::UninstallDoneBody);
            self_delete_pending_ = true;
        } else if (result.exit_code == kExitPrecondition) {
            title = strings_.text(Str::FinishCancelledTitle);
            body = strings_.text(Str::FinishCancelledBody);
        } else if (result.exit_code == kExitNeedAdmin) {
            mark = msixpkg::gui::StatusMark::Kind::Failure;
            title = strings_.text(Str::FinishFailureTitle);
            body = strings_.text(Str::ElevationDeclined);
        } else {
            mark = msixpkg::gui::StatusMark::Kind::Failure;
            title = strings_.text(Str::UninstallFailedTitle);
            body = result.error.empty() ? strings_.text(Str::UnknownFailure) : result.error;
        }
    }

    result_mark_->set_kind(mark);
    result_title_->set_text(title);
    result_body_->set_text(body);
    result_log_->set_text(log_.available() ? log_.path().wstring() : strings_.text(Str::LogUnavailable));
    run_after_->visible = mode_ == Mode::Install && result.exit_code == kExitSuccess;
    show_page(Page::Finish);
}

void Wizard::on_next() {
    switch (page_) {
        case Page::Welcome:
            show_page(Page::Options);
            break;
        case Page::Options:
            start_install();
            break;
        case Page::Confirm:
            start_uninstall();
            break;
        case Page::Finish:
            on_close();
            break;
        case Page::Progress:
            break;
    }
}

void Wizard::on_back() {
    if (page_ == Page::Options) {
        show_page(Page::Welcome);
    }
}

void Wizard::on_cancel() {
    if (busy_) {
        ::MessageBoxW(hwnd(), strings_.text(Str::BusyClose), strings_.text(Str::WindowTitleInstall),
                      MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (page_ == Page::Finish) {
        on_close();
        return;
    }
    exit_code_ = kExitPrecondition;
    host_.quit(exit_code_);
}

void Wizard::on_close() {
    if (busy_) {
        ::MessageBoxW(hwnd(), strings_.text(Str::BusyClose), strings_.text(Str::WindowTitleInstall),
                      MB_OK | MB_ICONINFORMATION);
        return;
    }

    if (mode_ == Mode::Uninstall && self_delete_pending_) {
        self_delete_pending_ = false;
        (void)schedule_self_delete(self_path(), result_.install_dir, log_);
    }

    if (mode_ == Mode::Install && !diagnostics_ && page_ == Page::Finish &&
        result_.exit_code == kExitSuccess && !result_.install_dir.empty() && run_after_->checked()) {
        launch_wizard();
    }

    host_.quit(exit_code_);
}

void Wizard::launch_wizard() {
    const std::filesystem::path directory =
        result_.install_dir.empty() ? options_.install_dir : result_.install_dir;
    const std::filesystem::path executable = directory / kGuiExeName;
    ::ShellExecuteW(hwnd(), L"open", executable.c_str(), nullptr, directory.c_str(), SW_SHOWNORMAL);
}

void Wizard::open_log() {
    if (!log_.available()) {
        ::MessageBoxW(hwnd(), strings_.text(Str::LogUnavailable), strings_.text(Str::OpenLog),
                      MB_OK | MB_ICONINFORMATION);
        return;
    }
    ::ShellExecuteW(hwnd(), L"open", log_.path().c_str(), nullptr, log_.directory().c_str(),
                    SW_SHOWNORMAL);
}

void Wizard::show_page(Page page) {
    page_ = page;
    page_welcome_->visible = page == Page::Welcome;
    page_options_->visible = page == Page::Options;
    page_progress_->visible = page == Page::Progress;
    page_finish_->visible = page == Page::Finish;
    page_confirm_->visible = page == Page::Confirm;
    update_chrome();
    host_.invalidate();
}

void Wizard::update_chrome() {
    switch (page_) {
        case Page::Welcome: {
            heading_->set_text(strings_.format(
                Str::WelcomeTitle, {{L"{version}", std::wstring(MSIXPKG_SETUP_VERSION)}}));
            subheading_->set_text(strings_.text(Str::WelcomeSubtitle));
            break;
        }
        case Page::Options:
            heading_->set_text(strings_.text(Str::OptionsTitle));
            subheading_->set_text(strings_.text(Str::OptionsSubtitle));
            break;
        case Page::Progress:
            heading_->set_text(strings_.text(mode_ == Mode::Install ? Str::ProgressTitleInstall
                                                                   : Str::ProgressTitleUninstall));
            subheading_->set_text(progress_status_->text);
            break;
        case Page::Finish:
            heading_->set_text(strings_.text(Str::ProductName));
            subheading_->set_text(MSIXPKG_SETUP_VERSION);
            break;
        case Page::Confirm:
            heading_->set_text(strings_.text(Str::ConfirmUninstallTitle));
            subheading_->set_text(strings_.text(Str::ProductName));
            break;
    }

    int step = 0;
    if (page_ == Page::Progress) {
        step = 1;
    } else if (page_ == Page::Finish) {
        step = 2;
    }
    steps_->set_active(step);

    nav_back_->visible = page_ == Page::Options;
    nav_back_->set_default(false);
    nav_next_->visible = page_ == Page::Welcome || page_ == Page::Options || page_ == Page::Confirm;
    nav_next_->set_default(page_ != Page::Progress && page_ != Page::Finish);
    nav_next_->text = strings_.text(page_ == Page::Options ? Str::Install
                                      : page_ == Page::Confirm ? Str::Uninstall
                                                                : Str::Next);
    nav_next_->invalidate();
    nav_next_->set_primary(page_ == Page::Options || page_ == Page::Confirm);
    nav_cancel_->text = strings_.text(page_ == Page::Finish ? Str::Close : Str::Cancel);
    nav_cancel_->invalidate();
    nav_cancel_->set_default(page_ == Page::Finish);
    nav_cancel_->enabled = page_ != Page::Progress;

    if (page_ == Page::Welcome) {
        host_.focus_widget(nav_next_);
    } else if (page_ == Page::Options) {
        host_.focus_widget(selected_mode_ == InstallMode::AllUsers
                               ? static_cast<wren::Widget*>(choice_all_)
                               : static_cast<wren::Widget*>(choice_current_));
    } else if (page_ == Page::Confirm) {
        host_.focus_widget(nav_next_);
    } else {
        host_.focus_widget(nav_cancel_);
    }
}

void Wizard::apply_diagnostics() {
    diagnostics_ = options_.ui_page != UiPage::None || !options_.ui_result.empty();
    if (!options_.ui_theme.empty()) {
        if (options_.ui_theme == L"light") {
            host_.set_appearance(wren::Appearance::Light);
        } else if (options_.ui_theme == L"dark") {
            host_.set_appearance(wren::Appearance::Dark);
        } else if (options_.ui_theme == L"hc") {
            host_.set_appearance(wren::Appearance::HighContrast);
        } else if (options_.ui_theme == L"auto") {
            host_.set_appearance(wren::Appearance::Auto);
        }
    }
    if (!options_.ui_material.empty()) {
        if (options_.ui_material == L"solid") {
            host_.set_material(wren::MaterialPreference::Solid);
        } else if (options_.ui_material == L"mica") {
            host_.set_material(wren::MaterialPreference::Mica);
        } else if (options_.ui_material == L"acrylic") {
            host_.set_material(wren::MaterialPreference::Acrylic);
        } else if (options_.ui_material == L"auto") {
            host_.set_material(wren::MaterialPreference::Auto);
        }
    }

    if (!options_.ui_result.empty()) {
        synthesize_result(options_.ui_result);
    } else if (options_.ui_page != UiPage::None) {
        switch (options_.ui_page) {
            case UiPage::Welcome:
                show_page(Page::Welcome);
                exit_code_ = kExitSuccess;
                break;
            case UiPage::Options:
                apply_mode(plan_.ok ? plan_.mode : InstallMode::CurrentUser, true);
                show_page(Page::Options);
                exit_code_ = kExitSuccess;
                break;
            case UiPage::Progress:
                synthesize_progress(options_.ui_percent >= 0 ? options_.ui_percent : 42);
                exit_code_ = kExitSuccess;
                break;
            case UiPage::Finish:
                synthesize_result(L"success");
                break;
            case UiPage::Confirm:
                show_page(Page::Confirm);
                exit_code_ = kExitSuccess;
                break;
            case UiPage::None:
                break;
        }
    } else if (!plan_.ok) {
        result_.exit_code = plan_.exit_code;
        finish_with(result_);
    } else {
        if (mode_ == Mode::Install) {
            apply_mode(plan_.mode, true);
            show_page(Page::Welcome);
        } else {
            show_page(Page::Confirm);
        }
    }

    if (options_.exit_after_ms > 0) {
        host_.schedule_once(options_.exit_after_ms, [this] {
            if (hwnd() != nullptr) {
                ::PostMessageW(hwnd(), WM_CLOSE, 0, 0);
            }
        });
    }

    // 自动化路径：诊断页 + /AUTO 让页面立刻执行主动作，用来覆盖「UI 线程真的调用
    // 编排线程并切到结果页」这一段（--ui-page 单独只截图，不跑安装）。
    if (options_.auto_mode) {
        if (mode_ == Mode::Install && page_ == Page::Options) {
            host_.schedule_once(300, [this] { start_install(); });
        } else if (mode_ == Mode::Uninstall && page_ == Page::Confirm) {
            host_.schedule_once(300, [this] { start_uninstall(); });
        }
    }
}

void Wizard::synthesize_result(std::wstring_view kind) {
    msixpkg::gui::StatusMark::Kind mark = msixpkg::gui::StatusMark::Kind::Success;
    std::wstring title = strings_.text(mode_ == Mode::Uninstall ? Str::UninstallDoneTitle
                                                                : Str::FinishSuccessTitle);
    std::wstring body = strings_.text(mode_ == Mode::Uninstall ? Str::UninstallDoneBody
                                                               : Str::FinishSuccessBody);
    exit_code_ = kExitSuccess;

    if (kind == L"failure") {
        mark = msixpkg::gui::StatusMark::Kind::Failure;
        title = strings_.text(mode_ == Mode::Uninstall ? Str::UninstallFailedTitle
                                                        : Str::FinishFailureTitle);
        body = strings_.text(Str::UnknownFailure);
        exit_code_ = kExitNeedAdmin;
    } else if (kind == L"cancelled") {
        mark = msixpkg::gui::StatusMark::Kind::Info;
        title = strings_.text(Str::FinishCancelledTitle);
        body = strings_.text(Str::FinishCancelledBody);
        exit_code_ = kExitPrecondition;
    } else if (kind == L"warning") {
        mark = msixpkg::gui::StatusMark::Kind::Info;
        title = strings_.text(Str::FinishWarningTitle);
        body = strings_.text(Str::FinishWarningBody);
        exit_code_ = kExitSuccess;
    }

    result_mark_->set_kind(mark);
    result_title_->set_text(title);
    result_body_->set_text(body);
    result_log_->set_text(log_.available() ? log_.path().wstring() : strings_.text(Str::LogUnavailable));
    run_after_->visible = mode_ == Mode::Install && exit_code_ == kExitSuccess;
    show_page(Page::Finish);
}

void Wizard::synthesize_progress(int percent) {
    const int clamped = std::clamp(percent, 0, 100);
    progress_bar_->set_marquee(false);
    progress_bar_->set_value(clamped);
    progress_percent_->set_text(std::to_wstring(clamped) + L"%");
    const int phase = clamped >= 100 ? 4 : std::min(4, clamped / 20);
    progress_timeline_->set_state(phase, false);
    progress_status_->set_text(phase_label(phase));
    progress_lines_ = {L"preparing the install folder", L"writing the program files",
                       L"creating shortcuts"};
    append_log_line(L"sample progress event");
    show_page(Page::Progress);
}

void Wizard::shutdown() {
    if (!alive_ && report_written_) {
        return;
    }
    alive_ = false;
    if (worker_.joinable()) {
        worker_.join();
    }
    write_report();
}

void Wizard::write_report() {
    if (report_written_ || options_.report_path.empty()) {
        return;
    }
    report_written_ = true;

    std::wstring text;
    text += L"setup=wren\n";
    text += L"mode=";
    text += mode_ == Mode::Install ? L"install\n" : L"uninstall\n";
    text += L"page=";
    text += page_name(page_);
    text += L"\nappearance=";
    text += appearance_name(host_.appearance());
    text += L"\nmaterial=";
    text += wren::material_name(host_.material_kind());
    text += L"\npresenter=";
    text += host_.presenter_name();
    text += L"\ndpi=";
    text += std::to_wstring(static_cast<int>(host_.dpi()));
    text += L"\nclient=";
    text += std::to_wstring(host_.client_width_px());
    text += L"x";
    text += std::to_wstring(host_.client_height_px());

    const wren::FrameStats stats = host_.frame_stats();
    text += L"\nframes=" + std::to_wstring(stats.frames);
    text += L"\nrefresh_hz=" + std::to_wstring(stats.refresh_hz);
    text += L"\nmsaa.requests=" + std::to_wstring(host_.accessibility_request_count());
    text += L"\nheading=\"" + one_line(heading_->text) + L"\"";
    text += L"\nsubheading=\"" + one_line(subheading_->text) + L"\"\n";

    const auto entries = host_.accessible_entries();
    text += L"childCount=" + std::to_wstring(entries.size()) + L"\n";
    size_t index = 0;
    for (const auto& entry : entries) {
        const wren::Widget* widget = entry.widget;
        if (widget == nullptr) {
            continue;
        }
        ++index;
        wchar_t state[32]{};
        ::swprintf(state, 32, L"0x%08X", widget->accessible_state());
        text += L"child[" + std::to_wstring(index) + L"] id=" + std::to_wstring(widget->control_id);
        text += L" role=";
        text += role_name(widget->accessible_role());
        text += L" row=" + std::to_wstring(entry.row);
        text += L" name=\"" + one_line(widget->accessible_name()) + L"\"";
        text += L" value=\"" + one_line(widget->accessible_value()) + L"\"";
        text += L" state=" + std::wstring(state) + L"\n";
    }

    const std::string utf8 = to_utf8(text);
    std::wstring error;
    const std::filesystem::path path(options_.report_path);
    std::error_code code;
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path(), code);
    }
    if (!write_file_bytes(path, utf8.data(), utf8.size(), error)) {
        log_.line(L"ui: could not write the report: " + error);
    }
}

}  // namespace

int run_setup_ui(HINSTANCE instance, const Options& options, const Strings& strings, Log& log) {
    Wizard wizard(instance, Mode::Install, options, strings, log);
    return wizard.run();
}

int run_uninstall_ui(HINSTANCE instance, const Options& options, const Strings& strings, Log& log) {
    Wizard wizard(instance, Mode::Uninstall, options, strings, log);
    return wizard.run();
}

}  // namespace msixpkg::setup
