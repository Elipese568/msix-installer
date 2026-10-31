#include "setup_sha256.h"

#include <cwchar>

namespace msixpkg::setup {

Sha256::Sha256() {
    if (!BCRYPT_SUCCESS(::BCryptOpenAlgorithmProvider(&algorithm_, BCRYPT_SHA256_ALGORITHM, nullptr, 0))) {
        algorithm_ = nullptr;
        return;
    }
    if (!BCRYPT_SUCCESS(::BCryptCreateHash(algorithm_, &hash_, nullptr, 0, nullptr, 0, 0))) {
        hash_ = nullptr;
        ::BCryptCloseAlgorithmProvider(algorithm_, 0);
        algorithm_ = nullptr;
    }
}

Sha256::~Sha256() {
    if (hash_ != nullptr) {
        ::BCryptDestroyHash(hash_);
    }
    if (algorithm_ != nullptr) {
        ::BCryptCloseAlgorithmProvider(algorithm_, 0);
    }
}

bool Sha256::update(const void* data, size_t size) {
    if (hash_ == nullptr || finished_ || (data == nullptr && size != 0)) {
        return false;
    }
    if (size == 0) {
        return true;
    }
    const auto* bytes = static_cast<const unsigned char*>(data);
    size_t offset = 0;
    while (offset < size) {
        const size_t chunk = (size - offset) < (1u << 30) ? (size - offset) : (1u << 30);
        if (!BCRYPT_SUCCESS(::BCryptHashData(hash_, const_cast<PUCHAR>(bytes + offset),
                                            static_cast<ULONG>(chunk), 0))) {
            return false;
        }
        offset += chunk;
    }
    return true;
}

std::array<uint8_t, 32> Sha256::finish() {
    std::array<uint8_t, 32> digest{};
    if (hash_ == nullptr || finished_) {
        return digest;
    }
    finished_ = true;
    if (!BCRYPT_SUCCESS(::BCryptFinishHash(hash_, digest.data(), static_cast<ULONG>(digest.size()), 0))) {
        digest.fill(0);
    }
    return digest;
}

std::array<uint8_t, 32> sha256_of(const void* data, size_t size) {
    Sha256 hash;
    if (!hash.update(data, size)) {
        return {};
    }
    return hash.finish();
}

std::wstring to_hex(const std::array<uint8_t, 32>& digest) {
    static constexpr wchar_t digits[] = L"0123456789abcdef";
    std::wstring text;
    text.reserve(digest.size() * 2);
    for (const uint8_t byte : digest) {
        text.push_back(digits[(byte >> 4) & 0x0F]);
        text.push_back(digits[byte & 0x0F]);
    }
    return text;
}

}  // namespace msixpkg::setup
