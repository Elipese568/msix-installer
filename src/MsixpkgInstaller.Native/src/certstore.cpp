#include "certstore.h"

#include <Windows.h>
#include <wincrypt.h>

#include <string>
#include <utility>

#include "crypto.h"
#include "internal.h"

namespace msixpkg {
namespace {

/// 只写 `LocalMachine\TrustedPeople`，绝不写 `Root`（见 docs/security-model.md 红线 1）。
constexpr wchar_t kTrustedPeopleStore[] = L"TrustedPeople";

[[noreturn]] void throw_win32(const char* message, DWORD error) {
    throw Error(codes::IoError, message, "Win32 error " + std::to_string(error));
}

std::wstring describe_target(const CertStoreTarget& target) {
    if (target.location == CertStoreLocation::Memory) {
        return L"(memory store)";
    }

    return std::wstring(L"LocalMachine\\") + kTrustedPeopleStore;
}

HCERTSTORE open_store(const CertStoreTarget& target) {
    if (target.location == CertStoreLocation::Memory) {
        HCERTSTORE store = ::CertOpenStore(CERT_STORE_PROV_MEMORY, 0, 0, 0, nullptr);
        if (store == nullptr) {
            throw_win32("Unable to open the in-memory certificate store.", ::GetLastError());
        }

        return store;
    }

    if (target.location != CertStoreLocation::LocalMachineTrustedPeople) {
        throw Error(codes::IoError, "Unsupported certificate store location.");
    }

    const DWORD flags =
        CERT_SYSTEM_STORE_LOCAL_MACHINE | (target.read_only ? CERT_STORE_READONLY_FLAG : 0);
    HCERTSTORE store = ::CertOpenStore(
        CERT_STORE_PROV_SYSTEM_W, 0, 0, flags, static_cast<const void*>(kTrustedPeopleStore));
    if (store == nullptr) {
        throw_win32("Unable to open the certificate store.", ::GetLastError());
    }

    return store;
}

}  // namespace

CertificateStore::CertificateStore(const CertStoreTarget& target)
    : read_only_(target.read_only) {
    display_name_ = describe_target(target);
    store_ = open_store(target);
}

CertificateStore::~CertificateStore() {
    if (store_ != nullptr) {
        ::CertCloseStore(store_, 0);
        store_ = nullptr;
    }
}

CertificateStore::CertificateStore(CertificateStore&& other) noexcept
    : store_(other.store_),
      display_name_(std::move(other.display_name_)),
      read_only_(other.read_only_) {
    other.store_ = nullptr;
}

CertificateStore& CertificateStore::operator=(CertificateStore&& other) noexcept {
    if (this != &other) {
        if (store_ != nullptr) {
            ::CertCloseStore(store_, 0);
        }

        store_ = other.store_;
        display_name_ = std::move(other.display_name_);
        read_only_ = other.read_only_;
        other.store_ = nullptr;
    }

    return *this;
}

CertificateStore CertificateStore::open_machine_trusted_people(bool read_only) {
    CertStoreTarget target;
    target.location = CertStoreLocation::LocalMachineTrustedPeople;
    target.read_only = read_only;
    return CertificateStore(target);
}

CertificateStore CertificateStore::open_memory(bool read_only) {
    CertStoreTarget target;
    target.location = CertStoreLocation::Memory;
    target.read_only = read_only;
    return CertificateStore(target);
}

void CertificateStore::require_sha1(const Bytes& sha1) const {
    if (sha1.size() != 20) {
        throw Error(
            codes::CertificateMalformed,
            "A certificate SHA-1 thumbprint must be exactly 20 bytes.",
            "got " + std::to_string(sha1.size()) + " byte(s)");
    }
}

PCCERT_CONTEXT CertificateStore::find(const Bytes& sha1) const {
    if (store_ == nullptr) {
        throw Error(codes::IoError, "The certificate store is not open.");
    }

    require_sha1(sha1);

    // CertFindCertificateInStore 接受的是非 const 指针，但它只读取这段哈希。
    CRYPT_HASH_BLOB hash{};
    hash.cbData = static_cast<DWORD>(sha1.size());
    hash.pbData = const_cast<BYTE*>(sha1.data());

    // 返回的上下文归调用方所有，必须 CertFreeCertificateContext。
    return ::CertFindCertificateInStore(
        store_, X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, 0, CERT_FIND_HASH, &hash, nullptr);
}

bool CertificateStore::contains(const Bytes& sha1) const {
    PCCERT_CONTEXT found = find(sha1);
    if (found == nullptr) {
        // CERT_FIND_HASH 的「未命中」与真正的失败无法区分；两者都按「不存在」处理
        // 是安全的：查询失败只会让调用方更保守（倾向于不删证书）。
        return false;
    }

    ::CertFreeCertificateContext(found);
    return true;
}

bool CertificateStore::contains_hex(std::string_view sha1_hex) const {
    Bytes sha1;
    if (!try_from_hex(sha1_hex, sha1) || sha1.size() != 20) {
        throw Error(
            codes::CertificateMalformed,
            "The certificate SHA-1 thumbprint is not 40 hexadecimal characters.",
            std::string(sha1_hex));
    }

    return contains(sha1);
}

AddResult CertificateStore::add(const Bytes& der, const Bytes& sha1) const {
    if (store_ == nullptr) {
        throw Error(codes::IoError, "The certificate store is not open.");
    }

    if (read_only_) {
        throw Error(
            codes::IoError,
            "The certificate store was opened read-only; adding a certificate is not allowed.",
            "ERROR_ACCESS_DENIED");
    }

    require_sha1(sha1);

    // 先把字节解析成证书上下文：不是合法 DER 证书时在这里就失败，存储不留任何残留。
    PCCERT_CONTEXT created = ::CertCreateCertificateContext(
        X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, der.data(), static_cast<DWORD>(der.size()));
    if (created == nullptr) {
        const DWORD error = ::GetLastError();
        throw Error(
            codes::CertificateMalformed,
            "The publisher certificate bytes are not a valid DER X.509 certificate.",
            "Win32 error " + std::to_string(error));
    }

    // 交叉校验：调用方声称的指纹必须就是这张证书的指纹。
    // 否则状态库会记下 A 的指纹、而信任区里躺着的却是 B —— 卸载时会删错对象或删不掉。
    const Bytes actual = msixpkg::sha1(created->pbCertEncoded, created->cbCertEncoded);
    const std::string actual_hex = to_hex_lower(actual);
    const std::string claimed_hex = to_hex_lower(sha1);
    if (actual_hex != claimed_hex) {
        ::CertFreeCertificateContext(created);
        throw Error(
            codes::CertificateMalformed,
            "The supplied SHA-1 thumbprint does not match the publisher certificate.",
            "expected " + claimed_hex + ", actual " + actual_hex);
    }

    // 先查再写：写入是幂等的，但只有「写入前不存在」才允许在卸载时回收。
    AddResult result;
    PCCERT_CONTEXT existing = find(sha1);
    result.existed_before = existing != nullptr;
    if (existing != nullptr) {
        ::CertFreeCertificateContext(existing);
    }

    if (!::CertAddCertificateContextToStore(
            store_, created, CERT_STORE_ADD_USE_EXISTING, nullptr)) {
        const DWORD error = ::GetLastError();
        ::CertFreeCertificateContext(created);
        throw_win32("Unable to add the publisher certificate to the trust store.", error);
    }

    ::CertFreeCertificateContext(created);
    result.added = !result.existed_before;
    return result;
}

bool CertificateStore::remove(const Bytes& sha1) const {
    if (store_ == nullptr) {
        throw Error(codes::IoError, "The certificate store is not open.");
    }

    if (read_only_) {
        throw Error(
            codes::IoError,
            "The certificate store was opened read-only; removing a certificate is not allowed.",
            "ERROR_ACCESS_DENIED");
    }

    PCCERT_CONTEXT found = find(sha1);
    if (found == nullptr) {
        return false;
    }

    // CertDuplicateCertificateContext 取得一个自有引用后就可以释放查询结果；
    // 传给 CertDeleteCertificateFromStore 的那个引用由它自己释放（成功失败都释放，
    // 见 MSDN 的 Remarks），因此两条路径都不能再手动 CertFreeCertificateContext。
    PCCERT_CONTEXT owned = ::CertDuplicateCertificateContext(found);
    ::CertFreeCertificateContext(found);
    if (owned == nullptr) {
        return false;
    }

    const BOOL deleted = ::CertDeleteCertificateFromStore(owned);
    return deleted != FALSE;
}

bool CertificateStore::remove_hex(std::string_view sha1_hex) const {
    Bytes sha1;
    if (!try_from_hex(sha1_hex, sha1) || sha1.size() != 20) {
        throw Error(
            codes::CertificateMalformed,
            "The certificate SHA-1 thumbprint is not 40 hexadecimal characters.",
            std::string(sha1_hex));
    }

    return remove(sha1);
}

// ---------- 生产路径的便捷入口 ----------

bool is_publisher_trusted(const Bytes& sha1) {
    CertificateStore store = CertificateStore::open_machine_trusted_people(true);
    return store.contains(sha1);
}

bool is_publisher_trusted_hex(std::string_view sha1_hex) {
    CertificateStore store = CertificateStore::open_machine_trusted_people(true);
    return store.contains_hex(sha1_hex);
}

AddResult trust_publisher(const Bytes& der, const Bytes& sha1) {
    CertificateStore store = CertificateStore::open_machine_trusted_people(false);
    return store.add(der, sha1);
}

bool untrust_publisher(const Bytes& sha1) {
    CertificateStore store = CertificateStore::open_machine_trusted_people(false);
    return store.remove(sha1);
}

bool untrust_publisher_hex(std::string_view sha1_hex) {
    Bytes sha1;
    if (!try_from_hex(sha1_hex, sha1) || sha1.size() != 20) {
        throw Error(
            codes::CertificateMalformed,
            "The certificate SHA-1 thumbprint is not 40 hexadecimal characters.",
            std::string(sha1_hex));
    }

    return untrust_publisher(sha1);
}

bool is_process_elevated() {
    HANDLE token = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return false;
    }

    TOKEN_ELEVATION elevation{};
    DWORD returned = 0;
    const BOOL ok = ::GetTokenInformation(
        token, TokenElevation, &elevation, sizeof(elevation), &returned);
    ::CloseHandle(token);
    return ok != FALSE && elevation.TokenIsElevated != 0;
}

}  // namespace msixpkg
