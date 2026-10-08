#pragma once

// 证书存储助手：本机受信任人员存储（LocalMachine\TrustedPeople）的增 / 查 / 删。
//
// 两条不可妥协的红线（见 docs/security-model.md §6）：
//  1. **绝不写 `Root`**。本模块只允许 `LocalMachine\TrustedPeople`（生产）与
//     进程内内存存储（仅单元测试）。存储位置由枚举决定、不接受任意字符串，
//     从类型上堵死「传成 Root」这条路。
//  2. 写入前必须记录「导入前是否已存在」（`AddResult::existed_before`）：
//     它决定卸载时有没有资格回收这张证书 —— 安装前用户自己就信任过的证书，
//     安装器永远无权删除。
//
// 部署栈只认 LocalMachine（MSIX 的 TrustedPeople 检查不看用户级存储），
// 因此生产路径固定为 LocalMachine\TrustedPeople。
//
// 关于单元测试为什么用「进程内内存存储」而不是交接文档建议的
// `CERT_SYSTEM_STORE_CURRENT_USER + L"MsixpkgTest"`，以及为什么不用序列化文件存储
// （`CERT_STORE_PROV_FILENAME_W`）—— 两条都是实测结论：
//  - 本执行环境拒绝**任何** HKCU 注册表写操作（`RegCreateKeyEx` 与 `CertOpenStore`
//    写打开都返回 5 = ERROR_ACCESS_DENIED），用户级存储的增 / 删根本无法运行；
//  - 序列化文件存储在 `CertCloseStore` 后不落盘（重新打开后新增的证书消失），
//    无法承载「换句柄仍然查得到」的断言。
// 内存存储与注册表存储走**完全相同**的 CRUD 路径（`CERT_STORE_ADD_USE_EXISTING`、
// `CERT_FIND_HASH`、`CertDeleteCertificateFromStore` 的引用计数语义），
// 却不需要注册表、不需要管理员、也不污染用户的任何真实证书存储。
// 「真实 TrustedPeople 的持久化」由 M7 的 Windows Sandbox E2E 覆盖。

#include <Windows.h>
#include <wincrypt.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include "internal.h"
#include "msixpkg/errors.h"

namespace msixpkg {

/// 允许操作的证书存储位置。刻意做成枚举而不是字符串：调用方无法构造出 `Root`。
enum class CertStoreLocation {
    /// `LocalMachine\TrustedPeople` —— 唯一被允许写入的信任存储（写入需管理员）。
    LocalMachineTrustedPeople,
    /// 进程内内存存储（`CERT_STORE_PROV_MEMORY`）—— 仅单元测试使用，随句柄销毁。
    Memory,
};

struct CertStoreTarget {
    CertStoreLocation location = CertStoreLocation::LocalMachineTrustedPeople;
    /// 只读打开：查询路径用它，避免为一次查询去申请写权限（普通用户也能查）。
    bool read_only = false;
};

/// `add` 的结果。`added == false && existed_before == true` 是幂等重复导入。
struct AddResult {
    /// 本次调用是否真的把证书写进了存储。
    bool added = false;
    /// 写入之前存储里是否已经有同一张证书（决定卸载时是否有权删除）。
    bool existed_before = false;
};

class CertificateStore {
public:
    /**
     * 打开存储；失败抛 `E_IO`（detail 带 Win32 错误码）。
     *
     * 注意：`LocalMachine\TrustedPeople` 的写打开需要管理员。未提权时 `open` 本身
     * 可能就失败，或推迟到 `add`/`remove` 才以 `E_ACCESSDENIED` 失败；调用方应先
     * 用 `is_process_elevated()` 判断，再决定是否启动提权助手。
     */
    explicit CertificateStore(const CertStoreTarget& target);
    ~CertificateStore();

    CertificateStore(const CertificateStore&) = delete;
    CertificateStore& operator=(const CertificateStore&) = delete;
    CertificateStore(CertificateStore&& other) noexcept;
    CertificateStore& operator=(CertificateStore&& other) noexcept;

    /// 生产查询路径：`LocalMachine\TrustedPeople`，默认只读打开。
    static CertificateStore open_machine_trusted_people(bool read_only = true);
    /// 测试路径：全新的进程内内存存储（每个句柄一份，互不影响）。
    static CertificateStore open_memory(bool read_only = false);

    [[nodiscard]] bool is_open() const noexcept { return store_ != nullptr; }
    [[nodiscard]] const std::wstring& display_name() const noexcept { return display_name_; }
    [[nodiscard]] bool read_only() const noexcept { return read_only_; }

    /// 按 SHA-1 指纹（20 字节 DER 摘要）查询是否存在。
    [[nodiscard]] bool contains(const Bytes& sha1) const;
    /// 同上，但用 40 位十六进制指纹。
    [[nodiscard]] bool contains_hex(std::string_view sha1_hex) const;

    /**
     * 幂等写入。
     *
     * - `der` 不是合法证书 → `E_CERTIFICATE_MALFORMED`，且存储无任何残留；
     * - `sha1` 与 `der` 实际指纹不符 → 同样 `E_CERTIFICATE_MALFORMED`
     *   （防止「记录了一个指纹、实际信任了另一张证书」）；
     * - `sha1` 长度不是 20 → `E_CERTIFICATE_MALFORMED`。
     */
    AddResult add(const Bytes& der, const Bytes& sha1) const;

    /// 删除命中项；未命中返回 false（视为「已不在存储中」，不是错误）。
    /// 删除失败（如权限不足）也返回 false —— 调用方按「未删除」处理，不重试。
    bool remove(const Bytes& sha1) const;
    bool remove_hex(std::string_view sha1_hex) const;

private:
    explicit CertificateStore(HCERTSTORE store, std::wstring display_name, bool read_only)
        : store_(store), display_name_(std::move(display_name)), read_only_(read_only) {}

    [[nodiscard]] PCCERT_CONTEXT find(const Bytes& sha1) const;
    /// 指纹长度必须是 20 字节，否则抛 E_CERTIFICATE_MALFORMED。
    void require_sha1(const Bytes& sha1) const;

    HCERTSTORE store_ = nullptr;
    std::wstring display_name_;
    bool read_only_ = false;
};

// ---------- 生产路径的便捷入口（固定 LocalMachine\TrustedPeople） ----------

/// 只读查询：该指纹是否已在**本机受信任人员**存储中。抛出 `E_IO` 仅当存储打不开。
bool is_publisher_trusted(const Bytes& sha1);
bool is_publisher_trusted_hex(std::string_view sha1_hex);

/// 写入本机受信任人员存储（需管理员）。返回是否本次新增 + 之前是否已存在。
AddResult trust_publisher(const Bytes& der, const Bytes& sha1);

/// 从本机受信任人员存储删除（需管理员）。未命中返回 false。
bool untrust_publisher(const Bytes& sha1);
bool untrust_publisher_hex(std::string_view sha1_hex);

/// 当前进程是否已提权（用于在启动 `runas` 助手之前做判断，而不是靠失败反推）。
bool is_process_elevated();

}  // namespace msixpkg
