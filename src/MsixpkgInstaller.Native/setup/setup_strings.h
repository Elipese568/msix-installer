#pragma once

// setup / Uninstall.exe 的本地化文案（zh-CN / en-US）。
//
// 不放进 .rc：这些字符串同时被引擎（日志里的快捷方式名字、ARP 名字）与 Wren 页面使用，
// 纯 C++ 表可以在单测里直接断言「两种语言都非空」，也避免 rc 的双份 LANGUAGE 块。

#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>

namespace msixpkg::setup {

enum class Str : size_t {
    WindowTitleInstall,
    WindowTitleUninstall,
    ProductName,
    WelcomeTitle,
    WelcomeSubtitle,
    WelcomeBody,
    WelcomeList,
    OptionsTitle,
    OptionsSubtitle,
    ScopeAllUsers,
    ScopeAllUsersDetail,
    ScopeCurrentUser,
    ScopeCurrentUserDetail,
    InstallDirLabel,
    InstallDirHint,
    Browse,
    ComponentsLabel,
    AssocCheck,
    AssocCheckDetail,
    PathCheck,
    PathCheckDetail,
    DesktopCheck,
    DesktopCheckDetail,
    AdminNote,
    ExistingMachineNote,
    ExistingUserNote,
    ExistingInstallNote,
    Install,
    Back,
    Next,
    Cancel,
    Close,
    ProgressTitleInstall,
    ProgressTitleUninstall,
    ProgressWaitingElevation,
    PhaseFiles,
    PhaseShortcuts,
    PhaseRecords,
    PhaseAssoc,
    PhasePath,
    PhaseDone,
    PhaseRemoveFiles,
    PhaseRemoveShortcuts,
    PhaseRemoveRegistry,
    PhaseRemovePath,
    PhaseCleanup,
    FinishSuccessTitle,
    FinishSuccessBody,
    FinishWarningTitle,
    FinishWarningBody,
    FinishFailureTitle,
    FinishCancelledTitle,
    FinishCancelledBody,
    RunAfter,
    OpenLog,
    LogUnavailable,
    ConfirmUninstallTitle,
    ConfirmUninstallBody,
    Uninstall,
    Uninstalling,
    UninstallDoneTitle,
    UninstallDoneBody,
    UninstallFailedTitle,
    StateKeptNote,
    AdminRequired,
    ElevationDeclined,
    TargetBusy,
    PreconditionFailed,
    ShortcutMain,
    ShortcutMainDescription,
    ShortcutManage,
    ShortcutManageDescription,
    ShortcutUninstall,
    ShortcutUninstallDescription,
    AssocTypeName,
    CapabilitiesDescription,
    ArpComments,
    ArpPublisher,
    StepOptions,
    StepProgress,
    StepFinish,
    StepConfirm,
    BusyClose,
    HelpTitle,
    HelpBody,
    UnknownFailure,
};

class Strings {
public:
    Strings();

    bool chinese() const { return chinese_; }
    const wchar_t* text(Str id) const;

    std::wstring format(
        Str id, std::initializer_list<std::pair<std::wstring_view, std::wstring>> replacements) const;

    static size_t count();

private:
    bool chinese_ = false;
};

}  // namespace msixpkg::setup
