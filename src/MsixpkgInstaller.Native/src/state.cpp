#include "state.h"

#include <Windows.h>
#include <Aclapi.h>
#include <bcrypt.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <utility>

#include "internal.h"
#include "json.h"
#include "msixpkg/errors.h"

namespace msixpkg {
namespace {

// 前向声明：下面的目录解析需要先用到它。
std::wstring environment_variable(const wchar_t* name);

std::string get_string(const json::Value& object, const char* name) {
    const json::Value* value = object.find(name);
    if (value == nullptr || value->kind != json::Value::Kind::String) {
        return {};
    }

    return value->text;
}

bool get_bool(const json::Value& object, const char* name) {
    const json::Value* value = object.find(name);
    return value != nullptr && value->kind == json::Value::Kind::Boolean && value->boolean;
}

std::vector<std::string> get_string_array(const json::Value& object, const char* name) {
    std::vector<std::string> result;
    const json::Value* value = object.find(name);
    if (value == nullptr || value->kind != json::Value::Kind::Array) {
        return result;
    }

    for (const json::Value& item : value->items) {
        if (item.kind == json::Value::Kind::String) {
            result.push_back(item.text);
        }
    }

    return result;
}

json::Value string_array(const std::vector<std::string>& values) {
    json::Value array = json::Value::make_array();
    for (const std::string& value : values) {
        array.push(json::Value::make_string(value));
    }

    return array;
}

/// `%ProgramData%`：机器级程序数据。
std::filesystem::path program_data_directory() {
    const std::wstring value = environment_variable(L"ProgramData");
    if (!value.empty()) {
        return std::filesystem::path(value);
    }

    return std::filesystem::path(L"C:\\ProgramData");
}

/// `%LOCALAPPDATA%`：用户级回落位置。
std::filesystem::path local_app_data_directory() {
    const std::wstring value = environment_variable(L"LocalAppData");
    if (!value.empty()) {
        return std::filesystem::path(value);
    }

    const std::wstring profile = environment_variable(L"USERPROFILE");
    if (!profile.empty()) {
        return std::filesystem::path(profile) / "AppData" / "Local";
    }

    return {};
}

/// 真实写入探测：能建目录、能建文件、能删文件，才算可写。
bool probe_writable(const std::filesystem::path& directory) {
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) {
        return false;
    }

    const std::filesystem::path probe = directory / ("msixpkg-write-probe-" + new_install_id() + ".tmp");
    const HANDLE file = ::CreateFileW(
        probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }

    ::CloseHandle(file);
    std::filesystem::remove(probe, error);
    return true;
}

std::wstring environment_variable(const wchar_t* name) {
    const DWORD needed = ::GetEnvironmentVariableW(name, nullptr, 0);
    if (needed == 0) {
        return {};
    }

    std::wstring buffer(needed, L'\0');
    const DWORD written = ::GetEnvironmentVariableW(name, buffer.data(), needed);
    buffer.resize(written);
    return buffer;
}

DataRoots make_roots(const std::filesystem::path& root, bool machine_wide, std::string warning) {
    DataRoots roots;
    roots.root = root;
    roots.state = root / "state";
    roots.staging = root / "staging";
    roots.logs = root / "logs";
    roots.machine_wide = machine_wide;
    roots.warning = std::move(warning);
    return roots;
}

}  // namespace

// ---------- InstallationRecord ----------

