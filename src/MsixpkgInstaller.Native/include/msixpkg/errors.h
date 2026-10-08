#pragma once

#include <stdexcept>
#include <string>
#include <utility>

namespace msixpkg {

// 错误码必须与 src/Msixpkg.Core/MsixpkgErrors.cs、docs/msixpkg-format-v1.md §8 逐字一致。
// 本地化只针对展示文案，错误码本身不得翻译或改写。
namespace codes {

inline constexpr char ContainerInvalid[] = "E_CONTAINER_INVALID";
inline constexpr char ContainerEmpty[] = "E_CONTAINER_EMPTY";
inline constexpr char ContainerTooLarge[] = "E_CONTAINER_TOO_LARGE";
inline constexpr char EntryCountExceeded[] = "E_ENTRY_COUNT_EXCEEDED";
inline constexpr char EntryTooLarge[] = "E_ENTRY_TOO_LARGE";
inline constexpr char EntryRatioExceeded[] = "E_ENTRY_RATIO_EXCEEDED";
inline constexpr char TotalSizeExceeded[] = "E_TOTAL_SIZE_EXCEEDED";
inline constexpr char EntryNameInvalid[] = "E_ENTRY_NAME_INVALID";
inline constexpr char EntryDuplicate[] = "E_ENTRY_DUPLICATE";
inline constexpr char EntryUnknown[] = "E_ENTRY_UNKNOWN";
inline constexpr char ManifestMissing[] = "E_MANIFEST_MISSING";
inline constexpr char ManifestNotFirst[] = "E_MANIFEST_NOT_FIRST";
inline constexpr char ManifestSchema[] = "E_MANIFEST_SCHEMA";
inline constexpr char FormatVersionUnsupported[] = "E_FORMAT_VERSION_UNSUPPORTED";
inline constexpr char InstallerTooOld[] = "E_INSTALLER_TOO_OLD";
inline constexpr char OsTooOld[] = "E_OS_TOO_OLD";
inline constexpr char ArchitectureMismatch[] = "E_ARCHITECTURE_MISMATCH";
inline constexpr char FileMissing[] = "E_FILE_MISSING";
inline constexpr char HashMismatch[] = "E_HASH_MISMATCH";
inline constexpr char SizeMismatch[] = "E_SIZE_MISMATCH";
inline constexpr char SignatureMissing[] = "E_SIGNATURE_MISSING";
inline constexpr char SignatureMalformed[] = "E_SIGNATURE_MALFORMED";
inline constexpr char SignatureAlgorithmUnsupported[] = "E_SIGNATURE_ALGORITHM_UNSUPPORTED";
inline constexpr char SignatureInvalid[] = "E_SIGNATURE_INVALID";
inline constexpr char SignatureThumbprintMismatch[] = "E_SIGNATURE_THUMBPRINT_MISMATCH";
inline constexpr char CertificateMissing[] = "E_CERTIFICATE_MISSING";
inline constexpr char CertificateMalformed[] = "E_CERTIFICATE_MALFORMED";
inline constexpr char CertificateExpired[] = "E_CERTIFICATE_EXPIRED";
inline constexpr char CertificateNotYetValid[] = "E_CERTIFICATE_NOT_YET_VALID";
inline constexpr char CertificateNotCodeSigning[] = "E_CERTIFICATE_NOT_CODE_SIGNING";
inline constexpr char PublisherMismatch[] = "E_PUBLISHER_MISMATCH";
inline constexpr char NotPinned[] = "E_PUBLISHER_NOT_PINNED";
inline constexpr char AppxInvalid[] = "E_APPX_INVALID";
inline constexpr char AppxManifestMissing[] = "E_APPX_MANIFEST_MISSING";
inline constexpr char AppxSignatureMissing[] = "E_APPX_SIGNATURE_MISSING";
inline constexpr char AppxSignerMismatch[] = "E_APPX_SIGNER_MISMATCH";
inline constexpr char AppxPublisherMismatch[] = "E_APPX_PUBLISHER_MISMATCH";
inline constexpr char AppxVersionMismatch[] = "E_APPX_VERSION_MISMATCH";
inline constexpr char AppxArchitectureMismatch[] = "E_APPX_ARCHITECTURE_MISMATCH";
inline constexpr char IoError[] = "E_IO";

}  // namespace codes

/// 所有可预期的失败都通过它抛出；调用方按 code() 分支，按 what()/detail() 记日志。
class Error final : public std::runtime_error {
public:
    Error(std::string code, std::string message, std::string detail = {})
        : std::runtime_error(message),
          code_(std::move(code)),
          message_(std::move(message)),
          detail_(std::move(detail)) {}

    [[nodiscard]] const std::string& code() const noexcept { return code_; }
    [[nodiscard]] const std::string& message() const noexcept { return message_; }
    [[nodiscard]] const std::string& detail() const noexcept { return detail_; }

    /// "CODE: message [detail]"，用于日志与命令行输出。
    [[nodiscard]] std::string to_string() const {
        std::string result = code_ + ": " + message_;
        if (!detail_.empty()) {
            result += " [" + detail_ + "]";
        }
        return result;
    }

private:
    std::string code_;
    std::string message_;
    std::string detail_;
};

[[noreturn]] inline void fail(std::string code, std::string message, std::string detail = {}) {
    throw Error(std::move(code), std::move(message), std::move(detail));
}

}  // namespace msixpkg
