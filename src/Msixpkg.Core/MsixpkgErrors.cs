using System;

namespace Msixpkg.Core
{
    /// <summary>
    /// 稳定的错误码。GUI 依据 Code 做本地化，Message 面向日志与命令行，两者都不应随版本静默改变。
    /// 这些字符串与 docs/msixpkg-format-v1.md 及原生实现必须逐字一致。
    /// </summary>
    public static class MsixpkgErrorCodes
    {
        public const string ContainerInvalid = "E_CONTAINER_INVALID";
        public const string ContainerEmpty = "E_CONTAINER_EMPTY";
        public const string ContainerTooLarge = "E_CONTAINER_TOO_LARGE";
        public const string EntryCountExceeded = "E_ENTRY_COUNT_EXCEEDED";
        public const string EntryTooLarge = "E_ENTRY_TOO_LARGE";
        public const string EntryRatioExceeded = "E_ENTRY_RATIO_EXCEEDED";
        public const string TotalSizeExceeded = "E_TOTAL_SIZE_EXCEEDED";
        public const string EntryNameInvalid = "E_ENTRY_NAME_INVALID";
        public const string EntryDuplicate = "E_ENTRY_DUPLICATE";
        public const string EntryUnknown = "E_ENTRY_UNKNOWN";
        public const string ManifestMissing = "E_MANIFEST_MISSING";
        public const string ManifestNotFirst = "E_MANIFEST_NOT_FIRST";
        public const string ManifestSchema = "E_MANIFEST_SCHEMA";
        public const string FormatVersionUnsupported = "E_FORMAT_VERSION_UNSUPPORTED";
        public const string InstallerTooOld = "E_INSTALLER_TOO_OLD";
        public const string OsTooOld = "E_OS_TOO_OLD";
        public const string ArchitectureMismatch = "E_ARCHITECTURE_MISMATCH";
        public const string FileMissing = "E_FILE_MISSING";
        public const string HashMismatch = "E_HASH_MISMATCH";
        public const string SizeMismatch = "E_SIZE_MISMATCH";
        public const string SignatureMissing = "E_SIGNATURE_MISSING";
        public const string SignatureMalformed = "E_SIGNATURE_MALFORMED";
        public const string SignatureAlgorithmUnsupported = "E_SIGNATURE_ALGORITHM_UNSUPPORTED";
        public const string SignatureInvalid = "E_SIGNATURE_INVALID";
        public const string SignatureThumbprintMismatch = "E_SIGNATURE_THUMBPRINT_MISMATCH";
        public const string CertificateMissing = "E_CERTIFICATE_MISSING";
        public const string CertificateMalformed = "E_CERTIFICATE_MALFORMED";
        public const string CertificateExpired = "E_CERTIFICATE_EXPIRED";
        public const string CertificateNotYetValid = "E_CERTIFICATE_NOT_YET_VALID";
        public const string CertificateNotCodeSigning = "E_CERTIFICATE_NOT_CODE_SIGNING";
        public const string PublisherMismatch = "E_PUBLISHER_MISMATCH";
        public const string NotPinned = "E_PUBLISHER_NOT_PINNED";
        public const string AppxInvalid = "E_APPX_INVALID";
        public const string AppxManifestMissing = "E_APPX_MANIFEST_MISSING";
        public const string AppxSignatureMissing = "E_APPX_SIGNATURE_MISSING";
        public const string AppxSignerMismatch = "E_APPX_SIGNER_MISMATCH";
        public const string AppxPublisherMismatch = "E_APPX_PUBLISHER_MISMATCH";
        public const string AppxVersionMismatch = "E_APPX_VERSION_MISMATCH";
        public const string AppxArchitectureMismatch = "E_APPX_ARCHITECTURE_MISMATCH";
        public const string IoError = "E_IO";
    }

    /// <summary>格式或校验失败。Code 为稳定错误码，Detail 为可选的补充上下文。</summary>
    public sealed class MsixpkgFormatException : Exception
    {
        public MsixpkgFormatException(string code, string message)
            : base(message)
        {
            Code = code;
        }

        public MsixpkgFormatException(string code, string message, string detail)
            : base(message)
        {
            Code = code;
            Detail = detail;
        }

        public MsixpkgFormatException(string code, string message, Exception inner)
            : base(message, inner)
        {
            Code = code;
        }

        public string Code { get; }

        public string? Detail { get; set; }
    }
}
