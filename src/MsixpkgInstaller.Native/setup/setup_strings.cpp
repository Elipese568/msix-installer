#include "setup_strings.h"

#include <Windows.h>

#include <array>

namespace msixpkg::setup {
namespace {

struct Entry {
    const wchar_t* en;
    const wchar_t* zh;
};

// 顺序必须与 setup_strings.h 的 Str 完全一致（static_assert 会盯着）。
constexpr std::array<Entry, 86> kEntries{{
    {L"Msixpkg Installer Setup", L"Msixpkg 安装器 安装程序"},
    {L"Uninstall Msixpkg Installer", L"卸载 Msixpkg 安装器"},
    {L"Msixpkg Installer", L"Msixpkg 安装器"},
    {L"Install Msixpkg Installer {version}", L"安装 Msixpkg 安装器 {version}"},
    {L"Install .msixpkg packages without leaving the wizard",
     L"用向导安装 .msixpkg 包"},
    {L"This setup installs the tool that installs .msixpkg packages: the wizard "
     L"msixpkg-installer.exe and the command line tool msixpkg-native.exe.\r\n\r\n"
     L"It installs no applications and never touches the certificate store; both are "
     L"handled by the wizard only with your explicit consent. The files are read from "
     L"this setup itself, so no temporary folder is needed.",
     L"这个安装程序会装上「用来安装 .msixpkg 包的工具」：向导 msixpkg-installer.exe "
     L"与命令行 msixpkg-native.exe。\r\n\r\n"
     L"它不安装任何应用，也不会改动证书存储；应用与发布者证书只会在你明确同意后由"
     L"向导处理。程序文件直接从安装程序自身读取，因此不需要可写的临时目录。"},
    {L"What you get:\r\n"
     L"• msixpkg-installer.exe — the wizard\r\n"
     L"• msixpkg-native.exe — the command line\r\n"
     L"• Start menu shortcuts, the .msixpkg association and an uninstall entry",
     L"装完之后：\r\n"
     L"• msixpkg-installer.exe —— 向导\r\n"
     L"• msixpkg-native.exe —— 命令行\r\n"
     L"• 开始菜单快捷方式、.msixpkg 关联与「设置 → 应用」卸载入口"},
    {L"Choose what to install", L"选择安装选项"},
    {L"Both options install the same files; they only differ in scope and permissions.",
     L"两种范围装的是同一套文件，区别只在作用范围与所需权限。"},
    {L"For all users", L"为所有用户安装"},
    {L"Program Files, HKLM. Windows asks for administrator approval.",
     L"装到 Program Files 并写 HKLM；Windows 会请求管理员批准。"},
    {L"Only for me", L"仅为我安装"},
    {L"%LOCALAPPDATA%\\Programs, HKCU. No administrator rights required.",
     L"装到 %LOCALAPPDATA%\\Programs 并写 HKCU；不需要管理员权限。"},
    {L"Install location", L"安装位置"},
    {L"The folder is added to PATH when that component is selected.",
     L"如果勾选了 PATH 组件，这个目录会被加入 PATH。"},
    {L"Browse...", L"浏览..."},
    {L"Components", L"附带组件"},
    {L".msixpkg file association", L".msixpkg 文件关联"},
    {L"Double-clicking a .msixpkg opens the wizard on the review page.",
     L"双击 .msixpkg 会用向导打开并停在摘要页。"},
    {L"Add the install folder to PATH", L"把安装目录加入 PATH"},
    {L"Lets you run msixpkg-native from any shell.",
     L"让你在任意 shell 里直接调用 msixpkg-native。"},
    {L"Desktop shortcut", L"桌面快捷方式"},
    {L"Creates a Msixpkg Installer shortcut on the desktop.",
     L"在桌面放一个「Msixpkg 安装器」快捷方式。"},
    {L"Choosing \"for all users\" asks Windows for administrator approval when you press Install.",
     L"选择「为所有用户安装」后，点「安装」时 Windows 会请求管理员批准。"},
    {L"A per-machine installation already exists at {dir}. Updating it needs administrator approval.",
     L"这台机器上已有一份「为所有用户」的安装：{dir}。更新它需要管理员批准。"},
    {L"A per-user installation already exists at {dir}.", L"当前用户已有一份安装：{dir}。"},
    {L"Installing again updates the existing copy in place.", L"再次安装会就地更新现有的那一份。"},
    {L"Install", L"安装"},
    {L"Back", L"上一步"},
    {L"Next", L"下一步"},
    {L"Cancel", L"取消"},
    {L"Close", L"关闭"},
    {L"Installing Msixpkg Installer", L"正在安装 Msixpkg 安装器"},
    {L"Uninstalling Msixpkg Installer", L"正在卸载 Msixpkg 安装器"},
    {L"Waiting for administrator approval...", L"正在等待管理员批准…"},
    {L"Program files", L"程序文件"},
    {L"Shortcuts", L"快捷方式"},
    {L"Uninstall entry", L"卸载入口"},
    {L".msixpkg association", L".msixpkg 关联"},
    {L"PATH", L"PATH"},
    {L"Done", L"完成"},
    {L"Program files", L"程序文件"},
    {L"Shortcuts", L"快捷方式"},
    {L"Registry entries", L"注册表项"},
    {L"PATH", L"PATH"},
    {L"Cleanup", L"清理"},
    {L"Msixpkg Installer is ready", L"Msixpkg 安装器已就绪"},
    {L"Drop a .msixpkg file onto the wizard, or double-click one, to install an app.\r\n\r\n"
     L"Command line: msixpkg-native help",
     L"把 .msixpkg 文件拖进向导窗口，或双击它，即可开始安装应用。\r\n\r\n"
     L"命令行：msixpkg-native help"},
    {L"Installed, with warnings", L"已安装，但有警告"},
    {L"The program files are in place, but some entries could not be written. "
     L"See the log for details.",
     L"程序文件已经装好，但有些入口没能写入。详情见日志。"},
    {L"Installation did not complete", L"安装未完成"},
    {L"Installation cancelled", L"安装已取消"},
    {L"The installation was cancelled before it finished.",
     L"安装在中途被取消。"},
    {L"Run Msixpkg Installer", L"运行 Msixpkg 安装器"},
    {L"Open log", L"打开日志"},
    {L"No log file could be written.", L"无法写入日志文件。"},
    {L"Uninstall Msixpkg Installer?", L"卸载 Msixpkg 安装器？"},
    {L"This removes only the installer: program files, shortcuts, the .msixpkg "
     L"association, the PATH entry and the Apps & features entry.\r\n\r\n"
     L"Apps you installed are left alone. The installation records under "
     L"%ProgramData%\\Msixpkg are kept on purpose: they are what allows the publisher "
     L"certificate to be reclaimed later.",
     L"这里只会删除安装器本身：程序文件、快捷方式、.msixpkg 关联、PATH 条目与"
     L"「设置 → 应用」里的卸载入口。\r\n\r\n"
     L"你装过的应用不会被卸载。%ProgramData%\\Msixpkg 下的安装记录会**刻意保留** —— "
     L"那是以后回收发布者证书的依据。"},
    {L"Uninstall", L"卸载"},
    {L"Uninstalling...", L"正在卸载…"},
    {L"Msixpkg Installer was removed", L"Msixpkg 安装器已移除"},
    {L"The installer and its entries are gone. Installed apps and the certificate "
     L"bookkeeping were left untouched.",
     L"安装器与它的入口都已删除。已安装的应用与证书记账没有被改动。"},
    {L"Uninstall did not complete", L"卸载未完成"},
    {L"Installation records are kept at {state}.", L"安装记录保留在 {state}。"},
    {L"Administrator approval is required for this installation.",
     L"这次安装需要管理员批准。"},
    {L"Administrator approval was declined or unavailable.",
     L"管理员批准被拒绝，或无法请求提权。"},
    {L"A running Msixpkg Installer is holding the files to be replaced. "
     L"Close it and try again.",
     L"正在运行的 Msixpkg 安装器占用了要替换的文件。请先关闭它，再重试。"},
    {L"This setup cannot run on this machine: 64-bit Windows 10 or later is required.",
     L"这个安装程序无法在这台机器上运行：需要 64 位 Windows 10 或更高版本。"},
    {L"Msixpkg Installer", L"Msixpkg 安装器"},
    {L"Open a .msixpkg package and install the app inside",
     L"打开 .msixpkg 包并安装其中的应用"},
    {L"Manage installed apps", L"管理已安装的应用"},
    {L"Review what this installer installed, uninstall, repair advice",
     L"查看本安装器装过什么、卸载、修复建议"},
    {L"Uninstall Msixpkg Installer", L"卸载 Msixpkg 安装器"},
    {L"Remove the Msixpkg installer from this computer",
     L"从这台机器上移除 Msixpkg 安装器"},
    {L"Msixpkg package", L"Msixpkg 包"},
    {L"Open a .msixpkg package and install the app inside",
     L"打开 .msixpkg 包并安装其中的应用"},
    {L"The Msixpkg installer itself. Installed apps show up separately as "
     L"<app name> (Msixpkg) in Apps & features.",
     L"Msixpkg 安装器本体。已安装的应用会在「设置」里另有 <应用名>（Msixpkg）条目。"},
    {L"MsixpkgInstaller", L"MsixpkgInstaller"},
    {L"Options", L"安装选项"},
    {L"Install", L"正在安装"},
    {L"Finish", L"完成"},
    {L"Confirm", L"确认"},
    {L"The setup is still running. Please wait.", L"安装程序仍在运行，请稍候。"},
    {L"Msixpkg Installer Setup", L"Msixpkg 安装器 安装程序"},
    {L"Options:\r\n"
     L"  /S                  silent\r\n"
     L"  /D=<folder>         install folder\r\n"
     L"  /CURRENTUSER        per-user install\r\n"
     L"  /ALLUSERS           per-machine install (asks for UAC)\r\n"
     L"  /NOASSOC            skip the .msixpkg association\r\n"
     L"  /NOPATH             skip the PATH entry\r\n"
     L"  /DESKTOP            create a desktop shortcut\r\n"
     L"  /NODESKTOP          do not create a desktop shortcut (default)\r\n"
     L"  /NOELEVATE          never request elevation (tests)\r\n"
     L"  --verify-payload    check the embedded payload and exit\r\n"
     L"\r\n"
     L"Exit codes: 0 ok, 4 incomplete, 5 administrator needed, 6 precondition, 7 busy.",
     L"参数：\r\n"
     L"  /S                  静默安装\r\n"
     L"  /D=<目录>           指定安装目录\r\n"
     L"  /CURRENTUSER        仅当前用户\r\n"
     L"  /ALLUSERS           为所有用户（会请求 UAC）\r\n"
     L"  /NOASSOC            不注册 .msixpkg 关联\r\n"
     L"  /NOPATH             不改 PATH\r\n"
     L"  /DESKTOP            创建桌面快捷方式\r\n"
     L"  /NODESKTOP          不创建桌面快捷方式（默认）\r\n"
     L"  /NOELEVATE          绝不请求提权（测试用）\r\n"
     L"  --verify-payload    校验内嵌载荷后退出\r\n"
     L"\r\n"
     L"退出码：0 成功 / 4 未完成 / 5 需要管理员 / 6 前置条件 / 7 被占用。"},
    {L"The operation failed. See the log for details.", L"操作失败。详情见日志。"},
}};

static_assert(kEntries.size() == static_cast<size_t>(Str::UnknownFailure) + 1,
              "setup_strings 的条目数与 Str 枚举不一致");

}  // namespace

Strings::Strings() {
    const LANGID language = ::GetUserDefaultUILanguage();
    chinese_ = PRIMARYLANGID(language) == LANG_CHINESE;
}

size_t Strings::count() {
    return kEntries.size();
}

const wchar_t* Strings::text(Str id) const {
    const size_t index = static_cast<size_t>(id);
    if (index >= kEntries.size()) {
        return L"";
    }
    return chinese_ ? kEntries[index].zh : kEntries[index].en;
}

std::wstring Strings::format(
    Str id, std::initializer_list<std::pair<std::wstring_view, std::wstring>> replacements) const {
    std::wstring result = text(id);
    for (const auto& [token, value] : replacements) {
        size_t position = 0;
        while ((position = result.find(token, position)) != std::wstring::npos) {
            result.replace(position, token.size(), value);
            position += value.size();
        }
    }
    return result;
}

}  // namespace msixpkg::setup
