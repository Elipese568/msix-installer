// 自绘 UI 框架 demo（docs/self-drawn-ui-framework.md §8.1 的 spike 产物）。
//
// 这个程序不是安装器，而是把框架的每条路径都走一遍：
//   - 720x480 之外的固定尺寸（760x620）+ 绝对布局；
//   - Label / Button / Checkbox / ProgressBar / TextView / Table 全部控件；
//   - Tab 焦点链、Enter 默认按钮、Esc 关闭、Space 切换、表格方向键、Ctrl+A/C；
//   - 浅色/深色切换、Mica/Acrylic/Solid 材质切换、DPI 跟随；
//   - --report：通过 OBJID_CLIENT 真正走一遍 WM_GETOBJECT + MSAA，把子对象
//     的数量/名字/角色/状态写进文件，用于无人值守验证。

#include "controls.h"
#include "host.h"
#include "theme.h"

#include <Windows.h>
#include <oleacc.h>
#include <shellapi.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace msixpkg::ui;

std::string to_utf8(const std::wstring& value) {
    if (value.empty()) {
        return {};
    }

    const int size = ::WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0,
                                           nullptr, nullptr);
    if (size <= 0) {
        return {};
    }

    std::string result(static_cast<size_t>(size), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), result.data(), size, nullptr,
                          nullptr);
    return result;
}

const wchar_t* appearance_name(Appearance appearance) {
    switch (appearance) {
        case Appearance::Dark: return L"Dark";
        case Appearance::HighContrast: return L"HighContrast";
        case Appearance::Auto: return L"Auto";
        case Appearance::Light: return L"Light";
    }

    return L"Light";
}

std::wstring details_text() {
    return L"Subject:     CN=Contoso Software Ltd, O=Contoso Software Ltd, C=CN\r\n"
           L"Issuer:      CN=Contoso Software Ltd\r\n"
           L"Thumbprint:  8F3A 91C2 0D4E 77B1 2C6A 5E90 11D8 4A3F 6B22 9C04\r\n"
           L"SHA-256:     3E7D 04A1 9B22 77C0 5D18 2E46 8A91 0C3B 6F52 1188 4A0D 2B77 93E1 58FA 0C6D 71A2\r\n"
           L"Valid:       2026-09-01 00:00:00 UTC 至 2027-09-01 00:00:00 UTC\r\n"
           L"\r\n"
           L"同意页的证书详情必须是可聚焦、可滚动的只读文本：Tab 能进来，Ctrl+A 全选，"
           L"Ctrl+C 复制全文，Narrator 能读出 get_accValue。这一页是全自绘改造里无障碍的"
           L"硬要求，所以这一块故意给了超过可视区域的行数，用来验证换行与滚动。\r\n"
           L"\r\n"
           L"下面重复一段较长的英文指纹，用于观察拉丁文按词换行与 CJK 逐字断行的差异：\r\n"
           L"3E7D04A19B2277C05D182E468A910C3B6F5211884A0D2B7793E158FA0C6D71A2\r\n"
           L"如果这一行折行位置和上方行一致，说明 TextView 的测量与绘制用的是同一份行模型。";
}

// 演示用卡片：框架本身不提供 Card，这里用 10 行自定义 Widget 说明它是可扩展的。
class Card : public Panel {
public:
    void paint(Canvas& canvas) override {
        const Theme& current = theme();
        canvas.fill_round_rect(bounds, current.radius_card, current.card);
        canvas.stroke_round_rect(bounds, current.radius_card, current.card_border, 1.0f);
        Panel::paint(canvas);
    }
};

struct DemoApp {
    Host* host = nullptr;
    Label* badge = nullptr;
    Label* status = nullptr;
    Label* percent = nullptr;
    TextView* details = nullptr;
    Table* table = nullptr;
    Checkbox* consent = nullptr;
    Button* start = nullptr;
    Button* theme_button = nullptr;
    Button* material_button = nullptr;
    ProgressBar* progress = nullptr;

    int progress_value = 0;
    bool installing = false;
    bool auto_start = false;
    bool auto_started = false;
    ULONGLONG created_at = 0;
    ULONGLONG auto_close_at = 0;
    ULONGLONG last_tick = 0;

    void refresh_badge() {
        if (host == nullptr || badge == nullptr) {
            return;
        }

        const int dpi = static_cast<int>(host->dpi() + 0.5f);
        badge->set_text(std::wstring(appearance_name(host->appearance())) + L" · " +
                        material_name(host->material_kind()) + L" · " + std::to_wstring(dpi) + L"% · " +
                        host->presenter_name());
    }

