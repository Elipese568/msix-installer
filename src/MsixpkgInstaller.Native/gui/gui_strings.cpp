#include "gui_strings.h"

#include <cstdarg>
#include <cstdio>
#include <cwchar>
#include <vector>

namespace msixpkg::gui {
namespace {

/// 候选语言：用户首选 UI 语言 → 简体中文 → 美式英语。
/// 用「线程语言」而不是 FindResourceEx 的原因很实在：
/// RT_STRING 的块内偏移按**该块里第一条文案的序号**算，而 rc.exe 只把
/// STRINGTABLE 里出现过的 id 连续排进块 —— 用 (id-1)%16 去索引会整体错位
/// （而且是静默错位：FindResource 成功、取到的是邻居的文案）。
/// LoadStringW 走的是资源加载器的正规路径，不受这个影响。
constexpr WORD kSimplifiedChinese = MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_SIMPLIFIED);
constexpr WORD kEnglishUs = MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US);
constexpr LANGID kChineseLangId = MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_SIMPLIFIED);

/// 这个语言版本到底有没有字符串表：借 LoadStringW 探一条必然存在的文案。
/// 探测时把线程语言切成目标语言，读完立刻恢复 —— 这正是 LoadStringW
/// 「按调用线程语言选表」的用法。
bool language_has_strings(HINSTANCE module, WORD language, int probe_id) {
    const LANGID previous = ::GetThreadUILanguage();
    ::SetThreadUILanguage(language);

    wchar_t buffer[8] = {};
    const int written = ::LoadStringW(module, static_cast<UINT>(probe_id), buffer, 0);

    ::SetThreadUILanguage(previous);
    return written > 0;
}

}  // namespace

void Strings::load(void* instance) {
    instance_ = instance;
    missing_ = 0;

    const HINSTANCE module = static_cast<HINSTANCE>(instance);
    const WORD user = ::GetUserDefaultUILanguage();
    const WORD candidates[] = {user, kSimplifiedChinese, kEnglishUs};

    language_ = kEnglishUs;
    for (const WORD candidate : candidates) {
        if (language_has_strings(module, candidate, IDS_WINDOW_TITLE)) {
            language_ = candidate;
            break;
        }
    }

    // 逐条取出。用 LoadStringW 而不是手工解析 RT_STRING 块：见文件顶部的说明。
    // 切线程语言一次，把整张表都取出来，之后不再需要它。
    const LANGID previous = ::GetThreadUILanguage();
    ::SetThreadUILanguage(language_);

    for (int id = 1; id < MSIXPKG_STRING_SLOT_COUNT; ++id) {
        wchar_t buffer[2048] = {};
        // 首次调用探测长度（nBufferMax = 0 时返回整条长度），再真正取一次。
        const int length = ::LoadStringW(module, static_cast<UINT>(id), buffer, 0);
        if (length <= 0) {
            continue;
        }

        wchar_t text[2048] = {};
        const int written = ::LoadStringW(module, static_cast<UINT>(id), text, ARRAYSIZE(text));
        if (written > 0) {
            slots_[id].assign(text, static_cast<size_t>(written));
        }
    }

    ::SetThreadUILanguage(previous);

    // 自检：把「代码一定会用到」的编号数一遍，缺了就在 missing_count() 里体现。
    const int required[] = {
        IDS_WINDOW_TITLE,    IDS_COMMON_OK,      IDS_COMMON_CANCEL,  IDS_COMMON_BACK,
        IDS_COMMON_NEXT,     IDS_SUMMARY_TITLE,  IDS_CONSENT_TITLE,  IDS_CONSENT_ACCEPT,
        IDS_CONSENT_WARNING, IDS_PROGRESS_TITLE, IDS_RESULT_TITLE,   IDS_MANAGE_TITLE,
        IDS_MANAGE_UNINSTALL,
    };
    for (const int id : required) {
        if (slots_[id].empty()) {
            ++missing_;
        }
    }
}

std::wstring_view Strings::get(int id) const {
    if (id <= 0 || id >= MSIXPKG_STRING_SLOT_COUNT) {
        return L"";
    }

    if (!slots_[id].empty()) {
        return slots_[id];
    }

    // 不返回空串：空串会让界面出现「无标签的按钮」，而占位符一眼就能看出是资源缺失。
    static constexpr wchar_t kMissing[] = L"(!)";
    return kMissing;
}

std::wstring system_message(DWORD code) {
    LPWSTR buffer = nullptr;
    const DWORD written = ::FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        code,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPWSTR>(&buffer),
        0,
        nullptr);
    if (written == 0 || buffer == nullptr) {
        return L"Win32 error " + std::to_wstring(code);
    }

    std::wstring text(buffer, written);
    ::LocalFree(buffer);

    // FormatMessage 的文本尾部带 CRLF，塞进单行控件会顶出换行。
    while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n' || text.back() == L' ')) {
        text.pop_back();
    }

    return text;
}

std::wstring format_size(uint64_t bytes) {
    constexpr double kKiB = 1024.0;
    constexpr double kMiB = 1024.0 * 1024.0;
    constexpr double kGiB = 1024.0 * 1024.0 * 1024.0;

    wchar_t buffer[64] = {};
    const double value = static_cast<double>(bytes);
    if (value >= kGiB) {
        ::swprintf(buffer, ARRAYSIZE(buffer), L"%.2f GB", value / kGiB);
    } else if (value >= kMiB) {
        ::swprintf(buffer, ARRAYSIZE(buffer), L"%.2f MB", value / kMiB);
    } else if (value >= kKiB) {
        ::swprintf(buffer, ARRAYSIZE(buffer), L"%.1f KB", value / kKiB);
    } else {
        ::swprintf(buffer, ARRAYSIZE(buffer), L"%llu B", static_cast<unsigned long long>(bytes));
    }

    return buffer;
}

std::wstring format_fingerprint(std::wstring_view hex) {
    // 每字节一组、空格分开：人眼比对 64 个字符比比对一长串连续十六进制可靠得多。
    std::wstring result;
    result.reserve(hex.size() + hex.size() / 2);
    for (size_t index = 0; index < hex.size(); ++index) {
        if (index != 0 && index % 2 == 0) {
            result.push_back(L' ');
        }

        result.push_back(hex[index]);
    }

    return result;
}

std::wstring format(const wchar_t* pattern, ...) {
    // 先用 _vscwprintf 量长度再格式化：vswprintf_s 在缓冲区不够时会直接失败
    // （返回 -1），那样长文案会被静默吞掉。量一次长度再分配是唯一稳妥的写法。
    va_list arguments;
    va_start(arguments, pattern);
    const int needed = ::_vscwprintf(pattern, arguments);
    va_end(arguments);

    if (needed < 0) {
        return {};
    }

    std::wstring result(static_cast<size_t>(needed), L'\0');
    va_start(arguments, pattern);
    ::_vsnwprintf_s(result.data(), result.size() + 1, _TRUNCATE, pattern, arguments);
    va_end(arguments);
    return result;
}

}  // namespace msixpkg::gui
