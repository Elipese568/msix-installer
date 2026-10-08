#pragma once

// X.509 证书与 PKCS#7 证书集提取。
//
// 分工：
//  - X.509 字段（Subject/EKU/有效期/公钥）交给 crypt32 —— 让系统解析 ASN.1，
//    比手写一千行 DER 可靠得多；
//  - 只有「从 AppxSignature.p7x 里找出证书」这一段必须自己走 DER，
//    因为真实签名带 4 字节魔数 PKCX，且 Windows 的证书集合导入路径读不了它。
//
// 这个头直接包含 wincrypt.h：crypt32 的证书上下文类型无法用前向声明安全替代
// （在命名空间里前向声明会得到一个不同的类型）。

#include <Windows.h>
#include <wincrypt.h>

#include <cstdint>
#include <string>
#include <vector>

#include "crypto.h"
#include "internal.h"
#include "msixpkg/errors.h"

namespace msixpkg {

/// 最小 DER 元素视图。
struct DerElement {
    uint8_t tag = 0;
    size_t start = 0;
    size_t content = 0;
    size_t end = 0;
};

/// 读取 offset 处的 DER 元素；越界或不定长抛 E_APPX_INVALID。
DerElement der_read(const uint8_t* data, size_t size, size_t offset);

/// crypt32 证书上下文的 RAII 包装。
class Certificate {
public:
    Certificate() = default;
    ~Certificate();

    Certificate(const Certificate&) = delete;
    Certificate& operator=(const Certificate&) = delete;
    Certificate(Certificate&& other) noexcept;
    Certificate& operator=(Certificate&& other) noexcept;

    static Certificate from_der(const uint8_t* data, size_t size);

    [[nodiscard]] bool valid() const noexcept { return context_ != nullptr; }
    [[nodiscard]] PCCERT_CONTEXT context() const noexcept { return context_; }

    /// 与 .NET X509Certificate2.Subject 对齐的 DN 文本（编码顺序、", " 分隔、不引号化）。
    [[nodiscard]] std::string subject_string() const;

    /// 小写十六进制指纹。
    [[nodiscard]] std::string sha1_thumbprint() const;
    [[nodiscard]] std::string sha256_thumbprint() const;

    /// Subject == Issuer 的启发式自签名判定（与 C# 端一致）。
    [[nodiscard]] bool is_self_signed() const;

    /// 返回是否含代码签名 EKU；has_extension 指示是否存在 EKU 扩展
    /// （不存在时规范只告警，存在但缺该项才是错误）。
    [[nodiscard]] bool has_code_signing_eku(bool& has_extension) const;

    /// FILETIME（100ns since 1601）—— 可直接与 now_filetime() 做无符号比较。
    [[nodiscard]] uint64_t not_before() const;
    [[nodiscard]] uint64_t not_after() const;

    /// CNG 公钥句柄（惰性导入，随对象释放）。失败抛 E_CERTIFICATE_MALFORMED。
    [[nodiscard]] void* public_key() const;

private:
    void reset();
    PCCERT_CONTEXT context_ = nullptr;
    mutable void* public_key_ = nullptr;
};

/// 当前时间的 FILETIME 表示。
uint64_t now_filetime();

/// 从 AppxSignature.p7x 提取证书集（自动跳过 PKCX 等头部魔数）。
std::vector<Certificate> extract_certificates_from_appx_signature(const uint8_t* data, size_t size);
inline std::vector<Certificate> extract_certificates_from_appx_signature(const Bytes& bytes) {
    return extract_certificates_from_appx_signature(bytes.data(), bytes.size());
}

}  // namespace msixpkg
