#include "setup_entry.h"

#include "setup_engine.h"
#include "setup_log.h"
#include "setup_options.h"
#include "setup_strings.h"
#include "setup_ui.h"
#include "setup_util.h"

#include <objbase.h>
#include <shellapi.h>

#include <string>

namespace msixpkg::setup {
namespace {

int run_silent(const Options& options, const Strings& strings, Log& log, bool uninstall_mode) {
    auto progress = [&log](const ProgressUpdate& update) {
        switch (update.kind) {
            case ProgressKind::Phase:
                log.line(L"phase " + std::to_wstring(update.phase) + L" " +
                         std::to_wstring(update.percent) + L"% " + update.detail);
                break;
            case ProgressKind::Percent:
                break;
            case ProgressKind::Detail:
                log.line(update.detail);
                break;
            case ProgressKind::WaitingForElevation:
                log.line(L"waiting for administrator approval");
                break;
            case ProgressKind::Finished:
                log.line(L"finished exit=" + std::to_wstring(update.exit_code));
                break;
        }
    };

    const InstallResult result =
        uninstall_mode ? run_uninstall(options, strings, log, progress)
                       : run_install(options, strings, log, progress);

    if (uninstall_mode && result.exit_code == kExitSuccess) {
        (void)schedule_self_delete(self_path(), result.install_dir, log);
    }
    return result.exit_code;
}

}  // namespace

int run_setup_process(HINSTANCE instance, bool uninstall_mode) {
    int argc = 0;
    LPWSTR* argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);
    const ParseResult parsed = parse_arguments(argc, argv);
    if (argv != nullptr) {
        ::LocalFree(argv);
    }

    const Strings strings;
    Options options = parsed.options;

    Log log;
    log.open(options, uninstall_mode ? L"uninstall" : L"setup");
    log.line(L"---- process start: " + describe_options(options));

    if (!parsed.ok) {
        log.line(L"abort: " + parsed.error);
        if (!options.silent) {
            ::MessageBoxW(nullptr, parsed.error.c_str(),
                          strings.text(uninstall_mode ? Str::WindowTitleUninstall : Str::WindowTitleInstall),
                          MB_OK | MB_ICONERROR);
        }
        return kExitPrecondition;
    }

    if (options.help) {
        if (options.silent) {
            log.line(strings.text(Str::HelpBody));
        } else {
            ::MessageBoxW(nullptr, strings.text(Str::HelpBody), strings.text(Str::HelpTitle),
                          MB_OK | MB_ICONINFORMATION);
        }
        return kExitSuccess;
    }

    if (options.verify_payload) {
        std::wstring error;
        const bool ok = verify_embedded_payload(log, error);
        if (!ok) {
            log.line(L"payload verification failed: " + error);
        }
        return ok ? kExitSuccess : kExitIncomplete;
    }

    // Wren 的 Host 会自己 OleInitialize；这里先以 STA 初始化，快捷方式（IShellLink）与
    // 文件夹选择器都要求这个套间。重复初始化返回 S_FALSE，不是错误。
    const HRESULT com = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    const int code = options.silent
                         ? run_silent(options, strings, log, uninstall_mode)
                         : (uninstall_mode ? run_uninstall_ui(instance, options, strings, log)
                                           : run_setup_ui(instance, options, strings, log));

    if (SUCCEEDED(com)) {
        ::CoUninitialize();
    }
    log.line(L"---- process exit " + std::to_wstring(code));
    return code;
}

}  // namespace msixpkg::setup
