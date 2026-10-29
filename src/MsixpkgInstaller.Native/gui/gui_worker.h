#pragma once

// 后台编排线程与前端之间的契约（gui_worker.cpp 实现）。
//
// 这里刻意**不包含任何 UI 框架头**：老 Win32 控件版和新 Wren 版向导共用同一份
// 「投任务 / 收消息」接口，引擎与线程模型不会因为换 UI 而改变。

#include <Windows.h>

#include <atomic>
#include <functional>
#include <memory>
#include <string>

#include "gui_ui.h"

namespace msixpkg::gui {

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

// ---------- 后台任务 ----------
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

}  // namespace msixpkg::gui
