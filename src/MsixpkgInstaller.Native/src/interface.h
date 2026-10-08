#pragma once

// 前端与编排之间的契约（docs/installer-design.md §4）。
//
// 编排（installer.*）对 UI **零依赖**：它只认识下面这三个接口。
// 三种前端共用同一套编排：
//   - gui     —— 向导（gui/），IProgressSink 只做 PostMessage，UI 线程永不阻塞
//   - console —— `--json` / `--log`（cli/）
//   - silent  —— `--quiet`，CI 用（本文件里的 SilentProgressSink + AutoApproveConsent）
//
// 这个头不依赖 Windows 头，也不依赖 WinRT：GUI、CLI、测试都能包含它。

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace msixpkg {

/// 安装 / 卸载的阶段。GUI 用它渲染阶段列表，CLI 用它打日志。
enum class Phase {
    Idle,
    Validate,      // 校验容器、钉扎、载荷哈希
    Stage,         // 解包到受保护暂存目录
    Consent,       // 等待用户同意（安全关键）
    Certificate,   // 导入 / 回收发布者证书（可能提权）
    Dependencies,  // 安装依赖（框架）包
    MainPackage,   // 安装主包
    Provision,     // --all-users 的 provisioning
    Reclaim,       // 卸载时的证书回收判定
    Rollback,      // 失败回滚
    Done,
    Failed,
};

/// 阶段的中文/英文展示名交给前端（.rc 字符串表），这里只给稳定的英文标识。
inline const char* phase_id(Phase phase) {
    switch (phase) {
        case Phase::Idle: return "idle";
        case Phase::Validate: return "validate";
        case Phase::Stage: return "stage";
        case Phase::Consent: return "consent";
        case Phase::Certificate: return "certificate";
        case Phase::Dependencies: return "dependencies";
        case Phase::MainPackage: return "main-package";
        case Phase::Provision: return "provision";
        case Phase::Reclaim: return "reclaim";
        case Phase::Rollback: return "rollback";
        case Phase::Done: return "done";
        case Phase::Failed: return "failed";
        default: return "unknown";
    }
}

struct ProgressEvent {
    Phase phase = Phase::Idle;
    uint32_t percent = 0;
    std::wstring detail;
};

/// 发布者摘要：同意页要靠它把「将要信任谁」讲清楚。
struct PublisherSummary {
    std::string subject;
    std::string thumbprint_sha1;    // 小写 hex，40 字符
    std::string thumbprint_sha256;  // 小写 hex，64 字符
    std::string not_before_utc;
    std::string not_after_utc;
    bool self_signed = false;
    /// 该指纹是否**已经**在本机受信任人员存储里（决定卸载时是否有权回收）。
    bool already_trusted = false;
    /// 是否命中调用方给出的钉扎白名单。
    bool pinned = false;
};

/// 包摘要：摘要页要靠它告诉用户「将要装什么」。
struct PackageSummary {
    std::string name;
    std::string publisher;      // DN
    std::string version;
    std::string architecture;
    uint64_t total_size = 0;    // 容器内所有条目解包后的大小
    std::vector<std::string> dependencies;  // "Name Version"
    std::string description;
};

struct ConsentRequest {
    PublisherSummary publisher;
    PackageSummary package;
    /// 同 identity 的包是否已经装过（决定是「安装」还是「更新」的措辞）。
    bool already_installed = false;
    /// 导入证书需要管理员：为 false 表示当前已提权，不必再弹 UAC。
    bool requires_elevation = false;
    /// 容器是否声明允许所有用户（决定 --all-users 复选框是否可用）。
    bool allow_all_users = false;
    /// 未命中钉扎白名单：前端必须走「默认拒绝 + 二次确认 + 记住发布者」这条路径。
    bool unpinned_publisher = false;
};

struct ConsentDecision {
    bool approved = false;
    /// 未命中钉扎时，用户勾选的「信任此发布者」——会写审计日志，并且**不**自动永久记住。
    bool remember_publisher = false;
    bool all_users = false;
};

/// 进度接收方。必须是非阻塞的（GUI 实现只做 PostMessage）。
struct IProgressSink {
    virtual ~IProgressSink() = default;
    virtual void OnPhase(Phase phase, std::wstring_view detail) = 0;
    virtual void OnProgress(uint32_t percent) = 0;
    /// 即将拉起提权助手（GUI 显示「正在等待管理员确认…」）。
    virtual void OnWaitingForElevation() = 0;
};

/// 同意提供方。编排线程同步等待用户点击；UI 由实现方自己负责线程切换。
struct IConsentProvider {
    virtual ~IConsentProvider() = default;
    virtual ConsentDecision Ask(const ConsentRequest& request) = 0;
};

struct ICancelSource {
    virtual ~ICancelSource() = default;
    virtual bool IsCancelled() const = 0;
};

// ---------- 无界面前端可直接复用的实现 ----------

/// 什么都不做的进度接收方。
class SilentProgressSink final : public IProgressSink {
public:
    void OnPhase(Phase, std::wstring_view) override {}
    void OnProgress(uint32_t) override {}
    void OnWaitingForElevation() override {}
};

/// 永远不取消。
class NeverCancel final : public ICancelSource {
public:
    bool IsCancelled() const override { return false; }
};

/// 自动同意 —— 只在 `--yes`（无人值守）时使用。
/// 注意：它**不会**把未命中钉扎的发布者当成已批准，那一档永远要人点头
/// （见 docs/security-model.md 红线 2）。
class AutoApproveConsent final : public IConsentProvider {
public:
    explicit AutoApproveConsent(bool all_users = false) : all_users_(all_users) {}

    ConsentDecision Ask(const ConsentRequest& request) override {
        ConsentDecision decision;
        decision.approved = !request.unpinned_publisher;
        decision.all_users = all_users_ && request.allow_all_users;
        return decision;
    }

private:
    bool all_users_ = false;
};

}  // namespace msixpkg