std::string InstallationRecord::to_json() const {
    json::Value root = json::Value::make_object();
    root.set("installId", json::Value::make_string(install_id));
    root.set("containerPath", json::Value::make_string(container_path));
    root.set("contentDigest", json::Value::make_string(content_digest));
    root.set("identityName", json::Value::make_string(identity_name));
    root.set("identityPublisher", json::Value::make_string(identity_publisher));
    root.set("identityVersion", json::Value::make_string(identity_version));
    root.set("identityArchitecture", json::Value::make_string(identity_architecture));
    root.set("packageFullName", json::Value::make_string(package_full_name));
    root.set("packageFamilyName", json::Value::make_string(package_family_name));
    root.set("publisherSubject", json::Value::make_string(publisher_subject));
    root.set("certificateSha1", json::Value::make_string(certificate_sha1));
    root.set("certificateSha256", json::Value::make_string(certificate_sha256));
    root.set("certificateAddedByInstaller", json::Value::make_bool(certificate_added_by_installer));
    root.set("certificateExistedBefore", json::Value::make_bool(certificate_existed_before));
    root.set("certificateRemoved", json::Value::make_bool(certificate_removed));
    root.set("allUsers", json::Value::make_bool(all_users));
    root.set("provisioned", json::Value::make_bool(provisioned));
    root.set("arpEntryCreated", json::Value::make_bool(arp_entry_created));
    root.set("arpHive", json::Value::make_string(arp_hive));
    root.set("uninstallRemoveCertificate", json::Value::make_bool(uninstall_remove_certificate));
    root.set("dependencies", string_array(dependencies));
    root.set("payloadSha256", string_array(payload_sha256));
    root.set("logPath", json::Value::make_string(log_path));
    root.set("result", json::Value::make_string(result));
    root.set("failureCode", json::Value::make_string(failure_code));
    root.set("failureDetail", json::Value::make_string(failure_detail));
    root.set("startedUtc", json::Value::make_string(started_utc));
    root.set("finishedUtc", json::Value::make_string(finished_utc));
    return root.to_json();
}

InstallationRecord InstallationRecord::from_json(std::string_view text) {
    const json::Value root = json::parse(text);
    if (root.kind != json::Value::Kind::Object) {
        throw Error(codes::IoError, "The installation state file is not a JSON object.");
    }

    InstallationRecord record;
    record.install_id = get_string(root, "installId");
    record.container_path = get_string(root, "containerPath");
    record.content_digest = get_string(root, "contentDigest");
    record.identity_name = get_string(root, "identityName");
    record.identity_publisher = get_string(root, "identityPublisher");
    record.identity_version = get_string(root, "identityVersion");
    record.identity_architecture = get_string(root, "identityArchitecture");
    record.package_full_name = get_string(root, "packageFullName");
    record.package_family_name = get_string(root, "packageFamilyName");
    record.publisher_subject = get_string(root, "publisherSubject");
    record.certificate_sha1 = get_string(root, "certificateSha1");
    record.certificate_sha256 = get_string(root, "certificateSha256");
    record.certificate_added_by_installer = get_bool(root, "certificateAddedByInstaller");
    record.certificate_existed_before = get_bool(root, "certificateExistedBefore");
    record.certificate_removed = get_bool(root, "certificateRemoved");
    record.all_users = get_bool(root, "allUsers");
    record.provisioned = get_bool(root, "provisioned");
    record.arp_entry_created = get_bool(root, "arpEntryCreated");
    record.arp_hive = get_string(root, "arpHive");
    record.uninstall_remove_certificate = get_bool(root, "uninstallRemoveCertificate");
    record.dependencies = get_string_array(root, "dependencies");
    record.payload_sha256 = get_string_array(root, "payloadSha256");
    record.log_path = get_string(root, "logPath");
    record.result = get_string(root, "result");
    record.failure_code = get_string(root, "failureCode");
    record.failure_detail = get_string(root, "failureDetail");
    record.started_utc = get_string(root, "startedUtc");
    record.finished_utc = get_string(root, "finishedUtc");
    return record;
}

// ---------- 数据目录 ----------

DataRoots data_roots_at(const std::filesystem::path& root, bool machine_wide) {
    return make_roots(root, machine_wide, {});
}

/// 数据根目录的候选列表（顺序即优先级）。
struct DataRootCandidate {
    std::filesystem::path root;
    bool machine_wide = false;
    /// 来自 `MSIXPKG_DATA_ROOT`：显式指定，回落时不必再解释「为什么不是 ProgramData」。
    bool explicit_override = false;
};

std::vector<DataRootCandidate> data_root_candidates() {
    std::vector<DataRootCandidate> candidates;

    // 1) 显式覆盖：测试、E2E、以及用户把数据放到别处。
    const std::wstring override_root = environment_variable(L"MSIXPKG_DATA_ROOT");
    if (!override_root.empty()) {
        candidates.push_back({std::filesystem::path(override_root), false, true});
    }

    // 2) 机器级首选位置。
    const std::filesystem::path program_data = program_data_directory();
    if (!program_data.empty()) {
        candidates.push_back({program_data / "Msixpkg", true, false});
    }

    // 3) 用户级回落。
    const std::filesystem::path local_app_data = local_app_data_directory();
    if (!local_app_data.empty()) {
        candidates.push_back({local_app_data / "Msixpkg", false, false});
    }

    // 4) 最后兜底：工作区（受限执行环境里这是唯一一定可写的地方）。
    std::error_code error;
    candidates.push_back({std::filesystem::current_path(error) / "artifacts" / "data", false, false});
    return candidates;
}

