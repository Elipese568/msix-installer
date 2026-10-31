#include "setup_options.h"

#include "setup_util.h"

#include <algorithm>
#include <cstdlib>
#include <cwchar>

namespace msixpkg::setup {
namespace {

bool parse_int(std::wstring_view text, int& value) {
    if (text.empty()) {
        return false;
    }
    const std::wstring owned(text);
    wchar_t* end = nullptr;
    const long parsed = ::wcstol(owned.c_str(), &end, 10);
    if (end == nullptr || *end != L'\0' || parsed < 0 || parsed > 1000000) {
        return false;
    }
    value = static_cast<int>(parsed);
    return true;
}

bool page_from_name(std::wstring_view name, UiPage& page) {
    if (equals_ignore_case(name, L"welcome")) {
        page = UiPage::Welcome;
    } else if (equals_ignore_case(name, L"options")) {
        page = UiPage::Options;
    } else if (equals_ignore_case(name, L"progress")) {
        page = UiPage::Progress;
    } else if (equals_ignore_case(name, L"finish")) {
        page = UiPage::Finish;
    } else if (equals_ignore_case(name, L"confirm")) {
        page = UiPage::Confirm;
    } else {
        return false;
    }
    return true;
}

bool take_value(int& index, int argc, const wchar_t* const* argv, std::wstring& value,
                std::wstring& error, std::wstring_view option) {
    if (index + 1 >= argc) {
        error = std::wstring(option) + L" needs a value";
        return false;
    }
    ++index;
    value = argv[index] != nullptr ? argv[index] : L"";
    return true;
}

}  // namespace

ParseResult parse_arguments(int argc, const wchar_t* const* argv) {
    ParseResult result;
    result.ok = false;
    Options& options = result.options;

    // 环境开关：CI 的负例要证明「未提权 + /ALLUSERS 会被拒绝」，不能在这台机器上弹 UAC。
    if (equals_ignore_case(get_env(L"MSIXPKG_SETUP_NO_ELEVATE"), L"1")) {
        options.no_elevate = true;
    }

    for (int index = 1; index < argc; ++index) {
        const wchar_t* raw = argv[index];
        if (raw == nullptr) {
            continue;
        }
        const std::wstring argument(raw);
        if (argument.empty()) {
            continue;
        }

        // /D=<目录> 是 NSIS 的写法，必须优先处理（值里可以有空格与等号）。
        if (starts_with_ignore_case(argument, L"/D=")) {
            options.install_dir = std::filesystem::path(argument.substr(3));
            options.install_dir_explicit = true;
            continue;
        }
        if (starts_with_ignore_case(argument, L"--dir=")) {
            options.install_dir = std::filesystem::path(argument.substr(6));
            options.install_dir_explicit = true;
            continue;
        }

        if (equals_ignore_case(argument, L"/S") || equals_ignore_case(argument, L"--silent") ||
            equals_ignore_case(argument, L"/SILENT")) {
            options.silent = true;
        } else if (equals_ignore_case(argument, L"/CURRENTUSER") ||
                   equals_ignore_case(argument, L"--current-user")) {
            if (options.mode == InstallMode::AllUsers) {
                result.error = L"/CURRENTUSER and /ALLUSERS are mutually exclusive";
                return result;
            }
            options.mode = InstallMode::CurrentUser;
        } else if (equals_ignore_case(argument, L"/ALLUSERS") ||
                   equals_ignore_case(argument, L"--all-users")) {
            if (options.mode == InstallMode::CurrentUser) {
                result.error = L"/CURRENTUSER and /ALLUSERS are mutually exclusive";
                return result;
            }
            options.mode = InstallMode::AllUsers;
        } else if (equals_ignore_case(argument, L"/NOASSOC")) {
            options.no_assoc = true;
        } else if (equals_ignore_case(argument, L"/NOPATH")) {
            options.no_path = true;
        } else if (equals_ignore_case(argument, L"/NODESKTOP")) {
            options.desktop = false;
        } else if (equals_ignore_case(argument, L"/DESKTOP")) {
            options.desktop = true;
        } else if (equals_ignore_case(argument, L"/ELEVATED") ||
                   equals_ignore_case(argument, L"--elevated")) {
            options.elevated = true;
        } else if (equals_ignore_case(argument, L"/NOELEVATE") ||
                   equals_ignore_case(argument, L"--no-elevate")) {
            options.no_elevate = true;
        } else if (equals_ignore_case(argument, L"/AUTO") || equals_ignore_case(argument, L"--auto")) {
            options.auto_mode = true;
        } else if (equals_ignore_case(argument, L"--verify-payload")) {
            options.verify_payload = true;
        } else if (equals_ignore_case(argument, L"/?") || equals_ignore_case(argument, L"--help") ||
                   equals_ignore_case(argument, L"/HELP")) {
            options.help = true;
        } else if (equals_ignore_case(argument, L"--ui-page")) {
            std::wstring value;
            if (!take_value(index, argc, argv, value, result.error, L"--ui-page")) {
                return result;
            }
            if (!page_from_name(value, options.ui_page)) {
                result.error = L"unknown --ui-page value: " + value;
                return result;
            }
        } else if (equals_ignore_case(argument, L"--ui-result")) {
            if (!take_value(index, argc, argv, options.ui_result, result.error, L"--ui-result")) {
                return result;
            }
        } else if (equals_ignore_case(argument, L"--ui-progress")) {
            std::wstring value;
            if (!take_value(index, argc, argv, value, result.error, L"--ui-progress")) {
                return result;
            }
            if (!parse_int(value, options.ui_percent) || options.ui_percent > 100) {
                result.error = L"--ui-progress expects 0..100";
                return result;
            }
        } else if (equals_ignore_case(argument, L"--ui-theme")) {
            if (!take_value(index, argc, argv, options.ui_theme, result.error, L"--ui-theme")) {
                return result;
            }
        } else if (equals_ignore_case(argument, L"--ui-material")) {
            if (!take_value(index, argc, argv, options.ui_material, result.error, L"--ui-material")) {
                return result;
            }
        } else if (equals_ignore_case(argument, L"--ui-report")) {
            if (!take_value(index, argc, argv, options.report_path, result.error, L"--ui-report")) {
                return result;
            }
        } else if (equals_ignore_case(argument, L"--exit-after")) {
            std::wstring value;
            if (!take_value(index, argc, argv, value, result.error, L"--exit-after")) {
                return result;
            }
            if (!parse_int(value, options.exit_after_ms)) {
                result.error = L"--exit-after expects milliseconds";
                return result;
            }
        } else if (equals_ignore_case(argument, L"--log-dir")) {
            if (!take_value(index, argc, argv, options.log_dir, result.error, L"--log-dir")) {
                return result;
            }
        } else if (equals_ignore_case(argument, L"--dir")) {
            std::wstring value;
            if (!take_value(index, argc, argv, value, result.error, L"--dir")) {
                return result;
            }
            options.install_dir = std::filesystem::path(value);
            options.install_dir_explicit = true;
        } else if (starts_with_ignore_case(argument, L"_?=")) {
            // NSIS 卸载器的「等我跑完」开关。我们的卸载器同步完成收尾工作，
            // 保留这个参数只是为了不弄坏既有脚本；值本身没有语义。
            continue;
        } else {
            result.error = L"unknown argument: " + argument;
            return result;
        }
    }

    result.ok = true;
    return result;
}

std::vector<std::wstring> elevation_arguments(const Options& options) {
    std::vector<std::wstring> arguments;
    arguments.emplace_back(L"/S");
    arguments.emplace_back(L"/ELEVATED");

    switch (options.mode) {
        case InstallMode::AllUsers:
            arguments.emplace_back(L"/ALLUSERS");
            break;
        case InstallMode::CurrentUser:
            arguments.emplace_back(L"/CURRENTUSER");
            break;
        case InstallMode::Auto:
            break;
    }

    if (!options.install_dir.empty()) {
        arguments.emplace_back(L"/D=" + options.install_dir.wstring());
    }
    if (options.no_assoc) {
        arguments.emplace_back(L"/NOASSOC");
    }
    if (options.no_path) {
        arguments.emplace_back(L"/NOPATH");
    }
    if (options.desktop) {
        arguments.emplace_back(L"/DESKTOP");
    }
    if (!options.log_dir.empty()) {
        arguments.emplace_back(L"--log-dir");
        arguments.emplace_back(options.log_dir);
    }
    return arguments;
}

std::wstring describe_options(const Options& options) {
    std::wstring text;
    text += L"mode=";
    switch (options.mode) {
        case InstallMode::AllUsers:
            text += L"allusers";
            break;
        case InstallMode::CurrentUser:
            text += L"currentuser";
            break;
        case InstallMode::Auto:
            text += L"auto";
            break;
    }
    text += options.silent ? L" silent" : L" ui";
    if (options.install_dir_explicit) {
        text += L" dir=" + options.install_dir.wstring();
    }
    if (options.no_assoc) {
        text += L" noassoc";
    }
    if (options.no_path) {
        text += L" nopath";
    }
    if (options.desktop) {
        text += L" desktop";
    }
    return text;
}

}  // namespace msixpkg::setup
