#include "setup_shortcuts.h"

#include "setup_util.h"

#include <ShlObj.h>
#include <shobjidl.h>

#include <system_error>

namespace msixpkg::setup {

std::filesystem::path start_menu_folder(bool all_users) {
    return known_folder(all_users ? FOLDERID_CommonPrograms : FOLDERID_Programs) / kProductFolder;
}

std::filesystem::path public_start_menu() {
    return known_folder(FOLDERID_CommonPrograms) / kProductFolder;
}

std::filesystem::path desktop_folder(bool all_users) {
    return known_folder(all_users ? FOLDERID_PublicDesktop : FOLDERID_Desktop);
}

bool create_shortcut(const ShortcutSpec& spec, std::wstring& error) {
    std::error_code code;
    std::filesystem::create_directories(spec.link.parent_path(), code);
    if (code) {
        error = L"cannot create " + spec.link.parent_path().wstring() + L": " + to_utf16(code.message());
        return false;
    }

    IShellLinkW* shell_link = nullptr;
    HRESULT result = ::CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                        IID_IShellLinkW, reinterpret_cast<void**>(&shell_link));
    if (FAILED(result) || shell_link == nullptr) {
        error = L"CoCreateInstance(ShellLink) failed: " + format_hresult(result);
        return false;
    }

    bool ok = true;
    result = shell_link->SetPath(spec.target.c_str());
    if (SUCCEEDED(result) && !spec.arguments.empty()) {
        result = shell_link->SetArguments(spec.arguments.c_str());
    }
    if (SUCCEEDED(result) && !spec.working_directory.empty()) {
        result = shell_link->SetWorkingDirectory(spec.working_directory.c_str());
    }
    if (SUCCEEDED(result) && !spec.icon.empty()) {
        result = shell_link->SetIconLocation(spec.icon.c_str(), 0);
    }
    if (SUCCEEDED(result) && !spec.description.empty()) {
        result = shell_link->SetDescription(spec.description.c_str());
    }
    if (FAILED(result)) {
        error = L"configuring the shortcut failed: " + format_hresult(result);
        ok = false;
    }

    IPersistFile* persist = nullptr;
    if (ok) {
        result = shell_link->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(&persist));
        if (FAILED(result) || persist == nullptr) {
            error = L"IPersistFile is unavailable: " + format_hresult(result);
            ok = false;
        }
    }
    if (ok) {
        result = persist->Save(spec.link.c_str(), TRUE);
        if (FAILED(result)) {
            error = L"IPersistFile::Save(" + spec.link.wstring() + L") failed: " + format_hresult(result);
            ok = false;
        }
    }

    if (persist != nullptr) {
        persist->Release();
    }
    shell_link->Release();
    return ok;
}

bool delete_file_checked(const std::filesystem::path& path, std::wstring& error) {
    if (path.empty()) {
        return true;
    }
    if (!::DeleteFileW(path.c_str())) {
        const DWORD code = ::GetLastError();
        if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
            return true;
        }
        error = L"DeleteFileW(" + path.wstring() + L"): " + format_win32_error(code);
        return false;
    }
    return true;
}

bool remove_directory_tree(const std::filesystem::path& path, std::wstring& error) {
    if (path.empty()) {
        return true;
    }
    std::error_code code;
    const std::uintmax_t removed = std::filesystem::remove_all(path, code);
    if (code) {
        error = L"remove_all(" + path.wstring() + L"): " + to_utf16(code.message());
        return false;
    }
    (void)removed;
    return true;
}

bool directory_is_empty(const std::filesystem::path& path) {
    std::error_code code;
    return !std::filesystem::exists(path, code) ||
           (std::filesystem::is_directory(path, code) && std::filesystem::is_empty(path, code));
}

bool file_size(const std::filesystem::path& path, uint64_t& size) {
    std::error_code code;
    size = static_cast<uint64_t>(std::filesystem::file_size(path, code));
    return !code;
}

}  // namespace msixpkg::setup