    void set_status(std::wstring text) {
        if (status != nullptr) {
            status->set_text(std::move(text));
        }
    }

    void begin_install() {
        if (installing) {
            return;
        }

        installing = true;
        progress_value = 0;
        if (progress != nullptr) {
            progress->set_value(0);
        }

        if (percent != nullptr) {
            percent->set_text(L"0%");
        }

        if (start != nullptr) {
            start->enabled = false;
            start->invalidate();
        }

        set_status(L"正在校验容器 …");
    }

    void finish_install() {
        installing = false;
        if (start != nullptr) {
            start->enabled = true;
            start->invalidate();
        }

        set_status(L"演示安装完成。可以按「重置」再来一次。");
    }

    void reset() {
        installing = false;
        progress_value = 0;
        if (progress != nullptr) {
            progress->set_value(0);
        }

        if (percent != nullptr) {
            percent->set_text(L"0%");
        }

        if (start != nullptr) {
            start->enabled = true;
            start->invalidate();
        }

        set_status(L"就绪。按「开始演示安装」或 Enter。");
    }

    void cycle_theme() {
        if (host == nullptr || theme_button == nullptr) {
            return;
        }

        const bool dark_now = host->appearance() == Appearance::Dark;
        host->set_appearance(dark_now ? Appearance::Light : Appearance::Dark);
        theme_button->text = host->appearance() == Appearance::Dark ? L"切换到浅色" : L"切换到深色";
        theme_button->invalidate();
        refresh_badge();
    }

    void cycle_material() {
        if (host == nullptr || material_button == nullptr) {
            return;
        }

        const bool solid_now = host->material_preference() == MaterialPreference::Solid;
        host->set_material(solid_now ? MaterialPreference::Auto : MaterialPreference::Solid);
        material_button->text = solid_now ? L"材质：自动" : L"材质：纯色";
        material_button->invalidate();
        refresh_badge();
    }

    void tick() {
        const ULONGLONG now = ::GetTickCount64();
        const float dt = last_tick == 0 ? 0.0f : static_cast<float>(now - last_tick) / 1000.0f;
        last_tick = now;

        if (auto_start && !auto_started) {
            // 给窗口一点时间上屏，截图/人眼都能看到初始状态。
            if (now - created_at > 700) {
                auto_started = true;
                begin_install();
            }
        }

        if (installing) {
            progress_value = std::min(100, progress_value + static_cast<int>(dt * 26.0f));
            if (progress != nullptr) {
                progress->set_value(progress_value);
            }

            if (percent != nullptr) {
                percent->set_text(std::to_wstring(progress_value) + L"%");
            }

            if (progress_value < 20) {
                set_status(L"正在校验容器与签名 …");
            } else if (progress_value < 40) {
                set_status(L"正在导入发布者证书 …");
            } else if (progress_value < 85) {
                set_status(L"正在安装 MSIX 包 …");
            } else if (progress_value < 100) {
                set_status(L"正在回收旧版本 …");
            } else {
                finish_install();
            }
        }

        if (auto_close_at != 0 && now >= auto_close_at && host != nullptr) {
            host->close();
        }
    }
};

