#include "gui_worker.h"

// 编排线程：唯一接触引擎（Installer / StateStore / 部署栈 / validate）的编译单元。
//
// 三条不变量：
//  1. 这里的代码只跑在后台 std::jthread 上，绝不碰控件；结果先写进 SharedState（持锁），
//     再 PostMessage 通知 UI。
//  2. 跨线程传递的一切都是 std::shared_ptr，绝不让某个栈上对象在另一条线程上还活着。
//  3. 「等待用户同意」走请求-应答：挂上 PendingConsent → PostMessage → 等 atomic。
//     UI 线程永不阻塞，等待的始终是这条后台线程（interface.h 的 IConsentProvider 语义）。

#include <Windows.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "appx.h"
#include "certstore.h"
#include "container.h"
#include "deployment.h"
#include "installer.h"
#include "interface.h"
#include "internal.h"
#include "log.h"
#include "ops.h"
#include "state.h"
#include "verify.h"
#include "zip.h"
#include "msixpkg/errors.h"

namespace msixpkg::gui {
namespace {

// ---------- 小工具 ----------

std::wstring to_wide(std::string_view text) {
    return msixpkg::widen_utf8(text);
}

std::string to_narrow(std::wstring_view text) {
    return msixpkg::narrow_utf8(text);
}

void post(const WorkerContext& context, UINT message, WPARAM wparam = 0, LPARAM lparam = 0) {
    if (context.target == nullptr) {
        return;
    }

    const HWND hwnd = context.target->load();
    if (hwnd == nullptr || ::IsWindow(hwnd) == FALSE) {
        return;
    }

    ::PostMessageW(hwnd, message, wparam, lparam);
}

// ---------- 阶段翻译 ----------
PhaseView to_view(msixpkg::Phase phase) {
    switch (phase) {
        case msixpkg::Phase::Idle: return PhaseView::Idle;
        case msixpkg::Phase::Validate: return PhaseView::Validate;
        case msixpkg::Phase::Stage: return PhaseView::Stage;
        case msixpkg::Phase::Consent: return PhaseView::Consent;
        case msixpkg::Phase::Certificate: return PhaseView::Certificate;
        case msixpkg::Phase::Dependencies: return PhaseView::Dependencies;
        case msixpkg::Phase::MainPackage: return PhaseView::MainPackage;
        case msixpkg::Phase::Provision: return PhaseView::Provision;
        case msixpkg::Phase::Reclaim: return PhaseView::Reclaim;
        case msixpkg::Phase::Rollback: return PhaseView::Rollback;
        case msixpkg::Phase::Done: return PhaseView::Done;
        case msixpkg::Phase::Failed: return PhaseView::Failed;
    }

    return PhaseView::Idle;
}

// ---------- 进度接收方 ----------
// 只做「写共享状态 + PostMessage」：这里一旦阻塞（等锁很久、弹窗），整个安装就被拖住。
class GuiProgressSink final : public msixpkg::IProgressSink {
public:
    explicit GuiProgressSink(WorkerContext context) : context_(std::move(context)) {}

    void OnPhase(msixpkg::Phase phase, std::wstring_view detail) override {
        {
            const std::lock_guard<std::mutex> guard(context_.shared->mutex);
            context_.shared->phase = to_view(phase);
            if (!detail.empty()) {
                context_.shared->phase_log.push_back(std::wstring(detail));
            }
        }

        post(context_, kMsgProgress, 0, 0);
    }

    void OnProgress(uint32_t percent) override {
        {
            const std::lock_guard<std::mutex> guard(context_.shared->mutex);
            context_.shared->percent = percent;
        }

        post(context_, kMsgProgress, percent, 0);
    }

    void OnWaitingForElevation() override {
        {
            const std::lock_guard<std::mutex> guard(context_.shared->mutex);
            context_.shared->waiting_for_elevation = true;
        }

        post(context_, kMsgProgress, 0, 1);
    }

private:
    WorkerContext context_;
};

// ---------- 取消 ----------
class FlagCancel final : public msixpkg::ICancelSource {
public:
    explicit FlagCancel(std::shared_ptr<std::atomic<bool>> flag) : flag_(std::move(flag)) {}

