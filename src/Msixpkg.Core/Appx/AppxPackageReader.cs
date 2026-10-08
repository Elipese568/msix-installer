using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.IO.Compression;
using System.Security.Cryptography.X509Certificates;
using System.Xml.Linq;

namespace Msixpkg.Core.Appx
{
    public sealed class AppxDependency
    {
        public AppxDependency(string name, string minVersion)
        {
            Name = name;
            MinVersion = minVersion;
        }

        public string Name { get; }

        public string MinVersion { get; }
    }

    public sealed class AppxPackageInfo
    {
        public AppxPackageInfo(
            string name,
            string publisher,
            string version,
            string architecture,
            string? displayName,
            string? applicationId)
        {
            Name = name;
            Publisher = publisher;
            Version = version;
            Architecture = architecture;
            DisplayName = displayName;
            ApplicationId = applicationId;
        }

        public string Name { get; }

        public string Publisher { get; }

        public string Version { get; }

        public string Architecture { get; }

        /// <summary>Properties/DisplayName 原样值，可能是 "ms-resource:..."。</summary>
        public string? DisplayName { get; }

        /// <summary>第一个 Application/@Id，用于拼 AUMID（PFN + "!" + Id）。</summary>
        public string? ApplicationId { get; }

        public List<AppxDependency> Dependencies { get; } = new List<AppxDependency>();
    }

    public static class AppxPackageReader
    {
        public const string ManifestEntryName = "AppxManifest.xml";
        public const string SignatureEntryName = "AppxSignature.p7x";
        public const string BlockMapEntryName = "AppxBlockMap.xml";

        public static AppxPackageInfo ReadManifest(string msixPath)
        {
            using ZipArchive archive = OpenArchive(msixPath);
            ZipArchiveEntry entry = archive.GetEntry(ManifestEntryName)
                ?? throw new MsixpkgFormatException(
                    MsixpkgErrorCodes.AppxManifestMissing,
                    "The MSIX package has no AppxManifest.xml.",
                    msixPath);

            XDocument document;
            using (Stream stream = entry.Open())
            {
                try
                {
                    document = XDocument.Load(stream);
                }
                catch (System.Xml.XmlException ex)
                {
                    throw new MsixpkgFormatException(MsixpkgErrorCodes.AppxInvalid, "AppxManifest.xml is not valid XML.", ex.Message);
                }
            }

            XElement root = document.Root
                ?? throw new MsixpkgFormatException(MsixpkgErrorCodes.AppxInvalid, "AppxManifest.xml has no root element.");

            XElement identity = FindChild(root, "Identity")
                ?? throw new MsixpkgFormatException(MsixpkgErrorCodes.AppxInvalid, "AppxManifest.xml has no Identity element.");

            var info = new AppxPackageInfo(
                RequireAttribute(identity, "Name"),
                RequireAttribute(identity, "Publisher"),
                RequireAttribute(identity, "Version"),
                RequireAttribute(identity, "ProcessorArchitecture"),
                FindChild(FindChild(root, "Properties"), "DisplayName")?.Value,
                FindChild(FindChild(root, "Applications"), "Application")?.Attribute("Id")?.Value);

            XElement? dependencies = FindChild(root, "Dependencies");
            if (dependencies != null)
            {
                foreach (XElement element in dependencies.Elements())
                {
                    if (!string.Equals(element.Name.LocalName, "PackageDependency", StringComparison.Ordinal))
                    {
                        continue;
                    }

                    string? name = element.Attribute("Name")?.Value;
                    string? minVersion = element.Attribute("MinVersion")?.Value;
                    if (!string.IsNullOrEmpty(name))
                    {
                        info.Dependencies.Add(new AppxDependency(name!, minVersion ?? "0.0.0.0"));
                    }
                }
            }

            if (!MsixpkgPaths.IsKnownArchitecture(NormalizeArchitecture(info.Architecture)))
            {
                throw new MsixpkgFormatException(
                    MsixpkgErrorCodes.AppxInvalid,
                    "AppxManifest.xml declares an unsupported ProcessorArchitecture.",
                    info.Architecture);
            }

            return info;
        }

        /// <summary>把 x86/amd64 等 Appx 写法归一成 msixpkg 的架构代号。</summary>
        public static string NormalizeArchitecture(string architecture)
        {
            switch (architecture.Trim().ToLowerInvariant())
            {
                case "x64":
                case "amd64":
                    return "x64";
                case "x86":
                    return "x86";
                case "arm64":
                    return "arm64";
                case "neutral":
                    return "neutral";
                default:
                    return architecture.Trim().ToLowerInvariant();
            }
        }