void write_report(Host& host, const std::wstring& path) {
    std::wstring content;
    const auto line = [&content](const std::wstring& text) {
        content += text;
        content += L"\n";
    };

    line(L"Msixpkg self-drawn UI demo report");
    line(L"appearance=" + std::wstring(appearance_name(host.appearance())));
    line(L"material=" + std::wstring(material_name(host.material_kind())));
    line(L"presenter=" + std::wstring(host.presenter_name()));
    line(L"dpi=" + std::to_wstring(static_cast<int>(host.dpi() + 0.5f)));
    line(L"client=" + std::to_wstring(host.client_width_px()) + L"x" + std::to_wstring(host.client_height_px()));
    line(L"ui.entries=" + std::to_wstring(host.accessible_entries().size()));
    line(L"wm_getobject=" + std::to_wstring(host.accessibility_request_count()));

    wchar_t title[256]{};
    ::GetWindowTextW(host.hwnd(), title, static_cast<int>(std::size(title)));
    RECT window_rect{};
    ::GetWindowRect(host.hwnd(), &window_rect);
    line(std::wstring(L"window.title=") + title);
    line(L"window.visible=" + std::to_wstring(::IsWindowVisible(host.hwnd()) ? 1 : 0));
    line(L"window.rect=" + std::to_wstring(window_rect.left) + L"," + std::to_wstring(window_rect.top) + L" " +
         std::to_wstring(window_rect.right - window_rect.left) + L"x" +
         std::to_wstring(window_rect.bottom - window_rect.top));

    IAccessible* root = nullptr;
    const HRESULT hr = ::AccessibleObjectFromWindow(host.hwnd(), static_cast<DWORD>(OBJID_CLIENT), IID_IAccessible,
                                                    reinterpret_cast<void**>(&root));
    line(L"msaa.hr=" + std::to_wstring(static_cast<long>(hr)));
    if (SUCCEEDED(hr) && root != nullptr) {
        long count = 0;
        root->get_accChildCount(&count);
        line(L"msaa.children=" + std::to_wstring(count));

        const long limit = std::min<long>(count, 24);
        for (long index = 1; index <= limit; ++index) {
            VARIANT child_id{};
            child_id.vt = VT_I4;
            child_id.lVal = index;

            IDispatch* dispatch = nullptr;
            if (FAILED(root->get_accChild(child_id, &dispatch)) || dispatch == nullptr) {
                line(L"child[" + std::to_wstring(index) + L"]=<unavailable>");
                continue;
            }

            IAccessible* child = nullptr;
            dispatch->QueryInterface(IID_IAccessible, reinterpret_cast<void**>(&child));

            std::wstring name;
            long role = 0;
            long state = 0;
            if (child != nullptr) {
                VARIANT self{};
                self.vt = VT_I4;
                self.lVal = CHILDID_SELF;

                BSTR value = nullptr;
                if (SUCCEEDED(child->get_accName(self, &value)) && value != nullptr) {
                    name = value;
                    ::SysFreeString(value);
                }

                VARIANT role_value{};
                if (SUCCEEDED(child->get_accRole(self, &role_value)) && role_value.vt == VT_I4) {
                    role = role_value.lVal;
                }

                VARIANT state_value{};
                if (SUCCEEDED(child->get_accState(self, &state_value)) && state_value.vt == VT_I4) {
                    state = state_value.lVal;
                }

                child->Release();
            }

            dispatch->Release();
            line(L"child[" + std::to_wstring(index) + L"] role=" + std::to_wstring(role) +
                 L" state=" + std::to_wstring(state) + L" name=" + name);
        }

        root->Release();
    }

    line(L"wm_getobject_after=" + std::to_wstring(host.accessibility_request_count()));

    std::ofstream stream(std::filesystem::path(path), std::ios::binary | std::ios::trunc);
    if (stream.is_open()) {
        const std::string utf8 = to_utf8(content);
        stream.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
    }
}

struct Settings {
    std::wstring report_path;
    std::wstring theme = L"auto";
    std::wstring material = L"auto";
    bool auto_start = false;
    ULONGLONG exit_after_ms = 0;
};

