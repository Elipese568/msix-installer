#pragma once

// 密码学原语封装：SHA-256 / RSA / ECDSA 验签（BCrypt），Base64 与容器签名算法标识。
//
// 一条硬规则：**验签只做密码学验证，不查证书链信任**。
// 原因是鸡生蛋 —— 容器由尚未受信任的自签名证书签名，任何基于链信任的 API
// （WinVerifyTrust 的默认策略）都会在「把证书导入受信任存储」之前失败。
// 信任由另外两条独立机制建立：发布者指纹钉扎 + Windows 部署栈对 Appx 签名的校验。

#include <cstdint>
#include <string>
#include <string_view>

#include "internal.h"
#include "msixpkg/errors.h"

namespace msixpkg {

/// 与 C# 端 MsixpkgSignatureAlgorithms 逐字一致的算法标识。
namespace signature_algorithm {

/// v1 写出唯一允许值：PKCS#1 v1.5，确定性，容器字节可复现。
inline constexpr char RsaPkcs1Sha256[] = "RSA-PKCS1-SHA256";
/// 读支持：PSS 的盐随机，产物不可复现。
inline constexpr char RsaPssSha256[] = "RSA-PSS-SHA256";
/// 读支持：ECDSA 的 k 随机，产物不可复现。
inline constexpr char EcdsaP256Sha256[] = "ECDSA-P256-SHA256";

inline bool is_supported(std::string_view algorithm) {
    return algorithm == RsaPkcs1Sha256 || algorithm == RsaPssSha256 || algorithm == EcdsaP256Sha256;
}

}  // namespace signature_algorithm

/// 增量 SHA-256。
class Sha256 {
public:
    Sha256();
    ~Sha256();

    Sha256(const Sha256&) = delete;
    Sha256& operator=(const Sha256&) = delete;

    void update(const uint8_t* data, size_t size);
    /// 结束并返回 32 字节摘要；只能调用一次。
    Bytes finish();

private:
    void* algorithm_ = nullptr;
    /// BCryptCreateHash 要求这个对象缓冲区在哈希句柄整个生命周期内保持有效 ——
    /// 必须作为成员持有，放在构造函数里的局部变量会在构造函数返回时失效。
    Bytes hash_object_;
    void* hash_ = nullptr;
    bool finished_ = false;
};

Bytes sha256(const uint8_t* data, size_t size);
std::string sha256_hex(const uint8_t* data, size_t size);
std::string sha256_hex(const Bytes& bytes);

/// SHA-1（只用于把证书指纹渲染成 Windows 习惯的形式，不用于任何安全判定）。
Bytes sha1(const uint8_t* data, size_t size);

/// 严格 Base64 解码（允许省略 padding 之外无杂字符）；失败返回 false。
bool base64_decode(std::string_view text, Bytes& out);
std::string base64_encode(const uint8_t* data, size_t size);

/// 用公钥验签。`key` 为 CNG 公钥句柄（见 certs.h 的 Certificate::public_key）。
/// 失败抛 E_SIGNATURE_INVALID；算法不支持抛 E_SIGNATURE_ALGORITHM_UNSUPPORTED。
void verify_signature(void* key, std::string_view algorithm, const Bytes& digest, const Bytes& signature);

}  // namespace msixpkg