    bool IsCancelled() const override { return flag_->load(); }

private:
    std::shared_ptr<std::atomic<bool>> flag_;
};

// ---------- 同意 ----------
class GuiConsent final : public msixpkg::IConsentProvider {
public:
    GuiConsent(WorkerContext context, std::shared_ptr<std::atomic<bool>> cancelled)
        : context_(std::move(context)), cancelled_(std::move(cancelled)) {}

    msixpkg::ConsentDecision Ask(const msixpkg::ConsentRequest& request) override {
        (void)request;  // 同意页展示的数据来自第一次 validate（见 build_preview）

        // 同意页已经在用户点「下一步」时把决定交过来了：直接消费，不再发一次
        // kMsgConsentAsk。否则窗口会把刚勾好的复选框清空、退回同意页，用户必须
        // 再点一次「下一步」。
        {
            const std::lock_guard<std::mutex> guard(context_.shared->mutex);
            if (context_.shared->preapproved_consent.has_value()) {
                const ConsentAnswers answers = *context_.shared->preapproved_consent;
                context_.shared->preapproved_consent.reset();

                msixpkg::ConsentDecision decision;
                decision.approved = answers.approved;
                decision.remember_publisher = answers.remember_publisher;
                decision.all_users = answers.all_users;
                return decision;
            }
        }

        auto pending = std::make_shared<PendingConsent>();
        {
            const std::lock_guard<std::mutex> guard(context_.shared->mutex);
            context_.shared->consent = pending;
        }

        post(context_, kMsgConsentAsk);

        // 轮询 20 ms：既让界面立刻响应，又不把一个纯等待的循环烧成忙等。
        // 取消或窗口消失时也必须能退出，否则 stop_worker 的 join 会永远卡住。
        while (pending->answered.load() == false) {
            if (cancelled_->load() || context_.target->load() == nullptr) {
                const std::lock_guard<std::mutex> guard(context_.shared->mutex);
                if (context_.shared->consent == pending) {
                    context_.shared->consent.reset();
                }

                return {};
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }

        msixpkg::ConsentDecision decision;
        decision.approved = pending->approved;
        decision.remember_publisher = pending->remember_publisher;
        decision.all_users = pending->all_users;

        // 问答用过了就摘下来：下一次同意必须重新走一遍 UI。
        const std::lock_guard<std::mutex> guard(context_.shared->mutex);
        if (context_.shared->consent == pending) {
            context_.shared->consent.reset();
        }

        return decision;
    }

private:
    WorkerContext context_;
    std::shared_ptr<std::atomic<bool>> cancelled_;
};

// ---------- 错误码 → 退出码 / 建议 ----------
int translate_error(const std::string& code) {
    if (code == msixpkg::codes::IoError) {
        return exit_code::kIoError;
    }

    if (code == msixpkg::deployment_codes::Cancelled) {
        return exit_code::kCancelled;
    }

    if (code == msixpkg::deployment_codes::AllUsersNotPermitted) {
        return exit_code::kPrecondition;
    }

    if (code.rfind("E_DEPLOY_", 0) == 0) {
        return exit_code::kDeploymentFailed;
    }

    return exit_code::kContainerInvalid;
}

int hint_for(const std::string& code) {
    if (code == msixpkg::codes::NotPinned) {
        return IDS_RESULT_HINT_NOT_PINNED;
    }

    if (code == msixpkg::deployment_codes::PublisherNotTrusted ||
        code == msixpkg::codes::CertificateExpired || code == msixpkg::codes::CertificateNotYetValid ||
        code == msixpkg::codes::CertificateMissing || code == msixpkg::codes::CertificateMalformed) {
        return IDS_RESULT_HINT_PUBLISHER_NOT_TRUSTED;
    }

    if (code == msixpkg::codes::SignatureInvalid || code == msixpkg::codes::SignatureMalformed ||
        code == msixpkg::codes::SignatureThumbprintMismatch ||
        code == msixpkg::codes::SignatureMissing ||
        code == msixpkg::deployment_codes::SignatureInvalid) {
        return IDS_RESULT_HINT_SIGNATURE;
    }

    if (code == msixpkg::deployment_codes::PackageInUse) {
        return IDS_RESULT_HINT_PACKAGE_IN_USE;
    }

    if (code == msixpkg::deployment_codes::HigherVersionInstalled) {
        return IDS_RESULT_HINT_HIGHER_VERSION;
    }

    if (code == msixpkg::deployment_codes::AccessDenied) {
        return IDS_RESULT_HINT_ACCESS_DENIED;
    }

    if (code == msixpkg::deployment_codes::DependencyMissing) {
        return IDS_RESULT_HINT_DEPENDENCY;
    }

    if (code == msixpkg::deployment_codes::PackageInvalid) {
        return IDS_RESULT_HINT_PACKAGE_INVALID;
    }

    if (code == msixpkg::deployment_codes::ArchitectureMismatch ||
        code == msixpkg::codes::ArchitectureMismatch) {
        return IDS_RESULT_HINT_ARCHITECTURE;
    }

    if (code == msixpkg::deployment_codes::AllUsersNotPermitted) {
        return IDS_RESULT_HINT_ALL_USERS;
    }

    if (code == msixpkg::codes::HashMismatch || code == msixpkg::codes::SizeMismatch ||
        code == msixpkg::codes::ContainerInvalid || code == msixpkg::codes::ContainerEmpty ||
        code == msixpkg::codes::ContainerTooLarge || code == msixpkg::codes::ManifestMissing ||
        code == msixpkg::codes::ManifestSchema || code == msixpkg::codes::ManifestNotFirst ||
        code.rfind("E_ENTRY_", 0) == 0 || code.rfind("E_APPX_", 0) == 0 ||
        code.rfind("E_PUBLISHER_", 0) == 0 || code.rfind("E_FORMAT_", 0) == 0) {
        return IDS_RESULT_HINT_CONTAINER;
    }

    if (code == msixpkg::codes::OsTooOld || code == msixpkg::codes::InstallerTooOld) {
        return IDS_RESULT_HINT_PACKAGE_INVALID;
    }

    if (code.rfind("E_DEPLOY_", 0) == 0) {
        return IDS_RESULT_HINT_GENERIC;
    }

    return IDS_RESULT_HINT_GENERIC;
}

// ---------- 摘要收集 ----------

/// 内层 AppxManifest 的 Application/@Id：AUMID 的另一半。
/// 读不到只是「启动应用」按钮要禁用，不该让摘要页失败。
std::string read_application_id(const msixpkg::Container& source) {    try {
        const msixpkg::zip::Entry* entry = source.archive().find(source.manifest().main_package.path);
        if (entry == nullptr) {
            return {};
        }

        const msixpkg::zip::Archive inner = source.open_nested(*entry);
        return msixpkg::read_appx_main_application_id(inner);
    } catch (const msixpkg::Error&) {
        return {};
    } catch (const std::exception&) {
        return {};
    }
}

/// 摘要页 / 同意页的数据来源：**一次** `Installer::plan()`。
///
/// 以前这里是「先跑一遍 dry-run 的 install，再自己 validate 一遍」——两遍都读同一个容器，
/// 而且同意页要的钉扎判定/有效期只能靠第二遍重算，稍不留神就会和编排里的判定漂移。
/// 现在 plan() 一次给全（校验结果 + 同意请求 + 动作清单），且**零副作用**：
/// 不建目录、不写日志、不碰证书存储。
PackageView build_preview(const std::wstring& container, const std::shared_ptr<SharedState>& shared,
                          const std::shared_ptr<std::atomic<bool>>& cancelled) {
    PackageView view;
    const std::filesystem::path path(container);

    const WorkerContext context{nullptr, shared};
    GuiProgressSink sink(context);
    GuiConsent consent(context, cancelled);
    FlagCancel cancel(cancelled);
    msixpkg::Installer installer(sink, consent, cancel);

    msixpkg::InstallOptions options;
    options.container = path;

    const msixpkg::InstallPlan plan = installer.plan(options);
    if (!plan.valid) {
        view.error_code = to_wide(plan.failure_code);
        view.error_message = to_wide(plan.failure_message);
        if (!plan.failure_detail.empty()) {
            view.error_message += L" [" + to_wide(plan.failure_detail) + L"]";
        }

        // 校验失败也带一个与 CLI 一致的退出码（2 校验失败 / 4 I/O）：
        // 「打开了一个坏文件然后关窗」因此不再返回「未预期错误」。
        view.exit_code = translate_error(plan.failure_code);
        return view;
    }

    for (const msixpkg::PlannedAction& action : plan.actions) {
        view.plan.push_back(to_wide(action.step) + L" — " + to_wide(action.detail));
    }

    // 同意请求是编排自己构造的那一份：界面显示的「正文」与安装时真正据以决策的内容
    // 因此天然一致（包括钉扎结论与证书有效期）。
    const msixpkg::ConsentRequest& request = plan.consent;
    const msixpkg::Manifest& manifest = plan.manifest;

    view.name = request.package.name;
    view.display_name = (manifest.has_display && !manifest.display.name.empty()) ? manifest.display.name
                                                                               : manifest.identity.name;
    view.version = request.package.version;
    view.architecture = request.package.architecture;
    view.publisher = request.package.publisher;
    view.description = request.package.description;
    view.total_size = request.package.total_size;
    view.dependencies = request.package.dependencies;
    view.already_installed = plan.already_installed;
    view.allow_all_users = request.allow_all_users;
    view.family_name = to_wide(plan.package_family_name);

    view.has_certificate = plan.has_publisher_certificate;
    view.subject = request.publisher.subject;
    view.thumbprint_sha1 = request.publisher.thumbprint_sha1;
    view.thumbprint_sha256 = request.publisher.thumbprint_sha256;
    view.not_before_utc = request.publisher.not_before_utc;
    view.not_after_utc = request.publisher.not_after_utc;
    view.already_trusted = request.publisher.already_trusted;
    view.pinned = request.publisher.pinned;
    view.unpinned_publisher = request.unpinned_publisher;

    view.application_id = read_application_id(msixpkg::Container::open(path));
    view.ok = true;
    return view;
}

// ---------- 结果 ----------

InstallView make_failure_view(const std::string& code, const std::string& message, const std::string& detail,
                              const std::wstring& log_path, bool elevation_declined) {
    InstallView view;
    view.present = true;
    view.exit_code = translate_error(code);
    view.status = view.exit_code == exit_code::kCancelled ? InstallStatus::Cancelled : InstallStatus::Failure;
    view.error_code = to_wide(code);
    view.error_message = to_wide(message);
    view.error_detail = to_wide(detail);
    view.log_path = log_path;
    view.elevation_declined = elevation_declined;
    if (view.status == InstallStatus::Failure) {
        view.hint_id = hint_for(code);
    }

    return view;
}

void publish(const WorkerContext& context, InstallView view) {
    {
        const std::lock_guard<std::mutex> guard(context.shared->mutex);
        context.shared->install = std::move(view);
    }

    post(context, kMsgInstallDone);
}

// ---------- 任务 ----------

void task_preview(const WorkerContext context, const std::wstring container,
                  std::shared_ptr<std::atomic<bool>> cancelled) {
    PackageView view;
    try {
        view = build_preview(container, context.shared, cancelled);
    } catch (const msixpkg::Error& error) {
        // 摘要收集本身也可能抛（例如文件根本不存在 → E_IO、路径不是容器 → E_CONTAINER_INVALID）。
        // 这里必须接住并把它变成「一个失败的 PackageView」：否则 ThreadSlot 的兜底会
        // 静默吞掉异常，界面就永远停在「还没有容器」上，而用户其实已经给过路径了。
        view.error_code = to_wide(error.code());
        view.error_message = to_wide(error.message());
        if (!error.detail().empty()) {
            view.error_message += L" [" + to_wide(error.detail()) + L"]";
        }

        view.exit_code = translate_error(error.code());
    } catch (const std::exception& error) {
        view.error_message = to_wide(error.what());
        view.exit_code = exit_code::kUnexpected;
    } catch (...) {
        view.error_message = L"a non-standard exception escaped";
        view.exit_code = exit_code::kUnexpected;
    }

    {
        const std::lock_guard<std::mutex> guard(context.shared->mutex);
        context.shared->container_path = container;
        context.shared->preview = view;
    }

    post(context, kMsgPreviewReady);
}

void task_install(const WorkerContext context, std::shared_ptr<std::atomic<bool>> cancelled) {
    std::wstring container;
    {
        const std::lock_guard<std::mutex> guard(context.shared->mutex);
        container = context.shared->container_path;
    }

    GuiProgressSink sink(context);
    GuiConsent consent(context, cancelled);
    FlagCancel cancel(cancelled);

    msixpkg::Installer installer(sink, consent, cancel);

    msixpkg::InstallOptions options;
    options.container = std::filesystem::path(container);

    // 钉扎白名单刻意留空：安全模型（docs/security-model.md §1）要求它由管理员通过
    // HKLM\SOFTWARE\MsixpkgInstaller\AllowedPublishers 预先部署，而那段策略读取**目前
    // 还没有实现**（CLI 侧也只有 --pin 一条入口）。留空的后果是「任何发布者都算未命中
    // 钉扎」—— 咨询同意页会因此要求用户显式勾选「信任此发布者」。方向是安全的
    // （绝不会替用户扩大信任面），但代价是命中白名单的发布者也享受不到免问待遇。
    InstallView result;
    try {
        const msixpkg::InstallOutcome outcome = installer.install(options);

        result.present = true;
        result.installed = outcome.installed;
        result.log_path = to_wide(outcome.log_path);
        result.family_name = to_wide(outcome.family_name);
        result.certificate_added = outcome.certificate_added_by_installer;
        result.certificate_existed_before = outcome.certificate_existed_before;
        result.elevation_declined = outcome.elevation_declined;

        if (outcome.installed) {
            result.status = InstallStatus::Success;
            result.exit_code = exit_code::kSuccess;
        } else {
            result.status = InstallStatus::Cancelled;
            result.exit_code = exit_code::kCancelled;
        }
    } catch (const msixpkg::Error& error) {
        result = make_failure_view(error.code(), error.message(), error.detail(), result.log_path, false);
    } catch (const std::exception& error) {
        result = make_failure_view("E_UNEXPECTED", error.what(), {}, result.log_path, false);
    } catch (...) {
        result = make_failure_view("E_UNEXPECTED", "a non-standard exception escaped", {}, result.log_path, false);
    }

    {
        const std::lock_guard<std::mutex> guard(context.shared->mutex);
        result.application_id = context.shared->preview.application_id;
    }

    publish(context, std::move(result));
}

void task_manage_load(const WorkerContext context) {
    ManageView view;
    try {
        const msixpkg::DataRoots roots = msixpkg::resolve_data_roots();
        const msixpkg::StateStore store(roots);
        view.data_root = roots.root.wstring();

        // verify_state 一次就把「状态库 vs 实际系统」算全：包在不在、证书在不在。
        // 它比多次调用 find_registered_packages 便宜，也是 MANAGE 列「状态」的依据。
        const msixpkg::VerifyReport report = msixpkg::verify_state(store, msixpkg::Log{});

        for (const msixpkg::InstallationStatus& status : report.installations) {
            const msixpkg::InstallationRecord& record = status.record;
            ManageEntry entry;
            entry.install_id = record.install_id;
            entry.name = to_wide(record.identity_name);
            entry.version = to_wide(record.identity_version);
            entry.certificate_sha1 = to_wide(record.certificate_sha1);
            entry.package_present = status.package_present;
            entry.certificate_present = status.certificate_present;

            if (record.result == "installed" && status.package_present) {
                entry.status = L"present";
            } else if (record.result == "partial") {
                entry.status = L"partial";
            } else if (record.result == "failed") {
                entry.status = L"failed";
            } else {
                entry.status = L"missing";
            }

            entry.detail = to_wide([&record, &status] {
                std::string text = "family=" + record.package_family_name + "\n";
                text += "full=" + record.package_full_name + "\n";
                text += "container=" + record.container_path + "\n";
                text += std::string("package=") + (status.package_present ? "present" : "MISSING") + "\n";
                text += std::string("certificate=") + (status.certificate_present ? "present" : "MISSING") +
                        (record.certificate_existed_before ? " (pre-existing, never removed)" : "") + "\n";
                text += "log=" + record.log_path;
                return text;
            }());

            view.entries.push_back(std::move(entry));
        }
    } catch (const msixpkg::Error& error) {
        view.notice = to_wide(error.to_string());
    } catch (const std::exception& error) {
        view.notice = to_wide(error.what());
    }

    view.loaded = true;
    {
        const std::lock_guard<std::mutex> guard(context.shared->mutex);
        context.shared->manage = std::move(view);
    }

    post(context, kMsgManageLoaded);
}

void task_uninstall(const WorkerContext context, const std::string install_id, const std::wstring family_name) {
    InstallView result;
    try {
        GuiProgressSink sink(context);
        // 卸载不问同意：要回收的证书是安装器自己写进去的，且判定由引擎按四条红线做
        // （security-model.md §3）。这里给它一个「已同意」的空实现。
        class AlwaysApprove final : public msixpkg::IConsentProvider {
        public:
            msixpkg::ConsentDecision Ask(const msixpkg::ConsentRequest&) override {
                msixpkg::ConsentDecision decision;
                decision.approved = true;
                return decision;
            }
        };

        AlwaysApprove consent;
        auto cancel_flag = std::make_shared<std::atomic<bool>>(false);
        FlagCancel cancel(cancel_flag);

        msixpkg::Installer installer(sink, consent, cancel);
        msixpkg::UninstallOptions options;
        options.install_id = install_id;
        options.family_name = to_narrow(family_name);

        const msixpkg::UninstallOutcome outcome = installer.uninstall(options);
        result.present = true;
        result.status = InstallStatus::Success;
        result.exit_code = exit_code::kSuccess;
        result.log_path = to_wide(outcome.log_path);
        result.family_name = to_wide(outcome.family_name);
        result.installed = outcome.removed;
    } catch (const msixpkg::Error& error) {
        result = make_failure_view(error.code(), error.message(), error.detail(), {}, false);
    } catch (const std::exception& error) {
        result = make_failure_view("E_UNEXPECTED", error.what(), {}, {}, false);
    }

    publish(context, std::move(result));
}

/// 修复建议：只算「结论 + 建议」，绝不改动系统（ops.h 的硬规则）。
/// 放在后台线程上，是因为它会去查包仓库与证书存储，可能慢到几百毫秒 ——
/// 那在 UI 线程上就是肉眼可见的卡顿。
void task_repair(const WorkerContext context) {
    std::wstring text;
    try {
        const msixpkg::StateStore store(msixpkg::resolve_data_roots());
        const msixpkg::VerifyReport report = msixpkg::repair_plan(store, msixpkg::Log{});

        if (report.issues.empty()) {
            text = L"\u0001";  // 占位：UI 用 IDS_MANAGE_NO_SUGGESTIONS 替换它
        } else {
            for (const msixpkg::VerifyIssue& issue : report.issues) {
                text += L"\u2022 ";
                text += to_wide(issue.code);
                text += L": ";
                text += to_wide(issue.detail);
                if (!issue.suggestion.empty()) {
                    text += L"\r\n    \u2192 ";
                    text += to_wide(issue.suggestion);
                }

                text += L"\r\n";
            }
        }
    } catch (const msixpkg::Error& error) {
        text = to_wide(error.to_string());
    } catch (const std::exception& error) {
        text = to_wide(error.what());
    }

    {
        const std::lock_guard<std::mutex> guard(context.shared->mutex);
        context.shared->manage.loaded = true;
        context.shared->manage.repair_text = std::move(text);
    }

    post(context, kMsgRepairReady);
}

}  // namespace

// ---------- 线程槽位 ----------
// 注意：ThreadSlot 是 msixpkg::gui 的成员类型，它的成员函数必须定义在
// msixpkg::gui 里（不能在匿名命名空间里）—— 这里的 } 就是为此而关的。

ThreadSlot::~ThreadSlot() {
    stop();
}

void ThreadSlot::run(const WorkerContext& context, std::function<void(WorkerContext)> task) {
    shared_->cancel.store(false);
    shared_->consent.reset();
    busy_.store(true);

    // 线程自己持有 context（含 shared_ptr<SharedState>）与任务对象，所以它不需要
    // 槽位替它保命，detach 是安全的。
    std::thread([context, task = std::move(task), this] {
        try {
            task(context);
        } catch (...) {
            // 编排线程绝不能让异常逃逸（那会直接 terminate 整个进程）。
            // 引擎已经把可预期的失败翻译成 msixpkg::Error，这里是最后一道兜底。
        }

        // 只碰两个原子量：槽位可能已经进入析构等待，但那要等这两个写完之后才放行。
        generation_.fetch_add(1);
        busy_.store(false);
    }).detach();
}

void ThreadSlot::wait_idle() {
    if (!busy_.load()) {
        return;
    }

    // 任务函数一定会返回（它每次服务同意请求前都检查 cancel），所以这里不会无限等待。
    // 用轮询而不是条件变量，是为了完全不依赖任务侧的配合。
    const int generation = generation_.load();
    while (generation_.load() == generation) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

void ThreadSlot::stop() {
    if (shared_ == nullptr) {
        return;
    }

    // 先让编排线程知道「没人再需要这个答案了」。
    shared_->cancel.store(true);
    {
        const std::lock_guard<std::mutex> guard(shared_->mutex);
        if (shared_->consent != nullptr) {
            // 唤醒可能正在等用户点击的 Ask：否则它会一直等到窗口销毁。
            shared_->consent->answered.store(true);
        }
    }

    wait_idle();
}

// ---------- 对外：启动任务 ----------
//
// 每个 start_* 都做同三件事：停掉上一个任务、记下窗口句柄（原子量：窗口随时可能
// 消失）、把任务放进线程槽位。任务对象的生命周期由线程自己持有，这里不留引用。

void start_preview(ThreadSlot& slot, HWND hwnd, const std::wstring& container) {
    slot.stop();
    slot.shared()->target.store(hwnd);

    const WorkerContext context{&slot.shared()->target, slot.shared()};
    const std::shared_ptr<std::atomic<bool>> cancelled(slot.shared(), &slot.shared()->cancel);
    slot.run(context, [container, cancelled](WorkerContext running) {
        task_preview(running, container, cancelled);
    });
}

void start_install(ThreadSlot& slot, HWND hwnd) {
    slot.stop();
    slot.shared()->target.store(hwnd);

    const WorkerContext context{&slot.shared()->target, slot.shared()};
    const std::shared_ptr<std::atomic<bool>> cancelled(slot.shared(), &slot.shared()->cancel);
    slot.run(context, [cancelled](WorkerContext running) { task_install(running, cancelled); });
}

void start_manage_load(ThreadSlot& slot, HWND hwnd) {
    slot.stop();
    slot.shared()->target.store(hwnd);

    const WorkerContext context{&slot.shared()->target, slot.shared()};
    slot.run(context, [](WorkerContext running) { task_manage_load(running); });
}

void start_repair(ThreadSlot& slot, HWND hwnd) {
    slot.stop();
    slot.shared()->target.store(hwnd);

    const WorkerContext context{&slot.shared()->target, slot.shared()};
    slot.run(context, [](WorkerContext running) { task_repair(running); });
}

void start_uninstall(ThreadSlot& slot, HWND hwnd, const std::string& install_id,
                     const std::wstring& family_name) {
    slot.stop();
    slot.shared()->target.store(hwnd);

    const WorkerContext context{&slot.shared()->target, slot.shared()};
    slot.run(context, [install_id, family_name](WorkerContext running) {
        task_uninstall(running, install_id, family_name);
    });
}

void stop_worker(ThreadSlot& slot) {
    slot.stop();
}

}  // namespace msixpkg::gui
