#include "crypto.h"

#include <Windows.h>
#include <bcrypt.h>
#include <ncrypt.h>

#include <array>
#include <cstring>

#pragma comment(lib, "bcrypt.lib")

namespace msixpkg {
namespace {

void check_ntstatus(NTSTATUS status, const char* operation) {
    if (status < 0) {
        fail(codes::SignatureInvalid, std::string("A crypto operation failed: ") + operation + ".",
             "NTSTATUS " + std::to_string(static_cast<long>(status)));
    }
}

}  // namespace

Sha256::Sha256() {
    check_ntstatus(::BCryptOpenAlgorithmProvider(reinterpret_cast<BCRYPT_ALG_HANDLE*>(&algorithm_),
                                                 BCRYPT_SHA256_ALGORITHM, nullptr, 0),
                   "BCryptOpenAlgorithmProvider(SHA256)");

    DWORD object_size = 0;
    DWORD written = 0;
    check_ntstatus(::BCryptGetProperty(static_cast<BCRYPT_ALG_HANDLE>(algorithm_), BCRYPT_OBJECT_LENGTH,
                                       reinterpret_cast<PUCHAR>(&object_size), sizeof(object_size), &written, 0),
                   "BCryptGetProperty(OBJECT_LENGTH)");

    hash_object_.assign(object_size, 0);
    check_ntstatus(::BCryptCreateHash(static_cast<BCRYPT_ALG_HANDLE>(algorithm_),
                                      reinterpret_cast<BCRYPT_HASH_HANDLE*>(&hash_),
                                      hash_object_.data(), object_size, nullptr, 0, 0),
                   "BCryptCreateHash");
}

Sha256::~Sha256() {
    if (hash_ != nullptr) {
        ::BCryptDestroyHash(static_cast<BCRYPT_HASH_HANDLE>(hash_));
    }
    if (algorithm_ != nullptr) {
        ::BCryptCloseAlgorithmProvider(static_cast<BCRYPT_ALG_HANDLE>(algorithm_), 0);
    }
}

void Sha256::update(const uint8_t* data, size_t size) {
    if (size == 0) {
        return;
    }

    check_ntstatus(::BCryptHashData(static_cast<BCRYPT_HASH_HANDLE>(hash_),
                                    const_cast<PUCHAR>(data), static_cast<ULONG>(size), 0),
                   "BCryptHashData");
}

Bytes Sha256::finish() {
    if (finished_) {
        fail(codes::IoError, "Sha256::finish was called twice.");
    }
    finished_ = true;

    Bytes digest(32);
    check_ntstatus(::BCryptFinishHash(static_cast<BCRYPT_HASH_HANDLE>(hash_), digest.data(),
                                      static_cast<ULONG>(digest.size()), 0),
                   "BCryptFinishHash");
    return digest;
}

Bytes sha256(const uint8_t* data, size_t size) {
    Sha256 hash;
    hash.update(data, size);
    return hash.finish();
}

std::string sha256_hex(const uint8_t* data, size_t size) {
    return to_hex_lower(sha256(data, size));
}

std::string sha256_hex(const Bytes& bytes) {
    return sha256_hex(bytes.data(), bytes.size());
}

Bytes sha1(const uint8_t* data, size_t size) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    check_ntstatus(::BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA1_ALGORITHM, nullptr, 0), "BCryptOpenAlgorithmProvider(SHA1)");

    DWORD object_size = 0;
    DWORD written = 0;
    check_ntstatus(::BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&object_size),
                                       sizeof(object_size), &written, 0),
                   "BCryptGetProperty(SHA1 OBJECT_LENGTH)");

    Bytes object(object_size);
    BCRYPT_HASH_HANDLE hash = nullptr;
    check_ntstatus(::BCryptCreateHash(algorithm, &hash, object.data(), object_size, nullptr, 0, 0), "BCryptCreateHash(SHA1)");

    Bytes digest(20);
    check_ntstatus(::BCryptHashData(hash, const_cast<PUCHAR>(data), static_cast<ULONG>(size), 0), "BCryptHashData(SHA1)");
    check_ntstatus(::BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0), "BCryptFinishHash(SHA1)");

    ::BCryptDestroyHash(hash);
    ::BCryptCloseAlgorithmProvider(algorithm, 0);
    return digest;
}

bool base64_decode(std::string_view text, Bytes& out) {
    static const std::array<int8_t, 256> table = [] {
        std::array<int8_t, 256> values{};
        values.fill(-1);
        const std::string alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (size_t i = 0; i < alphabet.size(); ++i) {
            values[static_cast<unsigned char>(alphabet[i])] = static_cast<int8_t>(i);
        }
        return values;
    }();

    out.clear();
    uint32_t buffer = 0;
    int bits = 0;
    size_t padding = 0;

    for (const char raw : text) {
        const auto c = static_cast<unsigned char>(raw);
        if (c == '=') {
            ++padding;
            continue;
        }

        if (padding != 0) {
            return false;  // '=' 之后不允许再有数据
        }

        const int8_t value = table[c];
        if (value < 0) {
            return false;
        }

        buffer = (buffer << 6) | static_cast<uint32_t>(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>((buffer >> bits) & 0xFF));
        }
    }

    if (padding > 2 || (bits != 0 && bits != 2 && bits != 4)) {
        return false;
    }

    return true;
}