Settings parse_settings() {
    Settings settings;
    int argc = 0;
    LPWSTR* argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);
    if (argv == nullptr) {
        return settings;
    }

    for (int index = 1; index < argc; ++index) {
        const std::wstring_view token(argv[index]);
        const auto next = [&](std::wstring& out) {
            if (index + 1 < argc) {
                out = argv[++index];
            }
        };

        if (token == L"--report") {
            next(settings.report_path);
        } else if (token == L"--theme") {
            next(settings.theme);
        } else if (token == L"--material") {
            next(settings.material);
        } else if (token == L"--auto") {
            settings.auto_start = true;
        } else if (token == L"--exit-after") {
            std::wstring value;
            next(value);
            settings.exit_after_ms = static_cast<ULONGLONG>(_wtoi64(value.c_str()));
        }
    }

    ::LocalFree(argv);
    return settings;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    const Settings settings = parse_settings();

    const Theme base = make_theme(Appearance::Light, false);
    auto root = std::make_unique<Panel>();
    root->bounds = {0.0f, 0.0f, 760.0f, 620.0f};

    DemoApp app;

    // ---- 顶部标题 ----
    auto* title = root->emplace<Label>(L"全自绘 UI 框架", base.title);
    title->bounds = {18.0f, 12.0f, 500.0f, 32.0f};

    auto* subtitle = root->emplace<Label>(L"GDI+ · 单 HWND · Mica / Acrylic / Solid · MSAA", base.caption);
    subtitle->bounds = {18.0f, 46.0f, 520.0f, 18.0f};
    subtitle->set_tone(LabelTone::Secondary);

    app.badge = root->emplace<Label>(L"", base.caption);
    app.badge->bounds = {500.0f, 16.0f, 242.0f, 20.0f};
    app.badge->set_tone(LabelTone::Secondary);
    app.badge->set_align(TextAlign::Right);
    app.badge->set_ellipsis(true);

    // ---- 左卡片：摘要 ----
    auto* summary = root->emplace<Card>();
    summary->bounds = {18.0f, 78.0f, 352.0f, 188.0f};

    auto* summary_title = summary->emplace<Label>(L"示例包", base.heading);
    summary_title->bounds = {34.0f, 92.0f, 320.0f, 22.0f};

    struct Row {
        const wchar_t* label;
        const wchar_t* value;
    };

    const Row rows[] = {
        {L"名称", L"Contoso.Notes"},
        {L"版本", L"1.2.3.0 · x64"},
        {L"发布者", L"CN=Contoso Software Ltd"},
        {L"证书", L"8F3A 91C2 0D4E 77B1 …"},
    };

    for (int index = 0; index < 4; ++index) {
        auto* key = summary->emplace<Label>(rows[index].label, base.body);
        key->bounds = {34.0f, 126.0f + static_cast<float>(index) * 24.0f, 76.0f, 18.0f};
        key->set_tone(LabelTone::Secondary);
        key->set_align(TextAlign::Right);

        auto* value = summary->emplace<Label>(rows[index].value, base.body);
        value->bounds = {122.0f, 126.0f + static_cast<float>(index) * 24.0f, 234.0f, 18.0f};
        value->set_ellipsis(true);
    }

    // ---- 右卡片：Table ----
    auto* table_card = root->emplace<Card>();
    table_card->bounds = {386.0f, 78.0f, 356.0f, 188.0f};

    auto* table_caption = table_card->emplace<Label>(L"已安装的包（Table：方向键 / 单击选择）", base.caption);
    table_caption->bounds = {400.0f, 90.0f, 328.0f, 16.0f};
    table_caption->set_tone(LabelTone::Secondary);

    app.table = table_card->emplace<Table>();
    app.table->bounds = {400.0f, 112.0f, 328.0f, 140.0f};
    app.table->text = L"已安装的包";
    app.table->set_columns({
        {L"名称", 120.0f},
        {L"版本", 66.0f},
        {L"状态", 74.0f},
        {L"证书", 64.0f},
    });
    app.table->set_rows({
        {L"Contoso.Notes", L"1.2.3", L"已安装", L"8F3A"},
        {L"Contoso.Notes", L"1.2.2", L"已安装", L"1C7D"},
        {L"Contoso.Calc", L"2.0.0", L"缺失", L"9B24"},
        {L"Contoso.Files", L"3.1.4", L"已安装", L"44E0"},
        {L"Contoso.Camera", L"0.9.1", L"部分", L"7A18"},
        {L"Contoso.Music", L"5.0.2", L"已安装", L"2D6F"},
        {L"Contoso.Studio", L"1.0.0", L"缺失", L"B013"},
        {L"Contoso.Term", L"4.4.4", L"已安装", L"F5C9"},
    });
    app.table->set_selected_row(0, false);

    // ---- 证书详情 ----
    auto* details_caption = root->emplace<Label>(L"证书详情（TextView：滚轮/滚动条、Tab 聚焦、Ctrl+A/C 复制）", base.caption);
    details_caption->bounds = {18.0f, 280.0f, 724.0f, 16.0f};
    details_caption->set_tone(LabelTone::Secondary);

    app.details = root->emplace<TextView>(details_text(), base.mono);
    app.details->bounds = {18.0f, 298.0f, 724.0f, 128.0f};

    // ---- 同意 + 进度 ----
    app.consent = root->emplace<Checkbox>(L"我了解并同意导入上述发布者证书（演示不会真的操作证书）");
    app.consent->bounds = {18.0f, 438.0f, 640.0f, 22.0f};
    app.consent->set_checked(true);

    app.progress = root->emplace<ProgressBar>();
    app.progress->bounds = {18.0f, 470.0f, 600.0f, 16.0f};

    app.percent = root->emplace<Label>(L"0%", base.body);
    app.percent->bounds = {630.0f, 466.0f, 112.0f, 22.0f};
    app.percent->set_align(TextAlign::Right);
    app.percent->set_valign(TextVAlign::Center);
    app.percent->set_tone(LabelTone::Secondary);

    app.status = root->emplace<Label>(L"就绪。按「开始演示安装」或 Enter。", base.body);
    app.status->bounds = {18.0f, 496.0f, 724.0f, 20.0f};
    app.status->set_tone(LabelTone::Secondary);

    auto* hint = root->emplace<Label>(
        L"Tab / Shift+Tab 切换焦点 · Enter 触发默认按钮 · Space 切换复选框 · Esc 退出 · Ctrl+A/C 复制详情",
        base.caption);
    hint->bounds = {18.0f, 522.0f, 724.0f, 16.0f};
    hint->set_tone(LabelTone::Secondary);

    // ---- 底部按钮 ----
    app.theme_button = root->emplace<Button>(L"切换到深色");
    app.theme_button->bounds = {18.0f, 566.0f, 110.0f, 30.0f};
    app.theme_button->set_on_click([&app] { app.cycle_theme(); });

    app.material_button = root->emplace<Button>(L"材质：自动");
    app.material_button->bounds = {136.0f, 566.0f, 110.0f, 30.0f};
    app.material_button->set_on_click([&app] { app.cycle_material(); });

    app.start = root->emplace<Button>(L"开始演示安装");
    app.start->bounds = {254.0f, 566.0f, 130.0f, 30.0f};
    app.start->set_primary(true);
    app.start->set_default(true);
    app.start->set_icon(IconId::Download);
    app.start->set_on_click([&app] { app.begin_install(); });

    auto* reset = root->emplace<Button>(L"重置");
    reset->bounds = {392.0f, 566.0f, 90.0f, 30.0f};
    reset->set_on_click([&app] { app.reset(); });

    auto* exit_button = root->emplace<Button>(L"退出");
    exit_button->bounds = {652.0f, 566.0f, 90.0f, 30.0f};
    exit_button->set_on_click([&app] { app.host->close(); });

    // ---- Host ----
    HostOptions options;
    options.title = L"Msixpkg 自绘 UI 框架 · Demo";
    options.window_class = L"MsixpkgSelfDrawnDemoWindow";
    options.width_dip = 760;
    options.height_dip = 620;
    options.material = MaterialPreference::Auto;
    if (settings.theme == L"light") {
        options.appearance = Appearance::Light;
    } else if (settings.theme == L"dark") {
        options.appearance = Appearance::Dark;
    } else if (settings.theme == L"hc") {
        options.appearance = Appearance::HighContrast;
    }

    if (settings.material == L"solid") {
        options.material = MaterialPreference::Solid;
    } else if (settings.material == L"mica") {
        options.material = MaterialPreference::Mica;
    } else if (settings.material == L"acrylic") {
        options.material = MaterialPreference::Acrylic;
    }

    options.on_escape = [&app] { app.host->close(); };

    if (settings.material == L"solid") {
        app.material_button->text = L"材质：纯色";
    }

    app.host = nullptr;
    Host host;
    app.host = &host;

    if (!host.create(instance, std::move(root), options)) {
        ::MessageBoxW(nullptr, L"窗口创建失败。", L"自绘 UI Demo", MB_OK | MB_ICONERROR);
        return 1;
    }

    app.auto_start = settings.auto_start;
    app.auto_started = false;
    app.created_at = ::GetTickCount64();
    app.last_tick = app.created_at;
    app.refresh_badge();

    if (host.appearance() == Appearance::Dark) {
        app.theme_button->text = L"切换到浅色";
    }

    app.table->set_on_selection_changed([&app](int row) {
        if (row >= 0) {
            app.set_status(L"表格选中：" + app.table->row_text(row));
        }
    });

    app.consent->set_on_changed([&app](bool checked) {
        app.set_status(checked ? L"已同意导入证书。" : L"未同意：安装按钮应当被禁用（演示未强制）。");
    });

    host.set_tick_handler([&app] { app.tick(); });
    host.set_animating(true, 33);

    if (!settings.report_path.empty()) {
        write_report(host, settings.report_path);
    }

    if (settings.auto_start || settings.exit_after_ms > 0) {
        // 不引入额外线程：tick 里按时间决定自动开始/退出。
        app.last_tick = ::GetTickCount64();
        if (settings.exit_after_ms > 0) {
            app.auto_close_at = ::GetTickCount64() + settings.exit_after_ms;
        }
    }

    MSG message{};
    while (::GetMessageW(&message, nullptr, 0, 0) > 0) {
        ::TranslateMessage(&message);
        ::DispatchMessageW(&message);
    }

    host.set_animating(false);
    return 0;
}
