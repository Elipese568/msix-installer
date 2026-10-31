#include <Windows.h>
#include <shellapi.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "setup_options.h"
#include "setup_path.h"
#include "setup_payload.h"
#include "setup_sha256.h"
#include "setup_strings.h"
#include "setup_util.h"
#include "test_harness.h"

namespace {

using msixpkg::setup::InstallMode;
using msixpkg::setup::Options;
using msixpkg::setup::Payload;
using msixpkg::setup::Str;
using msixpkg::setup::Strings;

std::wstring add(const std::wstring& value, const std::wstring& entry, bool expected_changed) {
    bool changed = false;
    const std::wstring result = msixpkg::setup::path_add(value, entry, changed);
    CHECK(changed == expected_changed);
    return result;
}

std::vector<uint8_t> make_payload_blob(const std::vector<std::pair<std::string, std::string>>& files) {
    // 与 scripts/build-setup.ps1 相同的布局：[prefix][entries][index][footer]。
    std::vector<uint8_t> blob;
    const std::string prefix = "thin-exe-prefix";
    blob.insert(blob.end(), prefix.begin(), prefix.end());

    struct Row {
        std::string name;
        uint64_t offset;
        uint64_t size;
        std::array<uint8_t, 32> hash;
    };
    std::vector<Row> rows;
    for (const auto& [name, data] : files) {
        Row row;
        row.name = name;
        row.offset = blob.size();
        row.size = data.size();
        blob.insert(blob.end(), data.begin(), data.end());
        row.hash = msixpkg::setup::sha256_of(data.data(), data.size());
        rows.push_back(row);
    }

    std::vector<uint8_t> index;
    const auto append_u16 = [&](uint16_t value) {
        index.push_back(static_cast<uint8_t>(value & 0xFF));
        index.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    };
    const auto append_u32 = [&](uint32_t value) {
        for (int shift = 0; shift < 32; shift += 8) {
            index.push_back(static_cast<uint8_t>((value >> shift) & 0xFF));
        }
    };
    const auto append_u64 = [&](uint64_t value) {
        for (int shift = 0; shift < 64; shift += 8) {
            index.push_back(static_cast<uint8_t>((value >> shift) & 0xFF));
        }
    };

    append_u32(static_cast<uint32_t>(rows.size()));
    for (const Row& row : rows) {
        append_u16(static_cast<uint16_t>(row.name.size()));
        index.insert(index.end(), row.name.begin(), row.name.end());
        append_u64(row.offset);
        append_u64(row.size);
        index.insert(index.end(), row.hash.begin(), row.hash.end());
    }

    const uint64_t index_offset = blob.size();
    blob.insert(blob.end(), index.begin(), index.end());
    const auto index_hash = msixpkg::setup::sha256_of(index.data(), index.size());
    const uint64_t payload_size = index_offset - prefix.size();

    const auto append_blob_u32 = [&](uint32_t value) {
        for (int shift = 0; shift < 32; shift += 8) {
            blob.push_back(static_cast<uint8_t>((value >> shift) & 0xFF));
        }
    };
    const auto append_blob_u64 = [&](uint64_t value) {
        for (int shift = 0; shift < 64; shift += 8) {
            blob.push_back(static_cast<uint8_t>((value >> shift) & 0xFF));
        }
    };

    const char magic[8] = {'M', 'S', 'X', 'S', 'E', 'T', 'U', 'P'};
    blob.insert(blob.end(), magic, magic + sizeof(magic));
    append_blob_u32(1);
    append_blob_u32(0);
    append_blob_u64(index_offset);
    append_blob_u64(index.size());
    append_blob_u64(payload_size);
    append_blob_u64(0);
    blob.insert(blob.end(), index_hash.begin(), index_hash.end());
    return blob;
}

std::filesystem::path temp_file(const wchar_t* name) {
    wchar_t buffer[MAX_PATH]{};
    ::GetTempPathW(MAX_PATH, buffer);
    return std::filesystem::path(buffer) / name;
}

}  // namespace