DataRoots resolve_data_roots() {
    std::vector<DataRootCandidate> candidates = data_root_candidates();

    for (size_t index = 0; index < candidates.size(); ++index) {
        const DataRootCandidate& candidate = candidates[index];
        if (!probe_writable(candidate.root)) {
            continue;
        }

        DataRoots roots = make_roots(candidate.root, candidate.machine_wide, {});
        if (!probe_writable(roots.state) || !probe_writable(roots.staging) || !probe_writable(roots.logs)) {
            continue;
        }

        if (index > 0 && !candidates.front().explicit_override) {
            roots.warning =
                "The preferred machine-wide data directory (%ProgramData%\\Msixpkg) was not writable; "
                "using " + narrow_utf8(roots.root.wstring()) + " instead.";
        }

        return roots;
    }

    throw Error(
        codes::IoError,
        "No writable data directory was found. Set MSIXPKG_DATA_ROOT to a writable path.");
}

DataRoots peek_data_roots() {
    const std::vector<DataRootCandidate> candidates = data_root_candidates();
    if (candidates.empty()) {
        return {};
    }

    for (const DataRootCandidate& candidate : candidates) {
        std::error_code error;
        if (std::filesystem::is_directory(candidate.root, error)) {
            return make_roots(candidate.root, candidate.machine_wide, {});
        }
    }

    // 都还没建出来：把首选位置的路径交出去供展示 —— 但**绝不创建**。
    return make_roots(candidates.front().root, candidates.front().machine_wide, {});
}

std::vector<DataRoots> publisher_policy_roots() {
    const std::vector<DataRootCandidate> candidates = data_root_candidates();
    if (candidates.empty()) {
        return {};
    }

    // 显式覆盖（测试 / E2E / 用户把数据放到别处）：只认那一个根。
    // 否则测试会被本机真实的 %ProgramData% 、%LOCALAPPDATA% 记录污染。
    if (candidates.front().explicit_override) {
        return {make_roots(candidates.front().root, candidates.front().machine_wide, {})};
    }

    std::vector<DataRoots> roots;
    for (const DataRootCandidate& candidate : candidates) {
        std::error_code error;
        if (std::filesystem::is_directory(candidate.root, error)) {
            roots.push_back(make_roots(candidate.root, candidate.machine_wide, {}));
        }
    }

    // 一个都还没建出来：给首选位置（读不到就是「什么都没记住」）。
    if (roots.empty()) {
        roots.push_back(make_roots(candidates.front().root, candidates.front().machine_wide, {}));
    }

    return roots;
}

void StateStore::ensure_directories() const {
    std::error_code error;
    for (const std::filesystem::path& directory : {roots_.root, roots_.state, roots_.staging, roots_.logs}) {
        std::filesystem::create_directories(directory, error);
        if (error) {
            throw Error(
                codes::IoError,
                "Unable to create the data directory.",
                narrow_utf8(directory.wstring()) + ": " + error.message());
        }
    }
}

void StateStore::save(const InstallationRecord& record) const {
    if (record.install_id.empty()) {
        throw Error(codes::IoError, "An installation record requires an install id.");
    }

    if (!is_lower_hex(record.install_id, record.install_id.size())) {
        // install id 会直接变成文件名：不放行任何可能带路径分隔符/上级目录的字符。
        throw Error(codes::IoError, "The install id must be lowercase hexadecimal.", record.install_id);
    }

    ensure_directories();
    write_text_atomic(roots_.state / (record.install_id + ".json"), record.to_json());
}

std::optional<InstallationRecord> StateStore::load(std::string_view install_id) const {
    if (install_id.empty()) {
        return std::nullopt;
    }

    const std::optional<std::string> text = read_text_file(roots_.state / (std::string(install_id) + ".json"));
    if (!text.has_value()) {
        return std::nullopt;
    }

    return InstallationRecord::from_json(*text);
}

