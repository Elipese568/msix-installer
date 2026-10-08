using System;
using System.Globalization;

namespace Msixpkg.Core
{
    public enum MsixpkgEntryKind
    {
        Manifest,
        Signature,
        MainPackage,
        Dependency,
        PublisherCertificate,
        PublisherChain,
    }

    /// <summary>容器内的条目命名规则（规范性的，必须与原生实现一致）。</summary>
    public static class MsixpkgPaths
    {
        public const string ManifestEntry = "msixpkg.json";
        public const string SignatureEntry = "msixpkg.sig";
        public const string PayloadPrefix = "payload/";
        public const string MainSegment = "/main/";
        public const string DependencySegment = "/deps/";
        public const string PublisherPrefix = "publisher/";
        public const string PublisherCertificateEntry = "publisher/publisher.cer";
        public const string PublisherChainEntry = "publisher/chain.p7b";
        public const string MsixExtension = ".msix";

        public const int MaxEntryNameLength = 512;

        /// <summary>架构代号；用于条目路径，必须与 Appx 清单的 ProcessorArchitecture 一致。</summary>
        public static bool IsKnownArchitecture(string architecture)
        {
            return string.Equals(architecture, "x64", StringComparison.Ordinal)
                || string.Equals(architecture, "x86", StringComparison.Ordinal)
                || string.Equals(architecture, "arm64", StringComparison.Ordinal)
                || string.Equals(architecture, "neutral", StringComparison.Ordinal);
        }

        public static string MainPackageEntry(string architecture, string identityName, string version)
        {
            return PayloadPrefix
                + RequireSegment(architecture, nameof(architecture))
                + "/main/"
                + RequireSegment(identityName, nameof(identityName))
                + "_"
                + RequireSegment(version, nameof(version))
                + "_"
                + architecture
                + MsixExtension;
        }

        public static string DependencyEntry(string architecture, string packageName, string version)
        {
            return PayloadPrefix
                + RequireSegment(architecture, nameof(architecture))
                + "/deps/"
                + RequireSegment(packageName, nameof(packageName))
                + "_"
                + RequireSegment(version, nameof(version))
                + "_"
                + architecture
                + MsixExtension;
        }

        /// <summary>校验单个条目名；不合法时抛 <see cref="MsixpkgFormatException"/>。</summary>
        public static void ValidateEntryName(string name)
        {
            if (string.IsNullOrEmpty(name))
            {
                throw new MsixpkgFormatException(MsixpkgErrorCodes.EntryNameInvalid, "An entry name is empty.");
            }

            if (name.Length > MaxEntryNameLength)
            {
                throw new MsixpkgFormatException(
                    MsixpkgErrorCodes.EntryNameInvalid,
                    "Entry name exceeds " + MaxEntryNameLength.ToString(CultureInfo.InvariantCulture) + " characters.");
            }

            if (name[0] == '/' || name[name.Length - 1] == '/')
            {
                throw new MsixpkgFormatException(
                    MsixpkgErrorCodes.EntryNameInvalid,
                    "Entry name must not start or end with '/'.",
                    name);
            }

            if (name.IndexOf('\\') >= 0)
            {
                throw new MsixpkgFormatException(
                    MsixpkgErrorCodes.EntryNameInvalid,
                    "Entry name must not contain a backslash.",
                    name);
            }

            for (int i = 0; i < name.Length; i++)
            {
                char c = name[i];
                if (c < 0x20 || c == 0x7F)
                {
                    throw new MsixpkgFormatException(
                        MsixpkgErrorCodes.EntryNameInvalid,
                        "Entry name contains a control character.",
                        name);
                }

                switch (c)
                {
                    case ':':
                    case '*':
                    case '?':
                    case '"':
                    case '<':
                    case '>':
                    case '|':
                        throw new MsixpkgFormatException(
                            MsixpkgErrorCodes.EntryNameInvalid,
                            "Entry name contains a reserved character '" + c + "'.",
                            name);
                }
            }

            string[] segments = name.Split('/');
            for (int i = 0; i < segments.Length; i++)
            {
                string segment = segments[i];
                if (segment.Length == 0)
                {
                    throw new MsixpkgFormatException(
                        MsixpkgErrorCodes.EntryNameInvalid,
                        "Entry name contains an empty path segment.",
                        name);
                }

                if (segment == "." || segment == "..")
                {
                    throw new MsixpkgFormatException(
                        MsixpkgErrorCodes.EntryNameInvalid,
                        "Entry name contains a relative path segment.",
                        name);
                }

                if (segment.Length > 1 && segment[segment.Length - 1] == '.')
                {
                    // Windows 会静默去掉结尾的点/空格，会造成同一路径的两种写法。
                    throw new MsixpkgFormatException(
                        MsixpkgErrorCodes.EntryNameInvalid,
                        "Entry name segment ends with '.'.",
                        name);
                }
            }
        }

        /// <summary>按白名单判定条目类别；不在白名单内时抛 E_ENTRY_UNKNOWN。</summary>
        public static MsixpkgEntryKind Classify(string name)
        {
            ValidateEntryName(name);

            if (string.Equals(name, ManifestEntry, StringComparison.Ordinal))
            {
                return MsixpkgEntryKind.Manifest;
            }

            if (string.Equals(name, SignatureEntry, StringComparison.Ordinal))
            {
                return MsixpkgEntryKind.Signature;
            }

            if (string.Equals(name, PublisherCertificateEntry, StringComparison.OrdinalIgnoreCase))
            {
                if (!string.Equals(name, PublisherCertificateEntry, StringComparison.Ordinal))
                {
                    throw new MsixpkgFormatException(
                        MsixpkgErrorCodes.EntryUnknown,
                        "Entry name differs only by case from a reserved name.",
                        name);
                }

                return MsixpkgEntryKind.PublisherCertificate;
            }

            if (string.Equals(name, PublisherChainEntry, StringComparison.Ordinal))
            {
                return MsixpkgEntryKind.PublisherChain;
            }

            if (name.StartsWith(PayloadPrefix, StringComparison.Ordinal) && name.EndsWith(MsixExtension, StringComparison.Ordinal))
            {
                string[] segments = name.Split('/');
                // payload/<arch>/{main|deps}/<file>.msix
                if (segments.Length == 4 && segments[1].Length > 0 && segments[3].Length > MsixExtension.Length)
                {
                    if (string.Equals(segments[2], "main", StringComparison.Ordinal))
                    {
                        return MsixpkgEntryKind.MainPackage;
                    }

                    if (string.Equals(segments[2], "deps", StringComparison.Ordinal))
                    {
                        return MsixpkgEntryKind.Dependency;
                    }
                }
            }

            throw new MsixpkgFormatException(
                MsixpkgErrorCodes.EntryUnknown,
                "Entry is not part of the msixpkg v1 whitelist.",
                name);
        }

        private static string RequireSegment(string value, string parameterName)
        {
            if (string.IsNullOrEmpty(value))
            {
                throw new ArgumentException("Value must not be empty.", parameterName);
            }

            for (int i = 0; i < value.Length; i++)
            {
                char c = value[i];
                if (c == '/' || c == '\\' || c == ':' || c < 0x20)
                {
                    throw new ArgumentException("Value contains an invalid character '" + c + "'.", parameterName);
                }
            }

            return value;
        }
    }
}