std::string base64_encode(const uint8_t* data, size_t size) {
    static constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    result.reserve(((size + 2) / 3) * 4);

    for (size_t i = 0; i < size; i += 3) {
        const uint32_t a = data[i];
        const uint32_t b = (i + 1 < size) ? data[i + 1] : 0;
        const uint32_t c = (i + 2 < size) ? data[i + 2] : 0;
        const uint32_t triple = (a << 16) | (b << 8) | c;

        result.push_back(alphabet[(triple >> 18) & 0x3F]);
        result.push_back(alphabet[(triple >> 12) & 0x3F]);
        result.push_back((i + 1 < size) ? alphabet[(triple >> 6) & 0x3F] : '=');
        result.push_back((i + 2 < size) ? alphabet[triple & 0x3F] : '=');
    }

    return result;
}

void verify_signature(void* key, std::string_view algorithm, const Bytes& digest, const Bytes& signature) {
    if (key == nullptr) {
        fail(codes::SignatureInvalid, "The signature cannot be verified without a public key.");
    }

    if (digest.size() != 32) {
        fail(codes::SignatureInvalid, "The content digest must be a 32-byte SHA-256 digest.");
    }

    if (!signature_algorithm::is_supported(algorithm)) {
        fail(codes::SignatureAlgorithmUnsupported, "Unsupported container signature algorithm.", std::string(algorithm));
    }

    BCRYPT_PSS_PADDING_INFO pss_info{BCRYPT_SHA256_ALGORITHM, 32};
    // PKCS#1 验签必须告诉 CNG 用的是哪个哈希算法（它要靠这个构造 DigestInfo 前缀）；
    // 只传 NULL 会得到 STATUS_INVALID_PARAMETER（0xC000000D，实测）。
    BCRYPT_PKCS1_PADDING_INFO pkcs1_info{BCRYPT_SHA256_ALGORITHM};

    const bool is_pss = algorithm == signature_algorithm::RsaPssSha256;
    const bool is_pkcs1 = algorithm == signature_algorithm::RsaPkcs1Sha256;

    void* padding_info = is_pss ? static_cast<void*>(&pss_info)
                                : (is_pkcs1 ? static_cast<void*>(&pkcs1_info) : nullptr);
    const ULONG flags = is_pss ? BCRYPT_PAD_PSS : (is_pkcs1 ? BCRYPT_PAD_PKCS1 : 0);

    NTSTATUS status = ::BCryptVerifySignature(static_cast<BCRYPT_KEY_HANDLE>(key), padding_info,
                                              const_cast<PUCHAR>(digest.data()), static_cast<ULONG>(digest.size()),
                                              const_cast<PUCHAR>(signature.data()), static_cast<ULONG>(signature.size()),
                                              flags);

    // 少数 CNG 实现要求 PKCS#1 的 padding info 为 NULL；只在参数非法时退一步重试。
    if (is_pkcs1 && status == static_cast<NTSTATUS>(0xC000000DL)) {
        status = ::BCryptVerifySignature(static_cast<BCRYPT_KEY_HANDLE>(key), nullptr,
                                         const_cast<PUCHAR>(digest.data()), static_cast<ULONG>(digest.size()),
                                         const_cast<PUCHAR>(signature.data()), static_cast<ULONG>(signature.size()),
                                         flags);
    }

    // STATUS_INVALID_SIGNATURE（0xC000A000）在 ntstatus.h 里，而 ntstatus.h 与 winerror.h
    // 不能同时包含；这里直接用字面量，避免把 DDK 头拖进来。
    constexpr NTSTATUS kStatusInvalidSignature = static_cast<NTSTATUS>(0xC000A000L);

    if (status == kStatusInvalidSignature) {
        fail(codes::SignatureInvalid, "The container signature does not match its content digest.");
    }

    if (status < 0) {
        // 把 NTSTATUS 与两个长度都写进 message：这条路径的失败几乎总是参数/格式问题，
        // 只留 detail 会让测试输出看不到关键数字。
        char buffer[128];
        std::snprintf(buffer, sizeof(buffer), "0x%08lX", static_cast<unsigned long>(status));
        fail(codes::SignatureInvalid,
             std::string("Signature verification failed with NTSTATUS ") + buffer +
                 " (algorithm=" + std::string(algorithm) +
                 ", digest=" + std::to_string(digest.size()) +
                 ", signature=" + std::to_string(signature.size()) + ").",
             "BCryptVerifySignature");
    }
}

}  // namespace msixpkg
