#pragma once

// 向导的主窗口对象（内部头：只有 gui_*.cpp 包含它）。
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

namespace msixpkg::gui {

/// 注册好的窗口类名。单实例仲裁用 FindWindowW 找已经存在的那个窗口。
inline constexpr wchar_t kWindowClass[] = L"MsixpkgInstaller.Wizard.v1";
/// 单实例互斥体名（Global\ 前缀：跨会话也要挡住，否则两个用户同时往证书存储里写）。
inline constexpr wchar_t kSingleInstanceMutex[] = L"Global\\MsixpkgInstaller.Single";
/// WM_COPYDATA 的标识，用于把新容器路径交给已有实例。
inline constexpr ULONG_PTR kCopyDataMagic = 0x4D535850ull;  // 'MSXP'
/// 已有实例不接受新容器（正在装东西）时的应答码。
inline constexpr ULONG_PTR kCopyDataBusy = 0x42555359ull;   // 'BUSY'

/// 逻辑（dp）尺寸：按 DPI 缩放后就是真实像素尺寸。720x480 是设计文档给的基准。
inline constexpr int kBaseWidth = 720;
inline constexpr int kBaseHeight = 480;

// ---------- 编排线程槽位 ----------
//
// 一个槽位同一时刻只跑一个任务。
//
// 先说清楚线程归属，因为它决定了后面所有代码的形状：任务函数**带着**它需要的全部
// 东西（尤其是 WorkerContext 里的 shared_ptr<SharedState>），所以线程自己就是安全的，
// 不需要槽位替它保命。槽位只负责两件事：
//   1. run() 之前先把上一个任务停干净（同一时刻只有一条线程在碰证书存储 / 部署栈）；
//   2. stop() 等到「确实没有线程在跑」才返回。
//
// 为什么线程是 detach 的、而不是成员 std::jthread：
// 编排线程会长时间阻塞在「等用户点击同意」或「等 UAC」上。成员线程对象意味着析构
// 必须 join，一旦某条唤醒路径漏了，关窗就卡死；而且 UI 线程不该等一个用户没操作的
// 对话框。detach + 「重置才停、停完才返回」把这两件事解耦：窗口关掉时线程会在
// 下一个检查点自己退出（Sink 的 PostMessage 也会因为窗口没了而变成空操作）。
class ThreadSlot {
public:
    explicit ThreadSlot(std::shared_ptr<SharedState> shared) : shared_(std::move(shared)) {}
    ~ThreadSlot();

    ThreadSlot(const ThreadSlot&) = delete;
    ThreadSlot& operator=(const ThreadSlot&) = delete;

    [[nodiscard]] const std::shared_ptr<SharedState>& shared() const noexcept { return shared_; }
    /// 当前是否有任务在跑（只有诊断用：渲染时要区分「已完成」与「还没开始」）。
    [[nodiscard]] bool busy() const noexcept { return busy_.load(); }

    /// 在**新线程**上执行 task(context)。返回前不会阻塞；调用方负责在那之前
    /// 用 stop()/wait_idle() 确认上一个任务已经退出。
    void run(const WorkerContext& context, std::function<void(WorkerContext)> task);
    /// 请求取消（唤醒等待中的同意问答）并等到没有任务在跑。可以安全地多次调用。
    void stop();
    /// 只等到没有任务在跑，不请求取消。
    void wait_idle();
    /// 任务收尾时由**任务自己**调用（见 gui_worker.cpp 的 start_* 包装）。
    void mark_idle() noexcept { busy_.store(false); }

private:
    std::shared_ptr<SharedState> shared_;
    std::atomic<bool> busy_{false};
    std::atomic<int> generation_{0};
};

// ---------- 后台任务（gui_worker.cpp 实现） ----------
//
// 这几个函数都**立即返回**：它们只是把任务放进线程槽位。编排线程绝不回调 UI，
// 只往 SharedState 里写数据 + PostMessage。
void start_preview(ThreadSlot& slot, HWND hwnd, const std::wstring& container);
void start_install(ThreadSlot& slot, HWND hwnd);
void start_manage_load(ThreadSlot& slot, HWND hwnd);
void start_repair(ThreadSlot& slot, HWND hwnd);
void start_uninstall(ThreadSlot& slot, HWND hwnd, const std::string& install_id,
                     const std::wstring& family_name);

/// 结束编排线程：先请求取消（并唤醒可能正在等用户点击的 Ask），再等它退出。
void stop_worker(ThreadSlot& slot);

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
    void begin_install();
    void submit_consent(bool approved);
    void begin_manage_load();
    void begin_uninstall();
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
    void enable(int id, bool enabled);
    [[nodiscard]] bool is_visible(int id) const;
    [[nodiscard]] bool is_checked(int id) const;
    [[nodiscard]] int selected_row() const;

    // ---- 主题 ----
    void apply_theme();
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
    bool installing_ = false;                // 进度页是否由「安装」进入（区别于卸载）
    bool dropped_in_ = false;                // 由拖放/第二实例换入的容器：跳过摘要页
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