MSIXPKG_TEST(setup_path_split_and_join_preserve_empty_segments) {
    const std::wstring original = L"C:\\one;;C:\\two;";
    const auto segments = msixpkg::setup::split_path(original);
    CHECK(segments.size() == 4);
    CHECK(segments[0] == L"C:\\one");
    CHECK(segments[1].empty());
    CHECK(segments[2] == L"C:\\two");
    CHECK(segments[3].empty());
    CHECK(msixpkg::setup::join_path(segments) == original);

    CHECK(msixpkg::setup::split_path(L"").size() == 1);
    CHECK(msixpkg::setup::join_path(msixpkg::setup::split_path(L"")) == L"");
}

MSIXPKG_TEST(setup_path_add_is_segment_exact) {
    CHECK(add(L"", L"C:\\tools\\msixpkg", true) == L"C:\\tools\\msixpkg");
    CHECK(add(L"C:\\a", L"C:\\a", false) == L"C:\\a");
    CHECK(add(L"C:\\A", L"c:\\a", false) == L"C:\\A");
    CHECK(add(L"C:\\a\\", L"C:\\a", false) == L"C:\\a\\");
    CHECK(add(L"\"C:\\a\"", L"C:\\a", false) == L"\"C:\\a\"");

    // 前缀陷阱：C:\a 不能匹配 C:\ab。
    CHECK(add(L"C:\\ab", L"C:\\a", true) == L"C:\\ab;C:\\a");
    // 尾部空段必须保留。
    CHECK(add(L"C:\\a;", L"C:\\b", true) == L"C:\\a;;C:\\b");
    CHECK(add(L"C:\\a;;C:\\b", L"C:\\c", true) == L"C:\\a;;C:\\b;C:\\c");
}

MSIXPKG_TEST(setup_path_remove_removes_all_copies) {
    bool changed = false;
    const std::wstring value = L"C:\\a;;C:\\tools;c:\\TOOLS;C:\\tools\\;C:\\ab";
    const std::wstring removed = msixpkg::setup::path_remove(value, L"C:\\tools", changed);
    CHECK(changed);
    CHECK(removed == L"C:\\a;;C:\\ab");
    CHECK(msixpkg::setup::path_remove(L"C:\\ab", L"C:\\a", changed) == L"C:\\ab");
    CHECK(!changed);
    CHECK(msixpkg::setup::path_remove(L"C:\\a", L"C:\\a", changed).empty());
    CHECK(changed);
}

MSIXPKG_TEST(setup_options_accept_legacy_switches) {
    const wchar_t* argv[] = {L"setup.exe", L"/S", L"/CURRENTUSER", L"/D=C:\\Program Files\\Msixpkg",
                             L"/NOASSOC", L"/NOPATH", L"/NODESKTOP"};
    const auto parsed = msixpkg::setup::parse_arguments(7, argv);
    CHECK(parsed.ok);
    CHECK(parsed.options.silent);
    CHECK(parsed.options.mode == InstallMode::CurrentUser);
    CHECK(parsed.options.install_dir == std::filesystem::path(L"C:\\Program Files\\Msixpkg"));
    CHECK(parsed.options.install_dir_explicit);
    CHECK(parsed.options.no_assoc);
    CHECK(parsed.options.no_path);
    CHECK(!parsed.options.desktop);
}

MSIXPKG_TEST(setup_options_reject_conflicting_modes_and_unknown_arguments) {
    const wchar_t* conflict[] = {L"setup.exe", L"/ALLUSERS", L"/CURRENTUSER"};
    CHECK(!msixpkg::setup::parse_arguments(3, conflict).ok);

    const wchar_t* unknown[] = {L"setup.exe", L"/WHAT"};
    CHECK(!msixpkg::setup::parse_arguments(2, unknown).ok);

    const wchar_t* missing[] = {L"setup.exe", L"--ui-page"};
    CHECK(!msixpkg::setup::parse_arguments(2, missing).ok);
}

