#include "gui_ui.h"

#include <Windows.h>
#include <commctrl.h>
#include <shellapi.h>

#include <algorithm>
#include <cwchar>
#include <iterator>

namespace msixpkg::gui {
namespace {

// 全部逻辑坐标以设计文档给的 720x480 dp 为基准；实际像素 = dp * dpi / 96
// （Wizard::scale）。每一行之间至少留 8dp，因此 150% / 200% 缩放时控件与文字
// 等比放大、不会被挤到控件外面。
//
// 同一页的控件按 Tab 顺序列出：创建顺序 == Tab 顺序，所以这个文件也是无障碍
// 「Tab 顺序完整」这一条的单一事实来源。

// ---------- 外壳（每一页共用） ----------
const ControlSpec kShell[] = {
    {IDC_HEADING, L"STATIC", PageLayout::Shell, SS_LEFT | SS_ENDELLIPSIS, 0, 18, 14, 684, 26},
    {IDC_SUBHEADING, L"STATIC", PageLayout::Shell, SS_LEFT | SS_ENDELLIPSIS, 0, 18, 40, 684, 18},
    // 导航条：同一组控件在不同页上换文案与可见性，避免重复的控件 ID 与重复的 Tab 站。
    {IDC_NAV_NEXT, L"BUTTON", PageLayout::Shell, WS_TABSTOP | BS_DEFPUSHBUTTON, 0, 534, 436, 84, 26},
    {IDC_NAV_BACK, L"BUTTON", PageLayout::Shell, WS_TABSTOP | BS_PUSHBUTTON, 0, 444, 436, 84, 26},
    {IDC_NAV_CANCEL, L"BUTTON", PageLayout::Shell, WS_TABSTOP | BS_PUSHBUTTON, 0, 624, 436, 78, 26},
};

// ---------- 摘要页 ----------
// 左列是标签（右对齐更易读），右列是值。
const ControlSpec kSummary[] = {
    {IDC_SUMMARY_LABEL_NAME, L"STATIC", PageLayout::Summary, SS_RIGHT, 0, 18, 74, 110, 17},
    {IDC_SUMMARY_NAME, L"STATIC", PageLayout::Summary, SS_LEFT | SS_ENDELLIPSIS, 0, 134, 74, 568, 17},
    {IDC_SUMMARY_LABEL_VERSION, L"STATIC", PageLayout::Summary, SS_RIGHT, 0, 18, 100, 110, 17},
    {IDC_SUMMARY_VERSION, L"STATIC", PageLayout::Summary, SS_LEFT | SS_ENDELLIPSIS, 0, 134, 100, 568, 17},
    {IDC_SUMMARY_LABEL_ARCH, L"STATIC", PageLayout::Summary, SS_RIGHT, 0, 18, 126, 110, 17},
    {IDC_SUMMARY_ARCH, L"STATIC", PageLayout::Summary, SS_LEFT | SS_ENDELLIPSIS, 0, 134, 126, 568, 17},
    {IDC_SUMMARY_LABEL_PUBLISHER, L"STATIC", PageLayout::Summary, SS_RIGHT, 0, 18, 152, 110, 17},
    {IDC_SUMMARY_PUBLISHER, L"STATIC", PageLayout::Summary, SS_LEFT | SS_ENDELLIPSIS, 0, 134, 152, 568, 17},
    {IDC_SUMMARY_LABEL_SIZE, L"STATIC", PageLayout::Summary, SS_RIGHT, 0, 18, 178, 110, 17},
    {IDC_SUMMARY_SIZE, L"STATIC", PageLayout::Summary, SS_LEFT | SS_ENDELLIPSIS, 0, 134, 178, 568, 17},
    {IDC_SUMMARY_LABEL_DEPS, L"STATIC", PageLayout::Summary, SS_RIGHT, 0, 18, 204, 110, 17},
    {IDC_SUMMARY_DEPS, L"STATIC", PageLayout::Summary, SS_LEFT | SS_ENDELLIPSIS, 0, 134, 204, 568, 17},
    {IDC_SUMMARY_LABEL_DESCRIPTION, L"STATIC", PageLayout::Summary, SS_RIGHT, 0, 18, 230, 110, 17},
    {IDC_SUMMARY_DESCRIPTION, L"STATIC", PageLayout::Summary, SS_LEFT | SS_ENDELLIPSIS, 0, 134, 230, 568, 17},
    // 更新提示 / 载入失败提示：只在有内容时可见。两行高度足以放下「错误码 + 说明」，
    // 单行的「此包已安装」只占第一行。
    {IDC_SUMMARY_NOTE, L"STATIC", PageLayout::Summary, SS_LEFT, 0, 18, 266, 684, 40},
};

// ---------- 同意页（安全关键） ----------
// 证书详情放在只读多行编辑框里：标准控件天然支持 Tab 聚焦、Ctrl+A/C 复制与屏幕阅读器
// 的文本读取，比一堆静态文本更满足 §8 的无障碍要求，也让指纹「可复制」。
//
// 注意：这里给出的是**初始**坐标。认可 / 信任发布者 / 未钉扎警告 / --all-users 的位置
// 会随可见性动态回流（Wizard::layout_consent）：信任框与警告隐藏时，用户选项紧跟
// 「我了解并同意」，不会在中间留一大块空白。
const ControlSpec kConsent[] = {
    {IDC_CONSENT_DETAILS, L"EDIT", PageLayout::Consent,
     WS_TABSTOP | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL | WS_BORDER, 0, 18, 74, 684, 152},
    {IDC_CONSENT_WARNING, L"STATIC", PageLayout::Consent, SS_LEFT, 0, 18, 236, 684, 34},
    {IDC_CONSENT_ACCEPT, L"BUTTON", PageLayout::Consent, WS_TABSTOP | BS_AUTOCHECKBOX | BS_MULTILINE, 0, 18,
     276, 684, 30},
    // 「信任此发布者」只在未命中钉扎时显示；可见性由 render_consent 决定。
    {IDC_CONSENT_TRUST_PUBLISHER, L"BUTTON", PageLayout::Consent,
     WS_TABSTOP | BS_AUTOCHECKBOX | BS_MULTILINE, 0, 18, 310, 684, 30},
    {IDC_CONSENT_UNPINNED_WARNING, L"STATIC", PageLayout::Consent, SS_LEFT, 0, 18, 342, 684, 30},
    // --all-users 复选框只在清单声明 allowAllUsers 时可用（且要显式勾选）。
    {IDC_CONSENT_ALL_USERS, L"BUTTON", PageLayout::Consent, WS_TABSTOP | BS_AUTOCHECKBOX, 0, 18, 378, 684, 20},
};

// ---------- 进度页 ----------
const ControlSpec kProgress[] = {
    // 阶段列表：只读多行编辑框（可聚焦、可复制），用于展示阶段表与事件流。
    {IDC_PROGRESS_PHASES, L"EDIT", PageLayout::Progress,
     WS_TABSTOP | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL | WS_BORDER, 0, 18, 78, 684, 200},
    {IDC_PROGRESS_BAR, PROGRESS_CLASSW, PageLayout::Progress, WS_TABSTOP | PBS_SMOOTH, 0, 18, 288, 568, 20},
    {IDC_PROGRESS_PERCENT, L"STATIC", PageLayout::Progress, SS_RIGHT | SS_ENDELLIPSIS, 0, 594, 288, 108, 20},
    {IDC_PROGRESS_STATUS, L"STATIC", PageLayout::Progress, SS_LEFT | SS_ENDELLIPSIS, 0, 18, 318, 684, 17},
    // 取消按钮只在进度页出现：它替代了导航条上的「取消」（两者文案相同、动作相同，
    // 但这里的语义是「取消正在进行的操作」）。
    {IDC_PROGRESS_CANCEL, L"BUTTON", PageLayout::Progress, WS_TABSTOP | BS_PUSHBUTTON, 0, 18, 384, 140, 26},
};

// ---------- 结果页 ----------
const ControlSpec kResult[] = {
    {IDC_RESULT_BODY, L"STATIC", PageLayout::Result, SS_LEFT, 0, 18, 74, 684, 120},
    {IDC_RESULT_LOG, L"STATIC", PageLayout::Result, SS_LEFT | SS_ENDELLIPSIS, 0, 18, 202, 684, 34},
    {IDC_RESULT_LAUNCH, L"BUTTON", PageLayout::Result, WS_TABSTOP | BS_PUSHBUTTON, 0, 18, 388, 160, 26},
    {IDC_RESULT_OPEN_LOG, L"BUTTON", PageLayout::Result, WS_TABSTOP | BS_PUSHBUTTON, 0, 186, 388, 140, 26},
    {IDC_RESULT_COPY_DETAILS, L"BUTTON", PageLayout::Result, WS_TABSTOP | BS_PUSHBUTTON, 0, 334, 388, 140, 26},
};

// ---------- 管理页 ----------
const ControlSpec kManage[] = {
    {IDC_MANAGE_LIST, WC_LISTVIEWW, PageLayout::Manage,
     WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS, WS_EX_CLIENTEDGE, 18, 78, 684, 250},
    {IDC_MANAGE_DETAIL, L"EDIT", PageLayout::Manage,
     WS_TABSTOP | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL | WS_BORDER, 0, 18, 334, 684, 42},
    {IDC_MANAGE_UNINSTALL, L"BUTTON", PageLayout::Manage, WS_TABSTOP | BS_PUSHBUTTON, 0, 18, 388, 96, 26},
    {IDC_MANAGE_REPAIR, L"BUTTON", PageLayout::Manage, WS_TABSTOP | BS_PUSHBUTTON, 0, 122, 388, 116, 26},
    {IDC_MANAGE_OPEN_LOGS, L"BUTTON", PageLayout::Manage, WS_TABSTOP | BS_PUSHBUTTON, 0, 246, 388, 130, 26},
    {IDC_MANAGE_OPEN_SETTINGS, L"BUTTON", PageLayout::Manage, WS_TABSTOP | BS_PUSHBUTTON, 0, 384, 388, 190, 26},
};

std::vector<ControlSpec> build_layout() {
    std::vector<ControlSpec> all;
    const auto append = [&all](const ControlSpec* specs, size_t count) {
        all.insert(all.end(), specs, specs + count);
    };

    append(kShell, std::size(kShell));
    append(kSummary, std::size(kSummary));
    append(kConsent, std::size(kConsent));
    append(kProgress, std::size(kProgress));
    append(kResult, std::size(kResult));
    append(kManage, std::size(kManage));
    return all;
}

}  // namespace

const std::vector<ControlSpec>& control_layout() {
    static const std::vector<ControlSpec> kLayout = build_layout();
    return kLayout;
}

PageLayout layout_group(WizardPage page) {
    switch (page) {
        case WizardPage::Summary: return PageLayout::Summary;
        case WizardPage::Consent: return PageLayout::Consent;
        case WizardPage::Progress: return PageLayout::Progress;
        case WizardPage::Result: return PageLayout::Result;
        case WizardPage::Manage: return PageLayout::Manage;
    }

    return PageLayout::Summary;
}

namespace {

/// 只有命令行前端（msixpkg-native.exe）实现的子命令。
/// 向导拿到它们必须**立刻失败**：当成容器路径 → 单实例交接 → 可能死锁。
bool is_cli_only_subcommand(std::wstring_view token) {
    constexpr std::wstring_view kCommands[] = {
        L"list", L"verify", L"repair", L"clean", L"cert-add", L"cert-remove",
    };

    return std::find(std::begin(kCommands), std::end(kCommands), token) != std::end(kCommands);
}

}  // namespace

CommandLine parse_command_line(const std::wstring& command_line) {
    CommandLine parsed;

    // wWinMain 的 lpCmdLine **不含**程序名（与 main/WinMain 的 argv 不同），而
    // CommandLineToArgvW 要求命令行以程序名开头 —— 直接把 lpCmdLine 喂给它，
    // 唯一的参数会被当成 argv[0] 丢掉（实测过：容器路径就"消失"在这里）。
    // 补一个占位程序名是最省事且不会误判引号/空格的修正。
    const std::wstring normalized = L"msixpkg-installer.exe " + command_line;

    int argc = 0;
    LPWSTR* argv = ::CommandLineToArgvW(normalized.c_str(), &argc);
    if (argv == nullptr) {
        return parsed;
    }

    for (int index = 1; index < argc; ++index) {
        const std::wstring_view token(argv[index]);
        if (token.empty()) {
            continue;
        }

        if (token == L"--manage") {
            parsed.manage = true;
            continue;
        }

        // ---- 诊断参数：只在 GUI 前端生效，解析失败就忽略（不改变安装语义） ----
        if (token == L"--ui-page" || token == L"--ui-report" || token == L"--exit-after" ||
            token == L"--ui-result" || token == L"--ui-theme" || token == L"--ui-material" ||
            token == L"--ui-progress") {
            const bool has_value = index + 1 < argc;
            const std::wstring value = has_value ? std::wstring(argv[index + 1]) : std::wstring();
            if (has_value) {
                ++index;
            }

            if (token == L"--ui-page") {
                parsed.ui_page = value;
            } else if (token == L"--ui-report") {
                parsed.report_path = value;
            } else if (token == L"--ui-result") {
                parsed.ui_result = value;
            } else if (token == L"--ui-theme") {
                parsed.ui_theme = value;
            } else if (token == L"--ui-material") {
                parsed.ui_material = value;
            } else if (token == L"--ui-progress") {
                wchar_t* end = nullptr;
                const long percent = std::wcstol(value.c_str(), &end, 10);
                if (end != nullptr && *end == L'\0' && percent >= 0 && percent <= 100) {
                    parsed.ui_percent = static_cast<int>(percent);
                }
            } else if (token == L"--exit-after" && !value.empty()) {
                wchar_t* end = nullptr;
                const long milliseconds = std::wcstol(value.c_str(), &end, 10);
                if (end != nullptr && *end == L'\0' && milliseconds > 0 && milliseconds <= 600000) {
                    parsed.exit_after_ms = static_cast<int>(milliseconds);
                }
            }
            continue;
        }

        if (token == L"--help" || token == L"-h" || token == L"/?") {
            parsed.help = true;
            continue;
        }

        // ARP 的卸载入口与 CLI 同形：`uninstall --family-name <PFN>`。
        if (token == L"uninstall") {
            parsed.uninstall = true;
            continue;
        }

        if (token == L"--family-name" || token == L"--install-id") {
            const bool has_value = index + 1 < argc;
            const std::wstring value = has_value ? std::wstring(argv[index + 1]) : std::wstring();
            if (has_value) {
                ++index;
            }

            if (token == L"--family-name") {
                parsed.uninstall_family_name = value;
            } else {
                parsed.uninstall_install_id = value;
            }

            continue;
        }

        // 已知的开关型参数直接忽略（GUI 目前只消费容器路径、卸载参数与 --manage）。
        if (token.front() == L'-') {
            continue;
        }

        // `install <path>` 与直接给 `<path>` 等价：让 GUI / CLI 的命令行可以互换。
        if (token == L"install") {
            continue;
        }

        // 剩下的是 CLI 独有的子命令：认下来、立刻返回，绝不落到「容器路径」上。
        if (is_cli_only_subcommand(token)) {
            parsed.unsupported_subcommand = std::wstring(token);
            ::LocalFree(argv);
            return parsed;
        }

        if (parsed.container.empty() && !parsed.uninstall) {
            parsed.container = std::filesystem::path(std::wstring(token));
        }
    }

    ::LocalFree(argv);
    return parsed;
}

}  // namespace msixpkg::gui