        public static bool HasSignature(string msixPath)
        {
            using ZipArchive archive = OpenArchive(msixPath);
            return archive.GetEntry(SignatureEntryName) != null;
        }

        /// <summary>
        /// 从 .msix 的 AppxSignature.p7x 取出证书集合。
        /// 真实 Appx 签名是 AppxSip 的 SignedData（encapContentInfo 指向自定义摘要结构），
        /// Windows 的证书集合导入路径解析不了它，所以先走自己的 DER 解析；
        /// 证书袋形态（测试夹具、部分工具产物）再退回 Import。
        /// 密码学校验交给 Windows 部署栈（AddPackageByUriAsync）。
        /// </summary>
        public static X509Certificate2Collection ReadSignerCertificates(string msixPath)
        {
            using ZipArchive archive = OpenArchive(msixPath);
            ZipArchiveEntry entry = archive.GetEntry(SignatureEntryName)
                ?? throw new MsixpkgFormatException(
                    MsixpkgErrorCodes.AppxSignatureMissing,
                    "The MSIX package is not signed (AppxSignature.p7x is missing).",
                    msixPath);

            byte[] raw;
            using (Stream stream = entry.Open())
            using (var buffer = new MemoryStream())
            {
                stream.CopyTo(buffer);
                raw = buffer.ToArray();
            }

            try
            {
                IReadOnlyList<X509Certificate2> parsed = Pkcs7CertificateReader.ExtractCertificates(raw);
                var parsedCollection = new X509Certificate2Collection();
                foreach (X509Certificate2 certificate in parsed)
                {
                    parsedCollection.Add(certificate);
                }

                return parsedCollection;
            }
            catch (MsixpkgFormatException derFailure)
            {
                var certificates = new X509Certificate2Collection();
                try
                {
                    certificates.Import(raw);
                }
                catch (System.Security.Cryptography.CryptographicException)
                {
                    throw derFailure;
                }

                if (certificates.Count == 0)
                {
                    throw derFailure;
                }

                return certificates;
            }
        }

        private static ZipArchive OpenArchive(string msixPath)
        {
            if (string.IsNullOrEmpty(msixPath))
            {
                throw new ArgumentException("MSIX path is required.", nameof(msixPath));
            }

            if (!File.Exists(msixPath))
            {
                throw new MsixpkgFormatException(MsixpkgErrorCodes.IoError, "The MSIX file does not exist.", msixPath);
            }

            try
            {
                // 刻意用 FileStream + ZipArchive，而不依赖 ZipFile（.NET Framework 宿主下少一个程序集依赖）。
                var stream = new FileStream(msixPath, FileMode.Open, FileAccess.Read, FileShare.Read);
                try
                {
                    return new ZipArchive(stream, ZipArchiveMode.Read);
                }
                catch
                {
                    stream.Dispose();
                    throw;
                }
            }
            catch (InvalidDataException ex)
            {
                throw new MsixpkgFormatException(MsixpkgErrorCodes.AppxInvalid, "The MSIX file is not a valid ZIP-based package.", ex.Message);
            }
        }

        private static XElement? FindChild(XElement? parent, string localName)
        {
            if (parent == null)
            {
                return null;
            }

            foreach (XElement element in parent.Elements())
            {
                if (string.Equals(element.Name.LocalName, localName, StringComparison.Ordinal))
                {
                    return element;
                }
            }

            return null;
        }

        private static string RequireAttribute(XElement element, string name)
        {
            string? value = element.Attribute(name)?.Value;
            if (string.IsNullOrEmpty(value))
            {
                throw new MsixpkgFormatException(
                    MsixpkgErrorCodes.AppxInvalid,
                    "AppxManifest.xml Identity is missing the '" + name + "' attribute.");
            }

            return value!;
        }

        /// <summary>按 msixpkg 的规则格式化四段版本，供依赖条目命名使用。</summary>
        public static string NormalizeVersion(string version)
        {
            string[] parts = version.Split('.');
            if (parts.Length >= 4)
            {
                return version;
            }

            var padded = new List<string>(parts);
            while (padded.Count < 4)
            {
                padded.Add("0");
            }

            return string.Join(".", padded);
        }

        internal static string Describe(X509Certificate2 certificate)
        {
            return string.Format(
                CultureInfo.InvariantCulture,
                "{0} ({1})",
                certificate.Subject,
                Thumbprints.Sha256Hex(certificate));
        }
    }
}
