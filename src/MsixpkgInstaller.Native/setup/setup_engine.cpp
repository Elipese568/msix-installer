#include "setup_engine.h"

#include "setup_path.h"
#include "setup_payload.h"
#include "setup_registry.h"
#include "setup_shortcuts.h"
#include "setup_util.h"
#include "setup_version.h"

#include <ShlObj.h>
#include <shellapi.h>

#include <algorithm>
#include <chrono>
#include <system_error>
#include <vector>

namespace msixpkg::setup {
namespace {

constexpr int kPhaseFiles = 0;
constexpr int kPhaseShortcuts = 1;
constexpr int kPhaseRecords = 2;
constexpr int kPhaseAssoc = 3;
constexpr int kPhasePath = 4;

constexpr int kUninstallPhaseShortcuts = 0;
constexpr int kUninstallPhaseRegistry = 1;
constexpr int kUninstallPhasePath = 2;
constexpr int kUninstallPhaseFiles = 3;
constexpr int kUninstallPhaseCleanup = 4;

const wchar_t* mode_text(InstallMode mode) {
    switch (mode) {
        case InstallMode::AllUsers:
            return L"allusers";
        case InstallMode::CurrentUser:
            return L"currentuser";
        case InstallMode::Auto:
            break;
    }
    return L"auto";
}

HKEY root_for(InstallMode mode) {
    return mode == InstallMode::AllUsers ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER;
}

HKEY other_root(HKEY root) {
    return root == HKEY_LOCAL_MACHINE ? HKEY_CURRENT_USER : HKEY_LOCAL_MACHINE;
}

void emit(const ProgressCallback& progress, ProgressUpdate update) {
    if (progress) {
        progress(update);
    }
}

void emit_phase(const ProgressCallback& progress, int percent, int phase, std::wstring detail) {
    ProgressUpdate update;
    update.kind = ProgressKind::Phase;
    update.percent = percent;
    update.phase = phase;
    update.detail = std::move(detail);
    emit(progress, std::move(update));
}

void emit_detail(const ProgressCallback& progress, std::wstring detail) {
    ProgressUpdate update;
    update.kind = ProgressKind::Detail;
    update.detail = std::move(detail);
    emit(progress, std::move(update));
}

InstallResult finish(InstallResult result, const ProgressCallback& progress) {
    ProgressUpdate update;
    update.kind = ProgressKind::Finished;
    update.exit_code = result.exit_code;
    update.failed = result.exit_code != kExitSuccess;
    update.detail = result.error;
    emit(progress, std::move(update));
    return result;
}

bool read_record_string(HKEY root, const wchar_t* name, std::wstring& value, bool& exists) {
    value.clear();
    exists = false;
    return reg_read_string(root, kCfgKey, name, value, exists);
}

std::wstring join_list(const std::vector<std::wstring>& items) {
    std::wstring text;
    for (size_t index = 0; index < items.size(); ++index) {
        if (index != 0) {
            text.push_back(L';');
        }
        text += items[index];
    }
    return text;
}

enum class TargetState {
    Missing,
    Writable,
    Busy,
    Denied,
};

TargetState probe_target(const std::filesystem::path& path, DWORD& error) {
    error = 0;
    if (!std::filesystem::exists(path)) {
        return TargetState::Missing;
    }
    const HANDLE file = ::CreateFileW(path.c_str(), GENERIC_WRITE,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                      FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        ::CloseHandle(file);
        return TargetState::Writable;
    }
    error = ::GetLastError();
    if (error == ERROR_SHARING_VIOLATION || error == ERROR_LOCK_VIOLATION) {
        return TargetState::Busy;
    }
    if (error == ERROR_ACCESS_DENIED) {
        return TargetState::Denied;
    }
    return TargetState::Writable;
}

bool probe_directory(const std::filesystem::path& directory, std::wstring& error) {
    std::error_code code;
    std::filesystem::create_directories(directory, code);
    if (code) {
        error = L"cannot create " + directory.wstring() + L": " + to_utf16(code.message());
        return false;
    }

    const std::wstring name = L".msixpkg-write-probe-" + std::to_wstring(::GetCurrentProcessId());
    const std::filesystem::path probe = directory / name;
    const HANDLE file = ::CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                      FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = L"cannot write into " + directory.wstring() + L": " +
                format_win32_error(::GetLastError());
        return false;
    }
    ::CloseHandle(file);
    return true;
}

bool install_entry(const Payload& payload, const std::string& name, const std::filesystem::path& target,
                   std::wstring& error) {
    const PayloadEntry* entry = payload.find(name);
    if (entry == nullptr) {
        error = L"payload entry is missing: " + to_utf16(name);
        return false;
    }

    const std::filesystem::path staging = target.wstring() + L".msixpkg-stage";
    const std::filesystem::path backup = target.wstring() + L".msixpkg-bak";
    ::DeleteFileW(staging.c_str());
    ::DeleteFileW(backup.c_str());

    if (!payload.extract_to(*entry, staging, error)) {
        return false;
    }

    const bool had_target = std::filesystem::exists(target);
    if (had_target && !::MoveFileExW(target.c_str(), backup.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        error = L"cannot replace " + target.wstring() + L": " + format_win32_error(::GetLastError());
        ::DeleteFileW(staging.c_str());
        return false;
    }

    if (!::MoveFileExW(staging.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error = L"cannot move " + staging.wstring() + L" into place: " +
                format_win32_error(::GetLastError());
        if (had_target) {
            ::MoveFileExW(backup.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING);
        }
        ::DeleteFileW(staging.c_str());
        return false;
    }
    ::DeleteFileW(backup.c_str());
    return true;
}

bool add_path_entry(HKEY root, const std::filesystem::path& directory, Log& log, bool& warnings) {
    PathValue current;
    std::wstring error;
    if (!read_environment_path(root, current, error)) {
        log.line(L"path: WARNING - " + error);
        warnings = true;
        return false;
    }

    const std::wstring entry = directory.wstring();
    bool changed = false;
    const std::wstring updated = path_add(current.value, entry, changed);
    if (!changed) {
        log.line(L"path: already present: " + entry);
        return true;
    }

    if (!current.exists) {
        current.type = REG_EXPAND_SZ;
    }
    current.value = updated;
    if (!write_environment_path(root, current, error)) {
        log.line(L"path: WARNING - could not write PATH: " + error);
        warnings = true;
        return false;
    }
    broadcast_environment_change();
    log.line(L"path: added " + entry);
    return true;
}

bool remove_path_entry(HKEY root, const std::filesystem::path& directory, Log& log, bool& warnings) {
    PathValue current;
    std::wstring error;
    if (!read_environment_path(root, current, error)) {
        log.line(L"path: WARNING - " + error);
        warnings = true;
        return false;
    }
    if (!current.exists || current.value.empty()) {
        return true;
    }

    const std::wstring entry = directory.wstring();
    bool changed = false;
    const std::wstring updated = path_remove(current.value, entry, changed);
    if (!changed) {
        return true;
    }

    if (updated.empty()) {
        if (!delete_environment_path(root, error)) {
            log.line(L"path: WARNING - could not delete PATH: " + error);
            warnings = true;
            return false;
        }
        log.line(L"path: value removed (it only contained " + entry + L")");
    } else {
        current.value = updated;
        if (!write_environment_path(root, current, error)) {
            log.line(L"path: WARNING - could not write PATH: " + error);
            warnings = true;
            return false;
        }
        log.line(L"path: removed " + entry);
    }
    broadcast_environment_change();
    return true;
}

bool register_association(HKEY root, const std::filesystem::path& directory, const Strings& strings,
                          Log& log, bool& warnings) {
    const std::wstring command = L"\"" + (directory / kGuiExeName).wstring() + L"\" \"%1\"";
    const std::wstring icon = (directory / kGuiExeName).wstring() + L",0";
    bool ok = true;
    std::wstring error;

    std::wstring existing_default;
    bool existing_exists = false;
    (void)reg_read_string(root, L"Software\\Classes\\.msixpkg", L"", existing_default, existing_exists);
    const bool keep_existing = existing_exists && !existing_default.empty() &&
                               !equals_ignore_case(existing_default, kProgId);
    if (keep_existing) {
        log.line(L"assoc: existing default handler kept: " + existing_default);
    } else if (!reg_write_string(root, L"Software\\Classes\\.msixpkg", L"", kProgId, error)) {
        log.line(L"assoc: FAILED to register .msixpkg -> Msixpkg.Package: " + error);
        warnings = true;
        ok = false;
    }

    const wchar_t* prog_id = L"Software\\Classes\\Msixpkg.Package";

    ok = reg_write_string(root, L"Software\\Classes\\.msixpkg\\OpenWithProgids", kProgId, L"", error) && ok;
    ok = reg_write_string(root, prog_id, L"", strings.text(Str::AssocTypeName), error) && ok;
    ok = reg_write_string(root, prog_id, L"FriendlyTypeName", strings.text(Str::AssocTypeName), error) && ok;
    ok = reg_write_string(root, L"Software\\Classes\\Msixpkg.Package\\DefaultIcon", L"", icon, error) && ok;
    ok = reg_write_string(root, L"Software\\Classes\\Msixpkg.Package\\shell\\open\\command", L"", command,
                          error) &&
         ok;

    ok = reg_write_string(root, kCapabilitiesKey, L"ApplicationName", strings.text(Str::ProductName), error) && ok;
    ok = reg_write_string(root, kCapabilitiesKey, L"ApplicationDescription",
                          strings.text(Str::CapabilitiesDescription), error) &&
         ok;
    ok = reg_write_string(root, L"Software\\MsixpkgInstaller\\Capabilities\\FileAssociations",
                          L".msixpkg", kProgId, error) &&
         ok;
    ok = reg_write_string(root, kRegisteredApplications, strings.text(Str::ProductName), kCapabilitiesKey,
                          error) &&
         ok;

    if (!ok) {
        log.line(L"assoc: FAILED to register .msixpkg -> Msixpkg.Package: " + error);
        warnings = true;
        return false;
    }

    ::SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    log.line(L"assoc: .msixpkg -> Msixpkg.Package");
    return true;
}

void remove_association(HKEY root, Log& log) {
    std::wstring value;
    bool exists = false;
    (void)reg_read_string(root, L"Software\\Classes\\.msixpkg", L"", value, exists);
    if (exists && equals_ignore_case(value, kProgId)) {
        reg_delete_value(root, L"Software\\Classes\\.msixpkg", L"");
    }

    reg_delete_value(root, L"Software\\Classes\\.msixpkg\\OpenWithProgids", kProgId);
    reg_delete_key_if_empty(root, L"Software\\Classes\\.msixpkg\\OpenWithProgids");
    reg_delete_key_if_empty(root, L"Software\\Classes\\.msixpkg");
    reg_delete_key(root, L"Software\\Classes\\Msixpkg.Package");
    reg_delete_key(root, kCapabilitiesKey);
    reg_delete_value(root, kRegisteredApplications, L"Msixpkg Installer");
    reg_delete_value(root, kRegisteredApplications, L"Msixpkg 安装器");

    // 用户曾经在「默认应用」里亲手把 .msixpkg 指向我们：只有这时才清 UserChoice。
    const wchar_t* user_choice =
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts\\.msixpkg\\UserChoice";
    (void)reg_read_string(HKEY_CURRENT_USER, user_choice, L"ProgId", value, exists);
    if (exists && equals_ignore_case(value, kProgId)) {
        reg_delete_key(HKEY_CURRENT_USER, user_choice);
    }

    ::SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    log.line(L"assoc: removed");
}

bool write_record(HKEY root, const wchar_t* name, const std::wstring& value, Log& log,
                  bool& warnings, const wchar_t* context) {
    std::wstring error;
    if (!reg_write_string(root, kCfgKey, name, value, error)) {
        log.line(L"core: WARNING - " + std::wstring(context) + L" could not be written: " + error);
        warnings = true;
        return false;
    }
    return true;
}

bool write_arp(HKEY root, const std::filesystem::path& directory, const Strings& strings, Log& log,
               bool& warnings) {
    std::wstring error;
    bool ok = true;

    const std::wstring uninstaller = (directory / kUninstallerExeName).wstring();
    const std::wstring display_name = std::wstring(strings.text(Str::ProductName)) + L" " +
                                      MSIXPKG_SETUP_VERSION;
    const std::wstring uninstall_string = L"\"" + uninstaller + L"\"";
    const std::wstring quiet_uninstall_string = L"\"" + uninstaller + L"\" /S";

    auto write = [&](const wchar_t* name, const std::wstring& value) {
        if (!reg_write_string(root, kUninstallKey, name, value, error)) {
            ok = false;
        }
    };

    write(L"DisplayName", display_name);
    write(L"DisplayVersion", MSIXPKG_SETUP_VERSION);
    write(L"Publisher", strings.text(Str::ArpPublisher));
    write(L"DisplayIcon", (directory / kGuiExeName).wstring() + L",0");
    write(L"UninstallString", uninstall_string);
    write(L"QuietUninstallString", quiet_uninstall_string);
    write(L"InstallLocation", directory.wstring());
    write(L"Comments", strings.text(Str::ArpComments));

    if (!reg_write_dword(root, kUninstallKey, L"NoModify", 1, error) ||
        !reg_write_dword(root, kUninstallKey, L"NoRepair", 1, error)) {
        ok = false;
    }

    uint64_t estimated_size = 0;
    for (const wchar_t* name : {kGuiExeName, kCliExeName, kUninstallerExeName}) {
        uint64_t size = 0;
        if (file_size(directory / name, size)) {
            estimated_size += size;
        }
    }
    const DWORD estimated_kb = static_cast<DWORD>(std::max<uint64_t>(1, estimated_size / 1024));
    if (!reg_write_dword(root, kUninstallKey, L"EstimatedSize", estimated_kb, error)) {
        ok = false;
    }

    if (!ok) {
        log.line(L"core: WARNING - the Apps & features uninstall entry could not be written: " + error);
        warnings = true;
    }
    return ok;
}

bool create_start_menu_shortcuts(const std::filesystem::path& directory, const Strings& strings,
                                 bool all_users, std::vector<std::wstring>& links, Log& log,
                                 bool& warnings) {
    const std::filesystem::path folder = start_menu_folder(all_users);
    std::error_code code;
    std::filesystem::create_directories(folder, code);
    if (code) {
        log.line(L"shortcuts: WARNING - cannot create " + folder.wstring() + L": " +
                 to_utf16(code.message()));
        warnings = true;
        return false;
    }

    struct Item {
        Str name;
        Str description;
        std::filesystem::path target;
        std::wstring arguments;
    };
    const Item items[] = {
        {Str::ShortcutMain, Str::ShortcutMainDescription, directory / kGuiExeName, L""},
        {Str::ShortcutManage, Str::ShortcutManageDescription, directory / kGuiExeName, L"--manage"},
        {Str::ShortcutUninstall, Str::ShortcutUninstallDescription, directory / kUninstallerExeName, L""},
    };

    bool ok = true;
    for (const Item& item : items) {
        ShortcutSpec spec;
        spec.link = folder / (std::wstring(strings.text(item.name)) + L".lnk");
        spec.target = item.target;
        spec.arguments = item.arguments;
        spec.working_directory = directory;
        spec.icon = directory / kGuiExeName;
        spec.description = strings.text(item.description);

        std::wstring error;
        if (!create_shortcut(spec, error)) {
            log.line(L"shortcuts: WARNING - " + error);
            warnings = true;
            ok = false;
            continue;
        }
        links.push_back(spec.link.wstring());
    }
    if (ok) {
        log.line(L"shortcuts: start menu folder " + folder.wstring());
    }
    return ok;
}

bool path_has_json(const std::filesystem::path& directory) {
    std::error_code code;
    if (!std::filesystem::is_directory(directory, code)) {
        return false;
    }
    for (const auto& entry : std::filesystem::directory_iterator(directory, code)) {
        if (code) {
            return false;
        }
        if (entry.is_regular_file()) {
            const std::wstring extension = entry.path().extension().wstring();
            if (equals_ignore_case(extension, L".json")) {
                return true;
            }
        }
    }
    return false;
}

bool run_elevated(const Options& options, const Plan& plan, Log& log, const ProgressCallback& progress,
                  InstallResult& result) {
    Options child = options;
    child.mode = plan.mode;
    child.install_dir = plan.install_dir;
    child.install_dir_explicit = true;
    child.silent = true;
    child.elevated = true;
    child.ui_page = UiPage::None;
    child.ui_result.clear();
    child.ui_percent = -1;
    child.report_path.clear();
    child.exit_after_ms = 0;
    if (child.log_dir.empty() && log.available()) {
        child.log_dir = log.directory().wstring();
    }

    const std::vector<std::wstring> arguments = elevation_arguments(child);
    const std::wstring parameters = join_arguments(arguments);
    const std::wstring executable = self_path().wstring();
    const std::wstring directory = self_path().parent_path().wstring();

    log.line(L"elevation: requesting administrator approval");
    emit(progress, ProgressUpdate{ProgressKind::WaitingForElevation, 0, -1, false,
                                  L"waiting for administrator approval", 0});

    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    // 不要 SEE_MASK_NOASYNC：GUI 的编排线程没有初始化 COM，而这个函数要能在
    // 工作线程上调用；我们本来就会 WaitForSingleObject，不需要「异步启动完成」保证。
    info.fMask = SEE_MASK_NOCLOSEPROCESS;
    info.lpVerb = L"runas";
    info.lpFile = executable.c_str();
    info.lpParameters = parameters.c_str();
    info.lpDirectory = directory.c_str();
    info.nShow = SW_SHOWNORMAL;

    if (!::ShellExecuteExW(&info) || info.hProcess == nullptr) {
        const DWORD code = ::GetLastError();
        result.elevated_child = true;
        if (code == ERROR_CANCELLED) {
            log.line(L"elevation: declined by the user");
            result.exit_code = kExitNeedAdmin;
            result.error = L"administrator approval was declined";
        } else {
            log.line(L"elevation: could not start the elevated setup: " + format_win32_error(code));
            result.exit_code = kExitNeedAdmin;
            result.error = L"could not request administrator approval";
        }
        return false;
    }

    ::WaitForSingleObject(info.hProcess, INFINITE);
    DWORD code = 0;
    if (!::GetExitCodeProcess(info.hProcess, &code)) {
        code = static_cast<DWORD>(kExitIncomplete);
    }
    ::CloseHandle(info.hProcess);
    log.line(L"elevation: elevated setup exited with " + std::to_wstring(code));
    result.elevated_child = true;
    result.exit_code = static_cast<int>(code);
    if (result.exit_code != kExitSuccess) {
        result.error = L"the elevated setup did not complete the installation";
    }
    return result.exit_code == kExitSuccess;
}

}  // namespace

Plan plan_install(const Options& options, Log& log) {
    (void)log;
    Plan plan;
    plan.desktop = options.desktop;

    if (!is_windows_x64() || !is_windows_10_or_later()) {
        plan.ok = false;
        plan.exit_code = kExitPrecondition;
        plan.error = L"64-bit Windows 10 or later is required";
        return plan;
    }

    std::wstring machine_dir;
    bool machine_exists = false;
    std::wstring user_dir;
    bool user_exists = false;
    (void)read_record_string(HKEY_LOCAL_MACHINE, L"InstallDir", machine_dir, machine_exists);
    (void)read_record_string(HKEY_CURRENT_USER, L"InstallDir", user_dir, user_exists);
    plan.existing_machine = machine_exists && !machine_dir.empty();
    plan.existing_user = user_exists && !user_dir.empty();
    plan.admin = is_process_elevated();

    switch (options.mode) {
        case InstallMode::AllUsers:
            plan.mode = InstallMode::AllUsers;
            break;
        case InstallMode::CurrentUser:
            plan.mode = InstallMode::CurrentUser;
            break;
        case InstallMode::Auto:
            if (plan.existing_machine) {
                plan.mode = InstallMode::AllUsers;
            } else if (plan.existing_user) {
                plan.mode = InstallMode::CurrentUser;
            } else {
                plan.mode = plan.admin ? InstallMode::AllUsers : InstallMode::CurrentUser;
            }
            break;
    }

    if (options.install_dir_explicit && !options.install_dir.empty()) {
        plan.install_dir = options.install_dir;
    } else if (plan.mode == InstallMode::AllUsers) {
        plan.install_dir = plan.existing_machine ? std::filesystem::path(machine_dir)
                                                 : program_files_x64() / kProductFolder;
    } else {
        plan.install_dir = plan.existing_user ? std::filesystem::path(user_dir)
                                              : local_app_data() / L"Programs" / kProductFolder;
    }

    if (plan.install_dir.empty()) {
        plan.ok = false;
        plan.exit_code = kExitPrecondition;
        plan.error = L"could not resolve the install folder";
        return plan;
    }

    if (plan.mode == InstallMode::AllUsers && !plan.admin) {
        if (options.no_elevate || options.elevated) {
            plan.ok = false;
            plan.exit_code = kExitNeedAdmin;
            plan.error = L"administrator approval is required for /ALLUSERS";
            return plan;
        }
        plan.needs_elevation = true;
        plan.ok = true;
        return plan;
    }

    // 只有能写的目标才谈得上「被占用」；非提权去探测 Program Files 只会得到拒绝。
    if (plan.mode == InstallMode::CurrentUser || plan.admin) {
        DWORD error = 0;
        if (probe_target(plan.install_dir / kGuiExeName, error) == TargetState::Busy) {
            plan.ok = false;
            plan.exit_code = kExitBusy;
            plan.target_busy = true;
            plan.error = L"a running Msixpkg Installer is holding " +
                         (plan.install_dir / kGuiExeName).wstring();
            return plan;
        }
    }

    plan.ok = true;
    plan.exit_code = kExitSuccess;
    return plan;
}

Plan plan_uninstall(const Options& options, Log& log) {
    (void)log;
    Plan plan;
    plan.desktop = false;
    plan.admin = is_process_elevated();

    std::wstring machine_dir;
    bool machine_exists = false;
    std::wstring user_dir;
    bool user_exists = false;
    (void)read_record_string(HKEY_LOCAL_MACHINE, L"InstallDir", machine_dir, machine_exists);
    (void)read_record_string(HKEY_CURRENT_USER, L"InstallDir", user_dir, user_exists);
    plan.existing_machine = machine_exists && !machine_dir.empty();
    plan.existing_user = user_exists && !user_dir.empty();

    if (plan.existing_machine) {
        plan.mode = InstallMode::AllUsers;
        plan.install_dir = machine_dir;
    } else if (plan.existing_user) {
        plan.mode = InstallMode::CurrentUser;
        plan.install_dir = user_dir;
    } else {
        plan.mode = options.mode == InstallMode::AllUsers ? InstallMode::AllUsers : InstallMode::CurrentUser;
        plan.install_dir = self_path().parent_path();
    }

    if (plan.mode == InstallMode::AllUsers && !plan.admin) {
        if (options.no_elevate || options.elevated) {
            plan.ok = false;
            plan.exit_code = kExitNeedAdmin;
            plan.error = L"uninstalling a per-machine copy requires administrator approval";
            return plan;
        }
        plan.needs_elevation = true;
    }

    plan.ok = true;
    plan.exit_code = kExitSuccess;
    return plan;
}

InstallResult run_install(const Options& options, const Strings& strings, Log& log,
                          const ProgressCallback& progress) {
    InstallResult result;
    Plan plan = plan_install(options, log);
    result.install_dir = plan.install_dir;

    if (!plan.ok) {
        log.line(L"---- setup start: mode=" + std::wstring(mode_text(options.mode)) +
                 L" admin=" + (is_process_elevated() ? L"1" : L"0") + L" dir=" +
                 plan.install_dir.wstring() + L" silent=" + (options.silent ? L"1" : L"0"));
        log.line(L"abort: " + plan.error);
        result.exit_code = plan.exit_code;
        result.error = plan.error;
        return finish(result, progress);
    }

    log.line(L"---- setup start: mode=" + std::wstring(mode_text(plan.mode)) +
             L" admin=" + (plan.admin ? L"1" : L"0") + L" dir=" + plan.install_dir.wstring() +
             L" silent=" + (options.silent ? L"1" : L"0"));

    if (plan.needs_elevation) {
        (void)run_elevated(options, plan, log, progress, result);
        return finish(result, progress);
    }

    const HKEY root = root_for(plan.mode);
    const std::filesystem::path& directory = plan.install_dir;

    emit_phase(progress, 5, kPhaseFiles, L"preparing the install folder");
    std::wstring error;
    if (!probe_directory(directory, error)) {
        log.line(L"core: FAILED to write the program files: " + error);
        result.exit_code = kExitIncomplete;
        result.error = error;
        return finish(result, progress);
    }

    Payload payload;
    if (!payload.open(self_path(), error)) {
        log.line(L"payload: FAILED - " + error);
        result.exit_code = kExitIncomplete;
        result.error = error;
        return finish(result, progress);
    }

    DWORD probe_error = 0;
    for (const wchar_t* name : {kGuiExeName, kCliExeName, kUninstallerExeName}) {
        const TargetState state = probe_target(directory / name, probe_error);
        if (state == TargetState::Busy) {
            log.line(L"abort: msixpkg-installer.exe is locked (wizard running?)");
            result.exit_code = kExitBusy;
            result.error = strings.text(Str::TargetBusy);
            return finish(result, progress);
        }
    }

    emit_phase(progress, 15, kPhaseFiles, L"writing the program files");
    const std::pair<const char*, const wchar_t*> files[] = {
        {"msixpkg-installer.exe", kGuiExeName},
        {"msixpkg-native.exe", kCliExeName},
        {"Uninstall.exe", kUninstallerExeName},
    };
    for (const auto& [payload_name, target_name] : files) {
        if (!install_entry(payload, payload_name, directory / target_name, error)) {
            log.line(L"core: FAILED to write the program files: " + error);
            result.exit_code = kExitIncomplete;
            result.error = error;
            return finish(result, progress);
        }
    }
    log.line(L"core: files installed to " + directory.wstring());
    emit_phase(progress, 40, kPhaseFiles, L"program files installed");

    bool warnings = false;
    emit_phase(progress, 45, kPhaseShortcuts, L"creating shortcuts");
    std::vector<std::wstring> start_menu_links;
    create_start_menu_shortcuts(directory, strings, plan.mode == InstallMode::AllUsers,
                                start_menu_links, log, warnings);

    std::wstring desktop_link;
    if (options.desktop) {
        ShortcutSpec spec;
        spec.link = desktop_folder(plan.mode == InstallMode::AllUsers) /
                    (std::wstring(strings.text(Str::ShortcutMain)) + L".lnk");
        spec.target = directory / kGuiExeName;
        spec.working_directory = directory;
        spec.icon = directory / kGuiExeName;
        spec.description = strings.text(Str::ShortcutMainDescription);
        if (create_shortcut(spec, error)) {
            desktop_link = spec.link.wstring();
            log.line(L"shortcuts: desktop link " + desktop_link);
        } else {
            log.line(L"shortcuts: WARNING - " + error);
            warnings = true;
        }
    }

    emit_phase(progress, 60, kPhaseRecords, L"writing the uninstall entry");
    bool records_ok = true;
    records_ok = write_record(root, L"InstallDir", directory.wstring(), log, warnings,
                              L"install records") &&
                 records_ok;
    records_ok = write_record(root, L"InstallMode", mode_text(plan.mode), log, warnings,
                              L"install records") &&
                 records_ok;
    records_ok = write_record(root, L"Version", MSIXPKG_SETUP_VERSION, log, warnings,
                              L"install records") &&
                 records_ok;
    records_ok = write_record(root, L"StartMenuFolder",
                              start_menu_folder(plan.mode == InstallMode::AllUsers).wstring(), log,
                              warnings, L"install records") &&
                 records_ok;
    records_ok = write_record(root, L"StartMenuLinks", join_list(start_menu_links), log, warnings,
                              L"install records") &&
                 records_ok;
    if (!desktop_link.empty()) {
        (void)write_record(root, L"DesktopLink", desktop_link, log, warnings, L"install records");
    }
    if (!records_ok) {
        log.line(L"core: WARNING - install records could not be written");
    }
    write_arp(root, directory, strings, log, warnings);

    emit_phase(progress, 75, kPhaseAssoc, L"registering the .msixpkg association");
    bool association_ok = false;
    if (!options.no_assoc) {
        association_ok = register_association(root, directory, strings, log, warnings);
        if (association_ok) {
            (void)write_record(root, L"AssocRegistered", L"1", log, warnings, L"install records");
        }
    }

    emit_phase(progress, 85, kPhasePath, L"updating PATH");
    if (!options.no_path && add_path_entry(root, directory, log, warnings)) {
        (void)write_record(root, L"PathEntry", L"1", log, warnings, L"install records");
    }

    emit_phase(progress, 100, kPhaseFiles, L"done");
    log.line(L"core: shortcuts + uninstall entry written (mode=" + std::wstring(mode_text(plan.mode)) + L")");

    result.exit_code = kExitSuccess;
    result.warnings = warnings;
    result.install_dir = directory;
    return finish(result, progress);
}

InstallResult run_uninstall(const Options& options, const Strings& strings, Log& log,
                            const ProgressCallback& progress) {
    (void)strings;
    InstallResult result;
    Plan plan = plan_uninstall(options, log);
    result.install_dir = plan.install_dir;

    log.line(L"---- uninstall start: mode=" + std::wstring(mode_text(plan.mode)) + L" dir=" +
             plan.install_dir.wstring() + L" silent=" + (options.silent ? L"1" : L"0"));

    if (!plan.ok) {
        log.line(L"abort: " + plan.error);
        result.exit_code = plan.exit_code;
        result.error = plan.error;
        return finish(result, progress);
    }

    if (plan.needs_elevation) {
        (void)run_elevated(options, plan, log, progress, result);
        return finish(result, progress);
    }

    const HKEY root = root_for(plan.mode);
    const std::filesystem::path& directory = plan.install_dir;

    emit_phase(progress, 10, -1, L"checking installation records");
    const std::filesystem::path state = find_state_records();
    if (!state.empty()) {
        log.line(L"state: records kept at " + state.wstring());
        emit_detail(progress, L"installation records kept at " + state.wstring());
    }

    emit_phase(progress, 20, kUninstallPhaseShortcuts, L"removing shortcuts");
    std::wstring value;
    bool exists = false;
    for (HKEY candidate : {root, other_root(root)}) {
        if (read_record_string(candidate, L"DesktopLink", value, exists) && exists && !value.empty()) {
            std::wstring error;
            if (!delete_file_checked(value, error)) {
                log.line(L"uninstall: WARNING - " + error);
            }
        }
    }

    std::wstring start_menu;
    for (HKEY candidate : {root, other_root(root)}) {
        if (read_record_string(candidate, L"StartMenuFolder", value, exists) && exists && !value.empty()) {
            start_menu = value;
            break;
        }
    }
    if (start_menu.empty()) {
        start_menu = start_menu_folder(plan.mode == InstallMode::AllUsers).wstring();
    }
    std::wstring error;
    if (!remove_directory_tree(start_menu, error)) {
        log.line(L"uninstall: WARNING - " + error);
    }

    emit_phase(progress, 40, kUninstallPhaseRegistry, L"removing registry entries");
    remove_association(root, log);
    for (HKEY candidate : {root, other_root(root)}) {
        reg_delete_key(candidate, kUninstallKey);
        reg_delete_key(candidate, kCfgKey);
    }

    emit_phase(progress, 60, kUninstallPhasePath, L"removing the PATH entry");
    bool warnings = false;
    remove_path_entry(root, directory, log, warnings);

    emit_phase(progress, 75, kUninstallPhaseFiles, L"removing the program files");
    bool files_ok = true;
    for (const wchar_t* name : {kGuiExeName, kCliExeName}) {
        if (!delete_file_checked(directory / name, error)) {
            log.line(L"uninstall: program files could not be deleted (in use?): " + error);
            files_ok = false;
        }
    }

    emit_phase(progress, 95, kUninstallPhaseCleanup, L"cleaning up");
    ::RemoveDirectoryW(directory.c_str());

    if (!files_ok) {
        result.exit_code = kExitIncomplete;
        result.error = L"some program files are still in use";
        return finish(result, progress);
    }

    log.line(L"uninstall done");
    emit_phase(progress, 100, kUninstallPhaseCleanup, L"done");
    result.exit_code = kExitSuccess;
    result.warnings = warnings;
    return finish(result, progress);
}

std::filesystem::path find_state_records() {
    std::vector<std::filesystem::path> roots;
    const std::wstring override = get_env(L"MSIXPKG_DATA_ROOT");
    if (!override.empty()) {
        roots.emplace_back(override);
    }
    const std::filesystem::path program_data = known_folder(FOLDERID_ProgramData);
    if (!program_data.empty()) {
        roots.push_back(program_data / L"Msixpkg");
    }
    const std::filesystem::path app_data = known_folder(FOLDERID_RoamingAppData);
    if (!app_data.empty()) {
        roots.push_back(app_data / L"Msixpkg");
    }
    const std::filesystem::path local = local_app_data();
    if (!local.empty()) {
        roots.push_back(local / L"Msixpkg");
    }

    for (const std::filesystem::path& base : roots) {
        const std::filesystem::path state = base / L"state";
        if (path_has_json(state)) {
            return state;
        }
    }
    return {};
}

bool verify_embedded_payload(Log& log, std::wstring& error) {
    Payload payload;
    if (!payload.open(self_path(), error)) {
        log.line(L"payload: " + error);
        return false;
    }

    std::vector<std::wstring> report;
    if (!payload.verify_all(report, error)) {
        log.line(L"payload: " + error);
        return false;
    }
    for (const std::wstring& line : report) {
        log.line(L"payload: " + line);
    }
    return true;
}

bool schedule_self_delete(const std::filesystem::path& self, const std::filesystem::path& install_dir,
                          Log& log) {
    std::wstring error;
    if (!std::filesystem::exists(self)) {
        (void)delete_file_checked(install_dir / kUninstallerExeName, error);
        ::RemoveDirectoryW(install_dir.c_str());
        return true;
    }

    const std::filesystem::path parent = self.parent_path();
    std::wstring parent_text = parent.wstring();
    std::wstring install_text = install_dir.wstring();
    while (!parent_text.empty() && (parent_text.back() == L'\\' || parent_text.back() == L'/')) {
        parent_text.pop_back();
    }
    while (!install_text.empty() && (install_text.back() == L'\\' || install_text.back() == L'/')) {
        install_text.pop_back();
    }
    if (!equals_ignore_case(parent_text, install_text)) {
        // 从别处复制的卸载器：安装目录里的原件可以立刻删，不需要自删除技巧。
        (void)delete_file_checked(install_dir / kUninstallerExeName, error);
        ::RemoveDirectoryW(install_dir.c_str());
        return true;
    }

    const std::filesystem::path renamed =
        install_dir / (L"~uninstall-" + std::to_wstring(::GetCurrentProcessId()) + L".tmp");
    if (!::MoveFileExW(self.c_str(), renamed.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        const DWORD code = ::GetLastError();
        log.line(L"uninstall: WARNING - could not rename the uninstaller: " + format_win32_error(code));
        ::MoveFileExW(self.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
        return false;
    }

    // 自己还在跑，删不掉自己；交给一个 detached cmd：先等约 1 秒让本进程退出，再删文件与空目录。
    // 必须用 System32 下的完整路径，不能让 CreateProcess 先搜当前目录。
    wchar_t system_directory[MAX_PATH]{};
    ::GetSystemDirectoryW(system_directory, MAX_PATH);
    const std::filesystem::path shell = std::filesystem::path(system_directory) / L"cmd.exe";
    std::wstring command_line = quote_argument(shell.wstring()) + L" /d /c \"ping -n 2 127.0.0.1 >nul & del /f /q " +
                                quote_argument(renamed.wstring()) + L" & rmdir " +
                                quote_argument(install_dir.wstring()) + L"\"";
    std::wstring mutable_command = command_line;
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    const BOOL started = ::CreateProcessW(nullptr, mutable_command.data(), nullptr, nullptr, FALSE,
                                          CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
    if (!started) {
        log.line(L"uninstall: WARNING - could not start the cleanup helper: " +
                 format_win32_error(::GetLastError()));
        ::MoveFileExW(renamed.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
        return false;
    }
    ::CloseHandle(process.hThread);
    ::CloseHandle(process.hProcess);
    log.line(L"uninstall: cleanup helper started");
    return true;
}

}  // namespace msixpkg::setup
