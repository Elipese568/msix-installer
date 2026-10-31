#pragma once

// 载荷校验用的 SHA-256（BCrypt）。setup 只链接 bcrypt，不拖 crypt32 / WinRT。

#include <Windows.h>
#include <bcrypt.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace msixpkg::setup {

class Sha256 {
public:
    Sha256();
    ~Sha256();

    Sha256(const Sha256&) = delete;
    Sha256& operator=(const Sha256&) = delete;

    bool valid() const { return hash_ != nullptr; }
    bool update(const void* data, size_t size);
    std::array<uint8_t, 32> finish();

private:
    BCRYPT_ALG_HANDLE algorithm_ = nullptr;
    BCRYPT_HASH_HANDLE hash_ = nullptr;
    bool finished_ = false;
};

std::array<uint8_t, 32> sha256_of(const void* data, size_t size);
std::wstring to_hex(const std::array<uint8_t, 32>& digest);

}  // namespace msixpkg::setup