std::vector<InstallationRecord> StateStore::load_all() const {
    std::vector<InstallationRecord> records;
    std::error_code error;
    if (!std::filesystem::is_directory(roots_.state, error)) {
        return records;
    }

    for (const auto& entry : std::filesystem::directory_iterator(roots_.state, error)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".json") {
            continue;
        }

        const std::optional<std::string> text = read_text_file(entry.path());
        if (!text.has_value()) {
            continue;
        }

        try {
            records.push_back(InstallationRecord::from_json(*text));
        } catch (const std::exception&) {
            // 坏文件（被人手改坏 / 写到一半）不能让 list、verify、repair 整体失败。
        }
    }

    std::sort(records.begin(), records.end(), [](const InstallationRecord& left, const InstallationRecord& right) {
        return left.install_id < right.install_id;
    });
    return records;
}

bool StateStore::remove(std::string_view install_id) const {
    if (install_id.empty()) {
        return true;
    }

    std::error_code error;
    std::filesystem::remove(roots_.state / (std::string(install_id) + ".json"), error);
    return !error;
}

std::vector<InstallationRecord> StateStore::find_by_family_name(std::string_view family_name) const {
    std::vector<InstallationRecord> matches;
    for (const InstallationRecord& record : load_all()) {
        if (equals_ignore_case_ascii(record.package_family_name, family_name)) {
            matches.push_back(record);
        }
    }

    return matches;
}

std::vector<InstallationRecord> StateStore::find_by_certificate(std::string_view sha1_hex) const {
    std::vector<InstallationRecord> matches;
    for (const InstallationRecord& record : load_all()) {
        if (equals_ignore_case_ascii(record.certificate_sha1, sha1_hex)) {
            matches.push_back(record);
        }
    }

    return matches;
}

// ---------- 文件与目录工具 ----------

std::optional<std::string> read_text_file(const std::filesystem::path& path) {
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error)) {
        return std::nullopt;
    }

    const Bytes bytes = read_file(path);
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

void write_text_atomic(const std::filesystem::path& path, const std::string& text) {
    const std::filesystem::path directory = path.parent_path();
    if (!directory.empty()) {
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        if (error) {
            throw Error(
                codes::IoError,
                "Unable to create the directory of the state file.",
                narrow_utf8(directory.wstring()) + ": " + error.message());
        }
    }

    const std::filesystem::path temporary = path.native() + L".tmp-" + widen_utf8(new_install_id());
    HANDLE file = ::CreateFileW(
        temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        throw Error(
            codes::IoError,
            "Unable to create the temporary state file.",
            narrow_utf8(temporary.wstring()) + ": Win32 error " + std::to_string(::GetLastError()));
    }

    const char* cursor = text.data();
    size_t remaining = text.size();
    while (remaining > 0) {
        const DWORD chunk = static_cast<DWORD>(std::min<size_t>(remaining, 1u << 20));
        DWORD written = 0;
        if (!::WriteFile(file, cursor, chunk, &written, nullptr) || written == 0) {
            const DWORD error = ::GetLastError();
            ::CloseHandle(file);
            ::DeleteFileW(temporary.c_str());
            throw Error(codes::IoError, "Unable to write the state file.", "Win32 error " + std::to_string(error));
        }

        cursor += written;
        remaining -= written;
    }

    // 先落盘再改名：断电时最多留下一个临时文件，绝不会留下半个状态文件。
    ::FlushFileBuffers(file);
    ::CloseHandle(file);

    if (::ReplaceFileW(path.c_str(), temporary.c_str(), nullptr, REPLACEFILE_WRITE_THROUGH, nullptr, nullptr)) {
        return;
    }

    const DWORD replace_error = ::GetLastError();
    // 目标还不存在时 ReplaceFile 一定失败，这不是错误，改用 MoveFileEx 完成「创建」。
    if (::MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        return;
    }

    const DWORD move_error = ::GetLastError();
    ::DeleteFileW(temporary.c_str());
    throw Error(
        codes::IoError,
        "Unable to replace the state file.",
        "ReplaceFile: " + std::to_string(replace_error) + ", MoveFileEx: " + std::to_string(move_error));
}

std::filesystem::path create_staging_directory(const DataRoots& roots) {
    const std::filesystem::path directory = roots.staging / widen_utf8(new_install_id());
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) {
        throw Error(
            codes::IoError,
            "Unable to create the staging directory.",
            narrow_utf8(directory.wstring()) + ": " + error.message());
    }

    return directory;
}

