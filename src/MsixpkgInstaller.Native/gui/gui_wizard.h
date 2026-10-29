#pragma once

// 老 Win32 标准控件版向导（legacy 回退，-DMSIXPKG_GUI_LEGACY=ON）。
//
// 默认前端是 Wren 版：见 gui_wizard_wren.cpp / gui_widgets.cpp。本文件只保留
// 「一页行为对不上就整份回退」时需要的窗口类、控件表与消息循环。
//
// 一条规则贯穿全文：**只有 UI 线程碰控件**。所有耗时工作都在 gui_worker.cpp 起的
// std::jthread 上，结果通过 PostMessage 回到这里。

#include <Windows.h>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "gui_ui.h"
#include "gui_worker.h"

namespace msixpkg::gui {

/// 注册好的窗口类名。单实例仲裁用 FindWindowW 找已经存在的那个窗口。
inline constexpr wchar_t kWindowClass[] = L"MsixpkgInstaller.Wizard.v1";

/// 逻辑（dp）尺寸：按 DPI 缩放后就是真实像素尺寸。720x480 是设计文档给的基准。
inline constexpr int kBaseWidth = 720;
inline constexpr int kBaseHeight = 480;

class Wizard {
public:
    Wizard(void* instance, std::shared_ptr<SharedState> shared, Strings strings);
    ~Wizard();

    Wizard(const Wizard&) = delete;
    Wizard& operator=(const Wizard&) = delete;

    /// 创建窗口与全部控件；失败返回 false。
    bool create();
    /// 显示窗口；`container` 非空时立刻开始收集摘要，`manage` 为真则直接进管理页。
    void show(const std::wstring& container, bool manage);

    /// 命令行驱动的一次卸载：`uninstall --family-name <PFN>`（ARP 的 UninstallString 就是它）。
    /// 两个参数给一个即可，install_id 优先。
    void show_uninstall(const std::wstring& family_name, const std::wstring& install_id);

    [[nodiscard]] HWND hwnd() const noexcept { return hwnd_; }
    [[nodiscard]] std::shared_ptr<SharedState> shared() const noexcept { return shared_; }
    [[nodiscard]] int exit_code() const noexcept { return exit_code_.load(); }

    /// WM_COPYDATA 带来的新容器：载入并跳到同意页。
    void adopt_container(const std::wstring& container);

private:
    [[nodiscard]] std::wstring result_body(const InstallView& view) const;

    // ---- 消息处理 ----
    static LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
    LRESULT handle(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);

    LRESULT on_create();
    LRESULT on_command(WORD id, WORD notification, HWND control);
    LRESULT on_notify(NMHDR* header);
    LRESULT on_copy_data(const COPYDATASTRUCT* data);
    LRESULT on_close();
    LRESULT on_ctl_color(UINT message, HDC dc, HWND control);
    void on_dpi_changed(int dpi, const RECT* suggested);

    // ---- 页面 ----
    void show_page(WizardPage page);
    void layout();
    void layout_consent();
    void move_control(int id, int x, int y, int width, int height);
    void apply_texts();
    void render_page();
    void render_summary();
    void render_consent();
    void update_consent_gate();
    void render_progress();
    void render_result();
    void render_manage();
    void build_phase_list();

    // ---- 动作 ----
    void begin_preview(const std::wstring& container);
    void begin_install(const ConsentAnswers& answers);
    void submit_consent(bool approved);
    void begin_manage_load();
    void begin_uninstall();
    /// 进度页 + 编排线程：管理页的「卸载」按钮与命令行卸载都走这里。
    void begin_uninstall_for(const std::string& install_id, const std::wstring& family_name);
    void cancel_install();
    void open_log_file();
    void open_log_directory();
    void launch_application();
    void copy_to_clipboard(const std::wstring& text) const;
    void open_settings_apps();
    void show_repair_plan();

    // ---- 控件小工具 ----
    HWND create_control(const ControlSpec& spec);
    void set_text(int id, std::wstring_view text);
    void set_visible(int id, bool visible);
    /// 让父窗口把 control 覆盖的那块区域重新清成透明（材质生效时擦掉旧字形）。
    void repaint_behind(HWND control);
    void enable(int id, bool enabled);
    [[nodiscard]] bool is_visible(int id) const;
    [[nodiscard]] bool is_checked(int id) const;
    [[nodiscard]] int selected_row() const;

    // ---- 主题 ----
    void apply_theme();
    /// 在窗口上屏后请求 DWM 的背景材质（Mica）；失败则整个退回纯色底色。
    void apply_backdrop();
    void release_brushes();
    void apply_fonts();
    void apply_list_colors();
    [[nodiscard]] int scale(int dp) const;
    [[nodiscard]] int point_to_pixels(int point) const;

    void* instance_ = nullptr;
    HWND hwnd_ = nullptr;
    HWND list_view_ = nullptr;               // 管理页的列表（需要专门的样式与通知）

    // 顺序有讲究，而且是两件事同时在起作用：
    //  - 构造：shared_ 必须先于 worker_ 初始化（worker_ 要拿它当参数）；
    //  - 析构：逆序进行，所以 worker_ 先于 shared_ 销毁 —— 正是我们要的，
    //    编排线程必须在共享状态消失之前彻底停下。
    std::shared_ptr<SharedState> shared_;
    ThreadSlot worker_;
    Strings strings_;

    WizardPage page_ = WizardPage::Summary;
    int dpi_ = 96;
    bool dark_ = false;
    /// DWM 的系统背景材质（Mica）是否真的接上了。为 true 时客户区用 alpha=0 清空，
    /// 让窗口背后由 DWM 画的材质透出来；任何一次不透明填充都会把它整片盖住。
    bool backdrop_ = false;
    bool installing_ = false;                // 进度页是否由「安装」进入（区别于卸载）
    bool dropped_in_ = false;                // 由拖放/第二实例换入的容器：跳过摘要页
    /// 本次安装是否已经为「即将提权」做过一次前台提醒（只做一次，别反复抢焦点）。
    bool elevation_foregrounded_ = false;
    // 默认 0：仅仅打开向导又关掉，不算失败（CLI 的 list / verify 同样是 0）。
    // 真正的结论由安装结果或容器校验结果覆盖。
    std::atomic<int> exit_code_{exit_code::kSuccess};

    HFONT font_title_ = nullptr;
    HFONT font_heading_ = nullptr;
    HFONT font_body_ = nullptr;
    HBRUSH brush_background_ = nullptr;
    HBRUSH brush_edit_ = nullptr;
    COLORREF color_background_ = CLR_INVALID;
    COLORREF color_text_ = CLR_INVALID;

    std::vector<HWND> controls_;
    std::vector<int> ids_;
};

}  // namespace msixpkg::gui