MSIXPKG_TEST(setup_options_diagnostics_and_elevation_arguments) {
    const wchar_t* argv[] = {L"setup.exe", L"--ui-page", L"progress", L"--ui-progress", L"42",
                             L"--ui-theme", L"dark", L"/DESKTOP"};
    const auto parsed = msixpkg::setup::parse_arguments(8, argv);
    CHECK(parsed.ok);
    CHECK(parsed.options.ui_page == msixpkg::setup::UiPage::Progress);
    CHECK(parsed.options.ui_percent == 42);
    CHECK(parsed.options.ui_theme == L"dark");
    CHECK(parsed.options.desktop);

    Options options;
    options.mode = InstallMode::AllUsers;
    options.install_dir = L"C:\\Program Files\\Msixpkg";
    const auto arguments = msixpkg::setup::elevation_arguments(options);
    bool has_elevated = false;
    bool has_all_users = false;
    for (const std::wstring& argument : arguments) {
        if (argument == L"/ELEVATED") {
            has_elevated = true;
        }
        if (argument == L"/ALLUSERS") {
            has_all_users = true;
        }
    }
    CHECK(has_elevated);
    CHECK(has_all_users);
}

MSIXPKG_TEST(setup_strings_are_complete_in_the_active_language) {
    const Strings strings;
    const size_t count = Strings::count();
    CHECK(count > 50);
    for (size_t index = 0; index < count; ++index) {
        const wchar_t* text = strings.text(static_cast<Str>(index));
        CHECK(text != nullptr);
        CHECK(text[0] != L'\0');
    }
}

MSIXPKG_TEST(setup_payload_round_trips_and_detects_corruption) {
    const auto path = temp_file(L"msixpkg-setup-payload-test.exe");
    const std::vector<std::pair<std::string, std::string>> files = {
        {"first.bin", "hello"},
        {"second.bin", std::string(5000, 'x')},
    };
    const auto blob = make_payload_blob(files);
    std::wstring error;
    CHECK(msixpkg::setup::write_file_bytes(path, blob.data(), blob.size(), error));

    {
        Payload payload;
        CHECK(payload.open(path, error));
        CHECK(payload.entries().size() == 2);
        CHECK(payload.find("first.bin") != nullptr);
        CHECK(payload.find("missing.bin") == nullptr);
        std::vector<std::wstring> report;
        CHECK(payload.verify_all(report, error));
        CHECK(report.size() == 2);

        const std::filesystem::path extracted = path.wstring() + L".first.bin";
        CHECK(payload.extract_to(*payload.find("first.bin"), extracted, error));
        std::string content;
        {
            std::ifstream stream(extracted, std::ios::binary);
            content.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
        }
        CHECK(content == "hello");
        std::error_code code;
        std::filesystem::remove(extracted, code);
    }

    // Authenticode appends a certificate table after the footer: the reader must find
    // the footer before the extra bytes instead of assuming it is at EOF-80.
    {
        auto signed_blob = blob;
        signed_blob.insert(signed_blob.end(), 4096, 0xA5);
        CHECK(msixpkg::setup::write_file_bytes(path, signed_blob.data(), signed_blob.size(), error));
        Payload payload;
        CHECK(payload.open(path, error));
        std::vector<std::wstring> report;
        CHECK(payload.verify_all(report, error));
    }
    CHECK(msixpkg::setup::write_file_bytes(path, blob.data(), blob.size(), error));

    // Flip one payload byte: the index still opens, but verification must fail.
    {
        std::fstream stream(path, std::ios::in | std::ios::out | std::ios::binary);
        stream.seekp(static_cast<std::streamoff>(std::string("thin-exe-prefix").size()));
        const char changed = 'j';
        stream.write(&changed, 1);
        stream.close();
    }
    {
        Payload payload;
        CHECK(payload.open(path, error));
        std::vector<std::wstring> report;
        CHECK(!payload.verify_all(report, error));
    }

    std::error_code code;
    std::filesystem::remove(path, code);
}

MSIXPKG_TEST(setup_quote_argument_round_trips_through_command_line_to_argv) {
    const std::vector<std::wstring> values = {
        L"C:\\simple", L"C:\\with space\\x", L"a\"b", L"", L"trailing\\", L"back\\\\slash",
    };
    const std::wstring command_line = L"setup.exe " + msixpkg::setup::join_arguments(values);
    int argc = 0;
    LPWSTR* argv = ::CommandLineToArgvW(command_line.c_str(), &argc);
    CHECK(argv != nullptr);
    if (argv != nullptr) {
        CHECK(argc == static_cast<int>(values.size()) + 1);
        for (size_t index = 0; index < values.size() && static_cast<int>(index + 1) < argc; ++index) {
            CHECK(std::wstring(argv[index + 1]) == values[index]);
        }
        ::LocalFree(argv);
    }
}
