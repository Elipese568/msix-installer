#include "certs.h"

#include <Windows.h>
#include <bcrypt.h>
#include <wincrypt.h>

#include <cstring>

#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "bcrypt.lib")

namespace msixpkg {
namespace {

/// 1.2.840.113549.1.7.2 —— PKCS#7 signedData（DER 内容字节）。
constexpr uint8_t kSignedDataOid[] = {0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x07, 0x02};

/// 真实 Appx 签名前 4 字节是魔数 "PKCX"（实测）。有界搜索同时兼容直接 DER 的产物。
constexpr size_t kMaxHeaderLength = 16;

bool oid_matches(const uint8_t* data, const DerElement& element, const uint8_t* expected, size_t expected_size) {
    const size_t length = element.end - element.content;
    if (length != expected_size) {
        return false;
    }
    return std::memcmp(data + element.content, expected, expected_size) == 0;
}

size_t find_content_info(const uint8_t* data, size_t size) {
    const size_t limit = (size > kMaxHeaderLength + 2) ? kMaxHeaderLength : size - 2;
    for (size_t candidate = 0; candidate <= limit; ++candidate) {
        if (data[candidate] != 0x30) {
            continue;
        }

        try {
            const DerElement content_info = der_read(data, size, candidate);
            const DerElement oid = der_read(data, size, content_info.content);
            if (oid.tag == 0x06 && oid_matches(data, oid, kSignedDataOid, sizeof(kSignedDataOid))) {
                return candidate;
            }
        } catch (const Error&) {
            // 这个偏移不是 ContentInfo，继续往后找。
        }
    }

    fail(codes::AppxInvalid,
         "No PKCS#7 signedData ContentInfo was found in AppxSignature.p7x.",
         to_hex_lower(data, size < 8 ? size : 8));
}

}  // namespace

DerElement der_read(const uint8_t* data, size_t size, size_t offset) {
    if (offset + 2 > size) {
        fail(codes::AppxInvalid, "A DER element is truncated.");
    }

    DerElement element;
    element.tag = data[offset];
    element.start = offset;

    size_t cursor = offset + 1;
    const uint8_t first_length = data[cursor++];
    uint64_t length = 0;

    if ((first_length & 0x80) == 0) {
        length = first_length;
    } else {
        const int length_bytes = first_length & 0x7F;
        if (length_bytes == 0) {
            fail(codes::AppxInvalid, "Indefinite-length DER is not supported.");
        }
        if (length_bytes > 8 || cursor + static_cast<size_t>(length_bytes) > size) {
            fail(codes::AppxInvalid, "A DER length field is invalid.");
        }
        for (int i = 0; i < length_bytes; ++i) {
            length = (length << 8) | data[cursor++];
        }
    }

    if (length > size - cursor) {
        fail(codes::AppxInvalid, "A DER element claims more bytes than remain in the buffer.");
    }

    element.content = cursor;
    element.end = cursor + static_cast<size_t>(length);
    return element;
}

Certificate::Certificate(Certificate&& other) noexcept : context_(other.context_), public_key_(other.public_key_) {
    other.context_ = nullptr;
    other.public_key_ = nullptr;
}

Certificate& Certificate::operator=(Certificate&& other) noexcept {
    if (this != &other) {
        reset();
        context_ = other.context_;
        public_key_ = other.public_key_;
        other.context_ = nullptr;
        other.public_key_ = nullptr;
    }
    return *this;
}

Certificate::~Certificate() {
    reset();
}

void Certificate::reset() {
    if (public_key_ != nullptr) {
        ::BCryptDestroyKey(static_cast<BCRYPT_KEY_HANDLE>(public_key_));
        public_key_ = nullptr;
    }
    if (context_ != nullptr) {
        ::CertFreeCertificateContext(context_);
        context_ = nullptr;
    }
}

Certificate Certificate::from_der(const uint8_t* data, size_t size) {
    if (data == nullptr || size == 0) {
        fail(codes::CertificateMalformed, "The certificate data is empty.");
    }

    PCCERT_CONTEXT context = ::CertCreateCertificateContext(
        X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, data, static_cast<DWORD>(size));
    if (context == nullptr) {
        fail(codes::CertificateMalformed, "The certificate could not be parsed.",
             "Win32 error " + std::to_string(::GetLastError()));
    }

    Certificate certificate;
    certificate.context_ = context;
    return certificate;
}

std::string Certificate::subject_string() const {
    if (context_ == nullptr) {
        return {};
    }

    // CERT_NAME_STR_REVERSE_FLAG：crypt32 默认用 Windows 顺序（C=…, O=…, CN=…）；
    // 加上它得到编码顺序，也就是 .NET X509Certificate2.Subject 的形式（CN 在前）。
    // 这条等价关系由 certificate_fields_match_the_golden_manifest 测试锁定。
    const DWORD flags = CERT_X500_NAME_STR | CERT_NAME_STR_NO_QUOTING_FLAG | CERT_NAME_STR_REVERSE_FLAG;
    const int length = ::CertNameToStrW(X509_ASN_ENCODING, &context_->pCertInfo->Subject, flags, nullptr, 0);
    if (length <= 1) {
        return {};
    }

    std::wstring buffer(static_cast<size_t>(length), L'\0');
    ::CertNameToStrW(X509_ASN_ENCODING, &context_->pCertInfo->Subject, flags, buffer.data(), length);
    if (!buffer.empty() && buffer.back() == L'\0') {
        buffer.pop_back();
    }

    return narrow_utf8(buffer);
}

std::string Certificate::sha1_thumbprint() const {
    if (context_ == nullptr) {
        return {};
    }
    return to_hex_lower(sha1(context_->pbCertEncoded, context_->cbCertEncoded));
}

std::string Certificate::sha256_thumbprint() const {
    if (context_ == nullptr) {
        return {};
    }
    return sha256_hex(context_->pbCertEncoded, context_->cbCertEncoded);
}

bool Certificate::is_self_signed() const {
    if (context_ == nullptr) {
        return false;
    }

    return ::CertCompareCertificateName(X509_ASN_ENCODING, &context_->pCertInfo->Subject,
                                       &context_->pCertInfo->Issuer) != FALSE;
}

bool Certificate::has_code_signing_eku(bool& has_extension) const {
    has_extension = false;
    if (context_ == nullptr) {
        return false;
    }

    PCERT_EXTENSION extension = ::CertFindExtension(szOID_ENHANCED_KEY_USAGE, context_->pCertInfo->cExtension,
                                                    context_->pCertInfo->rgExtension);
    if (extension == nullptr) {
        return false;
    }

    has_extension = true;

    DWORD size = 0;
    if (!::CryptDecodeObject(X509_ASN_ENCODING, X509_ENHANCED_KEY_USAGE, extension->Value.pbData,
                             extension->Value.cbData, 0, nullptr, &size)) {
        fail(codes::CertificateMalformed, "The certificate EKU extension could not be decoded.");
    }

    Bytes buffer(size);
    if (!::CryptDecodeObject(X509_ASN_ENCODING, X509_ENHANCED_KEY_USAGE, extension->Value.pbData,
                             extension->Value.cbData, 0, buffer.data(), &size)) {
        fail(codes::CertificateMalformed, "The certificate EKU extension could not be decoded.");
    }

    // X509_ENHANCED_KEY_USAGE 解码出来的就是 CTL_USAGE 布局（cUsageIdentifier + rgpszUsageIdentifier）。
    // 这里用本地结构体声明同一布局，避免依赖某个 SDK 版本把哪个 typedef 暴露成类型名。
    struct EnhancedKeyUsage {
        DWORD count;
        LPSTR* oids;
    };

    const auto* usage = reinterpret_cast<const EnhancedKeyUsage*>(buffer.data());
    for (DWORD i = 0; i < usage->count; ++i) {
        if (usage->oids[i] != nullptr && std::strcmp(usage->oids[i], szOID_PKIX_KP_CODE_SIGNING) == 0) {
            return true;
        }
    }

    return false;
}

uint64_t Certificate::not_before() const {
    if (context_ == nullptr) {
        return 0;
    }
    return (static_cast<uint64_t>(context_->pCertInfo->NotBefore.dwHighDateTime) << 32) |
           context_->pCertInfo->NotBefore.dwLowDateTime;
}

uint64_t Certificate::not_after() const {
    if (context_ == nullptr) {
        return 0;
    }
    return (static_cast<uint64_t>(context_->pCertInfo->NotAfter.dwHighDateTime) << 32) |
           context_->pCertInfo->NotAfter.dwLowDateTime;
}

void* Certificate::public_key() const {
    if (public_key_ != nullptr) {
        return public_key_;
    }

    if (context_ == nullptr) {
        fail(codes::CertificateMalformed, "The certificate is empty.");
    }

    BCRYPT_KEY_HANDLE key = nullptr;
    if (!::CryptImportPublicKeyInfoEx2(X509_ASN_ENCODING, &context_->pCertInfo->SubjectPublicKeyInfo, 0, nullptr, &key)) {
        fail(codes::CertificateMalformed, "The certificate public key could not be imported.",
             "Win32 error " + std::to_string(::GetLastError()));
    }

    public_key_ = key;
    return public_key_;
}

uint64_t now_filetime() {
    FILETIME now{};
    ::GetSystemTimeAsFileTime(&now);
    return (static_cast<uint64_t>(now.dwHighDateTime) << 32) | now.dwLowDateTime;
}

std::vector<Certificate> extract_certificates_from_appx_signature(const uint8_t* data, size_t size) {
    if (data == nullptr || size == 0) {
        fail(codes::AppxSignatureMissing, "AppxSignature.p7x is empty.");
    }

    const size_t offset = find_content_info(data, size);
    const DerElement content_info = der_read(data, size, offset);

    // ContentInfo ::= SEQUENCE { contentType OID, [0] EXPLICIT SignedData }
    const DerElement oid = der_read(data, size, content_info.content);
    if (oid.tag != 0x06 || !oid_matches(data, oid, kSignedDataOid, sizeof(kSignedDataOid))) {
        fail(codes::AppxInvalid, "AppxSignature.p7x is not a PKCS#7 signedData structure.");
    }

    const DerElement explicit_signed_data = der_read(data, size, oid.end);
    if (explicit_signed_data.tag != 0xA0) {
        fail(codes::AppxInvalid, "PKCS#7 signedData is not wrapped in an explicit [0] tag.");
    }

    const DerElement signed_data = der_read(data, size, explicit_signed_data.content);
    if (signed_data.tag != 0x30) {
        fail(codes::AppxInvalid, "PKCS#7 SignedData is not a SEQUENCE.");
    }

    // version INTEGER, digestAlgorithms SET, encapContentInfo SEQUENCE
    size_t cursor = signed_data.content;
    const DerElement version = der_read(data, size, cursor);
    if (version.tag != 0x02) {
        fail(codes::AppxInvalid, "PKCS#7 SignedData has no version.");
    }
    cursor = version.end;

    const DerElement digest_algorithms = der_read(data, size, cursor);
    if (digest_algorithms.tag != 0x31) {
        fail(codes::AppxInvalid, "PKCS#7 SignedData has no digestAlgorithms set.");
    }
    cursor = digest_algorithms.end;

    const DerElement encap_content = der_read(data, size, cursor);
    if (encap_content.tag != 0x30) {
        fail(codes::AppxInvalid, "PKCS#7 SignedData has no encapContentInfo.");
    }
    cursor = encap_content.end;

    std::vector<Certificate> certificates;
    while (cursor < signed_data.end) {
        const DerElement element = der_read(data, size, cursor);
        if (element.tag == 0xA0) {
            size_t inner = element.content;
            while (inner < element.end) {
                const DerElement certificate_element = der_read(data, size, inner);
                if (certificate_element.tag == 0x30) {
                    certificates.push_back(Certificate::from_der(data + certificate_element.start,
                                                                 certificate_element.end - certificate_element.start));
                }
                inner = certificate_element.end;
            }
        }
        cursor = element.end;
    }

    if (certificates.empty()) {
        fail(codes::AppxInvalid, "AppxSignature.p7x contains no certificate.");
    }

    return certificates;
}

}  // namespace msixpkg