void remove_tree_if_present(const std::filesystem::path& path) {
    std::error_code error;
    std::filesystem::remove_all(path, error);
}

bool restrict_directory_to_administrators(const std::filesystem::path& path, std::string& error) {
    BYTE administrators[SECURITY_MAX_SID_SIZE] = {};
    DWORD administrators_size = sizeof(administrators);
    if (!::CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, administrators, &administrators_size)) {
        error = "CreateWellKnownSid(Administrators) failed: " + std::to_string(::GetLastError());
        return false;
    }

    BYTE system_sid[SECURITY_MAX_SID_SIZE] = {};
    DWORD system_size = sizeof(system_sid);
    if (!::CreateWellKnownSid(WinLocalSystemSid, nullptr, system_sid, &system_size)) {
        error = "CreateWellKnownSid(SYSTEM) failed: " + std::to_string(::GetLastError());
        return false;
    }

    EXPLICIT_ACCESS_W entries[2] = {};
    entries[0].grfAccessPermissions = GENERIC_ALL;
    entries[0].grfAccessMode = SET_ACCESS;
    entries[0].grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
    entries[0].Trustee.TrusteeForm = TRUSTEE_IS_SID;
    entries[0].Trustee.TrusteeType = TRUSTEE_IS_GROUP;
    entries[0].Trustee.ptstrName = reinterpret_cast<LPWSTR>(administrators);

    entries[1].grfAccessPermissions = GENERIC_ALL;
    entries[1].grfAccessMode = SET_ACCESS;
    entries[1].grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
    entries[1].Trustee.TrusteeForm = TRUSTEE_IS_SID;
    entries[1].Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
    entries[1].Trustee.ptstrName = reinterpret_cast<LPWSTR>(system_sid);

    PACL acl = nullptr;
    const DWORD acl_status = ::SetEntriesInAclW(2, entries, nullptr, &acl);
    if (acl_status != ERROR_SUCCESS) {
        error = "SetEntriesInAcl failed: " + std::to_string(acl_status);
        return false;
    }

    // 保护位（PROTECTED_DACL_SECURITY_INFORMATION）让这个 DACL 不继承父目录的 ACE ——
    // 否则普通用户仍能从 %ProgramData% 继承写权限，收紧就白做了。
    std::wstring mutable_path = path.wstring();
    const DWORD status = ::SetNamedSecurityInfoW(
        mutable_path.data(),
        SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
        nullptr,
        nullptr,
        acl,
        nullptr);

    ::LocalFree(acl);
    if (status != ERROR_SUCCESS) {
        error = "SetNamedSecurityInfo failed: " + std::to_string(status);
        return false;
    }

    return true;
}

std::string utc_now() {
    SYSTEMTIME now{};
    ::GetSystemTime(&now);
    char buffer[32] = {};
    std::snprintf(
        buffer,
        sizeof(buffer),
        "%04u-%02u-%02uT%02u:%02u:%02uZ",
        static_cast<unsigned>(now.wYear),
        static_cast<unsigned>(now.wMonth),
        static_cast<unsigned>(now.wDay),
        static_cast<unsigned>(now.wHour),
        static_cast<unsigned>(now.wMinute),
        static_cast<unsigned>(now.wSecond));
    return buffer;
}

std::string new_install_id() {
    uint8_t buffer[16] = {};
    if (::BCryptGenRandom(nullptr, buffer, sizeof(buffer), BCRYPT_USE_SYSTEM_PREFERRED_RNG) >= 0) {
        return to_hex_lower(buffer, sizeof(buffer));
    }

    // 极端兜底：CNG 不可用时用「开机毫秒 + 进程内计数器」，仍然保证同一台机器内不重复。
    static uint64_t counter = 0;
    const uint64_t ticks = static_cast<uint64_t>(::GetTickCount64());
    const uint64_t mixed = (ticks << 16) ^ (++counter);
    for (int index = 0; index < 16; ++index) {
        buffer[index] = static_cast<uint8_t>((mixed >> ((index % 8) * 8)) & 0xFF);
    }

    return to_hex_lower(buffer, sizeof(buffer));
}

}  // namespace msixpkg
