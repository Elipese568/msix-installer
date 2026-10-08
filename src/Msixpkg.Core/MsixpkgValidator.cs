using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Security.Cryptography.X509Certificates;
using Msixpkg.Core.Appx;

namespace Msixpkg.Core
{
    public sealed class MsixpkgValidateOptions
    {
        /// <summary>安装器版本，用于 installerMinVersion 门禁；null 表示跳过。</summary>
        public string? InstallerVersion { get; set; }

        /// <summary>本机 Windows 构建号，用于 minOsBuild 门禁；null 表示跳过。</summary>
        public int? HostBuildNumber { get; set; }

        /// <summary>本机架构（x64/x86/arm64）；null 表示跳过。</summary>
        public string? HostArchitecture { get; set; }

        /// <summary>允许的发布者 SHA-256 指纹白名单（小写）。null 表示不做钉扎判定。</summary>
        public IReadOnlyCollection<string>? AllowedPublisherThumbprints { get; set; }

        /// <summary>为 true 时，未命中钉扎（或没有可钉扎的证书）一律判为失败。</summary>
        public bool RequirePinnedPublisher { get; set; }

        public bool RequireSignature { get; set; } = true;

        /// <summary>安装器已经解包好的主包路径；提供时直接复用它做内部 Appx 校验，避免二次解包。</summary>
        public string? ExtractedMainPackagePath { get; set; }

        public bool ValidateInnerAppx { get; set; } = true;

        public string? TempDirectory { get; set; }

        public DateTime? NowUtc { get; set; }
    }

    public sealed class MsixpkgValidationIssue
    {
        public MsixpkgValidationIssue(string code, string message)
            : this(code, message, null)
        {
        }

        public MsixpkgValidationIssue(string code, string message, string? detail)
        {
            Code = code;
            Message = message;
            Detail = detail;
        }

        public string Code { get; }

        public string Message { get; }

        public string? Detail { get; }

        public override string ToString()
        {
            return Detail == null ? Code + ": " + Message : Code + ": " + Message + " [" + Detail + "]";
        }
    }

    public sealed class MsixpkgValidationReport
    {
        internal MsixpkgValidationReport()
        {
        }

        public List<MsixpkgValidationIssue> Errors { get; } = new List<MsixpkgValidationIssue>();

        public List<MsixpkgValidationIssue> Warnings { get; } = new List<MsixpkgValidationIssue>();

        public bool IsValid => Errors.Count == 0;

        public MsixpkgManifest? Manifest { get; internal set; }

        public X509Certificate2? PublisherCertificate { get; internal set; }

        public bool PublisherCertificateWasPinned { get; internal set; }

        public string? ContentDigestHex { get; internal set; }

        public override string ToString()
        {
            if (IsValid)
            {
                return "valid" + (Warnings.Count > 0 ? " (" + Warnings.Count.ToString(CultureInfo.InvariantCulture) + " warning(s))" : string.Empty);
            }

            return Errors[0].ToString();
        }
    }

    /// <summary>
    /// 安装器侧的完整校验序列。打包器（Msixpkg.Packer verify）与原生安装器都必须实现同一序列，
    /// 规则以 docs/msixpkg-format-v1.md 为准。
    /// </summary>
    public static class MsixpkgValidator
    {
        public const string CodeSigningEkuOid = "1.3.6.1.5.5.7.3.3";

        public static MsixpkgValidationReport Validate(string containerPath, MsixpkgValidateOptions? options = null)
        {
            options ??= new MsixpkgValidateOptions();
            var report = new MsixpkgValidationReport();
            DateTime nowUtc = options.NowUtc ?? DateTime.UtcNow;
            string? extractedMain = null;
            bool deleteExtracted = false;

            try
            {
                using MsixpkgContainer container = MsixpkgContainer.Open(containerPath);

                // 清单必须存在且是第一个条目。
                if (container.Entries.Count == 0 || container.Entries[0].Kind != MsixpkgEntryKind.Manifest)
                {
                    report.Errors.Add(new MsixpkgValidationIssue(
                        MsixpkgErrorCodes.ManifestNotFirst,
                        "msixpkg.json must be present and be the first entry in the container.",
                        container.Entries.Count > 0 ? container.Entries[0].Name : null));
                    return report;
                }

                MsixpkgManifest manifest;
                try
                {
                    manifest = container.ReadManifest();
                }
                catch (MsixpkgFormatException ex)
                {
                    report.Errors.Add(ToIssue(ex));
                    return report;
                }

                report.Manifest = manifest;

                // 条目集合必须与清单声明完全一致。
                RequireDeclaredEntries(container, manifest, report);

                // 安装器版本与操作系统版本门禁。
                if (options.InstallerVersion != null && !IsVersionAtLeast(options.InstallerVersion, manifest.InstallerMinVersion))
                {
                    report.Errors.Add(new MsixpkgValidationIssue(
                        MsixpkgErrorCodes.InstallerTooOld,
                        "This installer is older than the minimum required by the package.",
                        manifest.InstallerMinVersion + " > " + options.InstallerVersion));
                }

                if (options.HostBuildNumber.HasValue && options.HostBuildNumber.Value < manifest.MinOsBuild)
                {
                    report.Errors.Add(new MsixpkgValidationIssue(
                        MsixpkgErrorCodes.OsTooOld,
                        "The operating system is older than the minimum required by the package.",
                        manifest.MinOsBuild.ToString(CultureInfo.InvariantCulture) + " > " + options.HostBuildNumber.Value.ToString(CultureInfo.InvariantCulture)));
                }

                if (!string.IsNullOrEmpty(options.HostArchitecture) && !IsArchitectureCompatible(manifest.Identity.Architecture, options.HostArchitecture!))
                {
                    report.Errors.Add(new MsixpkgValidationIssue(
                        MsixpkgErrorCodes.ArchitectureMismatch,
                        "The package architecture is not compatible with this machine.",
                        manifest.Identity.Architecture + " vs " + options.HostArchitecture));
                }

                // 逐条校验声明的载荷文件（存在性、大小、SHA-256）。
                VerifyDeclaredFile(container, manifest.MainPackage, MsixpkgErrorCodes.FileMissing, report);
                foreach (MsixpkgFileRef dependency in manifest.Dependencies)
                {
                    VerifyDeclaredFile(container, dependency, MsixpkgErrorCodes.FileMissing, report);
                }

                // 发布者证书。
                if (manifest.Publisher != null)
                {
                    VerifyPublisherCertificate(container, manifest, options, report, nowUtc);
                }

                // 容器签名。
                VerifyContainerSignature(container, manifest, options, report);

                // 内部 Appx 校验。
                if (options.ValidateInnerAppx && report.Errors.Count == 0)
                {
                    if (options.ExtractedMainPackagePath != null)
                    {
                        // 复用安装器已解包的主包：必须重新核对大小与哈希，否则校验与部署之间存在 TOCTOU 窗口。
                        extractedMain = options.ExtractedMainPackagePath;
                        VerifyLocalFile(extractedMain, manifest.MainPackage, report);
                    }
                    else
                    {
                        string tempDirectory = options.TempDirectory ?? Path.GetTempPath();
                        Directory.CreateDirectory(tempDirectory);
                        extractedMain = Path.Combine(tempDirectory, "msixpkg-validate-" + Guid.NewGuid().ToString("n") + ".msix");
                        container.ExtractEntryToFile(manifest.MainPackage.Path, extractedMain);
                        deleteExtracted = true;
                    }

                    if (report.Errors.Count == 0 && extractedMain != null)
                    {
                        VerifyInnerAppx(extractedMain, manifest, report);
                    }
                }
            }
            catch (MsixpkgFormatException ex)
            {
                report.Errors.Add(ToIssue(ex));
            }
            catch (IOException ex)
            {
                report.Errors.Add(new MsixpkgValidationIssue(MsixpkgErrorCodes.IoError, "An I/O error occurred while validating the container.", ex.Message));
            }
            catch (UnauthorizedAccessException ex)
            {
                report.Errors.Add(new MsixpkgValidationIssue(MsixpkgErrorCodes.IoError, "Access was denied while validating the container.", ex.Message));
            }
            finally
            {
                if (deleteExtracted && extractedMain != null)
                {
                    TryDelete(extractedMain);
                }
            }

            return report;
        }

        public static void ThrowIfInvalid(string containerPath, MsixpkgValidateOptions? options = null)
        {
            MsixpkgValidationReport report = Validate(containerPath, options);
            if (!report.IsValid)
            {
                MsixpkgValidationIssue first = report.Errors[0];
                throw new MsixpkgFormatException(first.Code, first.Message, first.Detail ?? string.Empty);
            }
        }

        private static void RequireDeclaredEntries(MsixpkgContainer container, MsixpkgManifest manifest, MsixpkgValidationReport report)
        {
            var declared = new HashSet<string>(StringComparer.OrdinalIgnoreCase)
            {
                MsixpkgPaths.ManifestEntry,
            };

            declared.Add(manifest.MainPackage.Path);
            foreach (MsixpkgFileRef dependency in manifest.Dependencies)
            {
                declared.Add(dependency.Path);
            }

            if (manifest.Publisher != null)
            {
                declared.Add(manifest.Publisher.CertPath);
            }

            foreach (MsixpkgContainerEntry entry in container.Entries)
            {
                if (entry.Kind == MsixpkgEntryKind.Signature)
                {
                    continue;
                }

                if (!declared.Contains(entry.Name))
                {
                    report.Errors.Add(new MsixpkgValidationIssue(
                        MsixpkgErrorCodes.EntryUnknown,
                        "The container carries an entry that the manifest does not declare.",
                        entry.Name));
                }
            }
        }

        private static void VerifyDeclaredFile(MsixpkgContainer container, MsixpkgFileRef fileRef, string missingCode, MsixpkgValidationReport report)
        {
            MsixpkgContainerEntry? entry = container.Find(fileRef.Path);
            if (entry == null)
            {
                report.Errors.Add(new MsixpkgValidationIssue(missingCode, "A declared payload entry is missing.", fileRef.Path));
                return;
            }

            if (entry.UncompressedSize != fileRef.Size)
            {
                report.Errors.Add(new MsixpkgValidationIssue(
                    MsixpkgErrorCodes.SizeMismatch,
                    "A payload entry size does not match the manifest.",
                    fileRef.Path + ": " + entry.UncompressedSize.ToString(CultureInfo.InvariantCulture) + " != " + fileRef.Size.ToString(CultureInfo.InvariantCulture)));
                return;
            }

            string actual = container.ComputeEntrySha256(fileRef.Path);
            if (!string.Equals(actual, fileRef.Sha256, StringComparison.Ordinal))
            {
                report.Errors.Add(new MsixpkgValidationIssue(
                    MsixpkgErrorCodes.HashMismatch,
                    "A payload entry hash does not match the manifest.",
                    fileRef.Path + ": " + actual));
            }
        }

        private static void VerifyPublisherCertificate(
            MsixpkgContainer container,
            MsixpkgManifest manifest,
            MsixpkgValidateOptions options,
            MsixpkgValidationReport report,
            DateTime nowUtc)
        {
            MsixpkgPublisherInfo declared = manifest.Publisher!;
            MsixpkgContainerEntry? entry = container.Find(declared.CertPath);
            if (entry == null)
            {
                report.Errors.Add(new MsixpkgValidationIssue(MsixpkgErrorCodes.CertificateMissing, "The publisher certificate entry is missing.", declared.CertPath));
                return;
            }

            X509Certificate2 certificate;
            try
            {
                certificate = new X509Certificate2(container.ReadEntryBytes(declared.CertPath));
            }
            catch (System.Security.Cryptography.CryptographicException ex)
            {
                report.Errors.Add(new MsixpkgValidationIssue(MsixpkgErrorCodes.CertificateMalformed, "The publisher certificate could not be parsed.", ex.Message));
                return;
            }

            report.PublisherCertificate = certificate;

            string sha256 = Thumbprints.Sha256Hex(certificate);
            string sha1 = Thumbprints.Sha1Hex(certificate);

            if (!string.Equals(sha256, declared.ThumbprintSha256, StringComparison.Ordinal)
                || !string.Equals(sha1, declared.ThumbprintSha1, StringComparison.Ordinal))
            {
                report.Errors.Add(new MsixpkgValidationIssue(
                    MsixpkgErrorCodes.CertificateMalformed,
                    "The publisher certificate thumbprint does not match the manifest.",
                    sha256));
            }

            if (!string.Equals(certificate.Subject, declared.Subject, StringComparison.Ordinal))
            {
                report.Errors.Add(new MsixpkgValidationIssue(
                    MsixpkgErrorCodes.PublisherMismatch,
                    "The certificate subject does not match the manifest publisher entry.",
                    certificate.Subject + " != " + declared.Subject));
            }

            if (!string.Equals(certificate.Subject, manifest.Identity.Publisher, StringComparison.Ordinal))
            {
                report.Errors.Add(new MsixpkgValidationIssue(
                    MsixpkgErrorCodes.PublisherMismatch,
                    "The certificate subject does not match the package identity publisher.",
                    certificate.Subject + " != " + manifest.Identity.Publisher));
            }

            VerifyCertificateUsage(certificate, report);

            DateTime notBefore = certificate.NotBefore.ToUniversalTime();
            DateTime notAfter = certificate.NotAfter.ToUniversalTime();
            if (nowUtc < notBefore)
            {
                report.Errors.Add(new MsixpkgValidationIssue(
                    MsixpkgErrorCodes.CertificateNotYetValid,
                    "The publisher certificate is not yet valid.",
                    MsixpkgManifest.FormatIsoUtc(notBefore)));
            }
            else if (nowUtc > notAfter)
            {
                report.Errors.Add(new MsixpkgValidationIssue(
                    MsixpkgErrorCodes.CertificateExpired,
                    "The publisher certificate has expired.",
                    MsixpkgManifest.FormatIsoUtc(notAfter)));
            }
            else if (notAfter - nowUtc <= TimeSpan.FromDays(90))
            {
                report.Warnings.Add(new MsixpkgValidationIssue(
                    MsixpkgErrorCodes.CertificateExpired,
                    "The publisher certificate expires within 90 days.",
                    MsixpkgManifest.FormatIsoUtc(notAfter)));
            }

            // 钉扎判定。
            if (options.AllowedPublisherThumbprints != null)
            {
                report.PublisherCertificateWasPinned = ContainsThumbprint(options.AllowedPublisherThumbprints, sha256);
                if (!report.PublisherCertificateWasPinned)
                {
                    report.Errors.Add(new MsixpkgValidationIssue(
                        MsixpkgErrorCodes.NotPinned,
                        "The publisher is not in the allowed publisher list.",
                        sha256));
                }
            }
            else if (options.RequirePinnedPublisher)
            {
                report.Errors.Add(new MsixpkgValidationIssue(
                    MsixpkgErrorCodes.NotPinned,
                    "No allowed publisher list is configured, but pinning is required.",
                    sha256));
            }
        }

        private static void VerifyCertificateUsage(X509Certificate2 certificate, MsixpkgValidationReport report)
        {
            bool hasEku = false;
            bool hasCodeSigning = false;

            foreach (X509Extension extension in certificate.Extensions)
            {
                if (extension is X509EnhancedKeyUsageExtension enhanced)
                {
                    hasEku = true;
                    foreach (System.Security.Cryptography.Oid oid in enhanced.EnhancedKeyUsages)
                    {
                        if (string.Equals(oid.Value, CodeSigningEkuOid, StringComparison.Ordinal))
                        {
                            hasCodeSigning = true;
                            break;
                        }
                    }
                }
            }

            if (hasEku && !hasCodeSigning)
            {
                report.Errors.Add(new MsixpkgValidationIssue(
                    MsixpkgErrorCodes.CertificateNotCodeSigning,
                    "The publisher certificate is not valid for code signing (missing EKU).",
                    CodeSigningEkuOid));
            }
            else if (!hasEku)
            {
                report.Warnings.Add(new MsixpkgValidationIssue(
                    MsixpkgErrorCodes.CertificateNotCodeSigning,
                    "The publisher certificate has no enhanced key usage extension; code signing usage cannot be confirmed."));
            }
        }

        private static void VerifyContainerSignature(
            MsixpkgContainer container,
            MsixpkgManifest manifest,
            MsixpkgValidateOptions options,
            MsixpkgValidationReport report)
        {
            MsixpkgSignature? signature;
            try
            {
                signature = container.ReadSignature();
            }
            catch (MsixpkgFormatException ex)
            {
                report.Errors.Add(ToIssue(ex));
                return;
            }

            if (signature == null)
            {
                if (options.RequireSignature)
                {
                    report.Errors.Add(new MsixpkgValidationIssue(
                        MsixpkgErrorCodes.SignatureMissing,
                        "The container is not signed (msixpkg.sig is missing)."));
                }

                return;
            }

            List<MsixpkgContentDigestEntry> digestEntries = container.ComputeDigestEntries(excludeSignature: true);
            string digestHex = MsixpkgContentDigest.ComputeHex(digestEntries);
            report.ContentDigestHex = digestHex;

            if (!string.Equals(digestHex, signature.ContentDigestHex, StringComparison.Ordinal))
            {
                report.Errors.Add(new MsixpkgValidationIssue(
                    MsixpkgErrorCodes.SignatureInvalid,
                    "The container content digest does not match the signature.",
                    digestHex + " != " + signature.ContentDigestHex));
                return;
            }

            X509Certificate2? certificate = report.PublisherCertificate;
            if (certificate == null)
            {
                report.Errors.Add(new MsixpkgValidationIssue(
                    MsixpkgErrorCodes.CertificateMissing,
                    "The container signature cannot be verified without a publisher certificate."));
                return;
            }

            string thumbprint = Thumbprints.Sha256Hex(certificate);
            if (!string.Equals(thumbprint, signature.KeyThumbprintSha256, StringComparison.Ordinal))
            {
                report.Errors.Add(new MsixpkgValidationIssue(
                    MsixpkgErrorCodes.SignatureThumbprintMismatch,
                    "The signature key thumbprint does not match the publisher certificate.",
                    signature.KeyThumbprintSha256 + " != " + thumbprint));
                return;
            }

            try
            {
                MsixpkgSigner.Verify(HexToBytes(digestHex), signature, certificate);
            }
            catch (MsixpkgFormatException ex)
            {
                report.Errors.Add(ToIssue(ex));
            }
        }

        private static void VerifyLocalFile(string path, MsixpkgFileRef expected, MsixpkgValidationReport report)
        {
            FileInfo info = new FileInfo(path);
            if (!info.Exists)
            {
                report.Errors.Add(new MsixpkgValidationIssue(MsixpkgErrorCodes.FileMissing, "The extracted main package is missing.", path));
                return;
            }

            if (info.Length != expected.Size)
            {
                report.Errors.Add(new MsixpkgValidationIssue(
                    MsixpkgErrorCodes.SizeMismatch,
                    "The extracted main package size does not match the manifest.",
                    expected.Path));
                return;
            }

            string actual = Sha256Util.HashFileHex(path);
            if (!string.Equals(actual, expected.Sha256, StringComparison.Ordinal))
            {
                report.Errors.Add(new MsixpkgValidationIssue(
                    MsixpkgErrorCodes.HashMismatch,
                    "The extracted main package hash does not match the manifest.",
                    expected.Path));
            }
        }

        private static void VerifyInnerAppx(string msixPath, MsixpkgManifest manifest, MsixpkgValidationReport report)
        {
            AppxPackageInfo appx;
            try
            {
                appx = AppxPackageReader.ReadManifest(msixPath);
            }
            catch (MsixpkgFormatException ex)
            {
                report.Errors.Add(ToIssue(ex));
                return;
            }

            if (!string.Equals(appx.Name, manifest.Identity.Name, StringComparison.Ordinal))
            {
                report.Errors.Add(new MsixpkgValidationIssue(
                    MsixpkgErrorCodes.AppxInvalid,
                    "The inner MSIX identity name does not match the container manifest.",
                    appx.Name + " != " + manifest.Identity.Name));
            }

            if (!string.Equals(appx.Publisher, manifest.Identity.Publisher, StringComparison.Ordinal))
            {
                report.Errors.Add(new MsixpkgValidationIssue(
                    MsixpkgErrorCodes.AppxPublisherMismatch,
                    "The inner MSIX publisher does not match the container manifest.",
                    appx.Publisher + " != " + manifest.Identity.Publisher));
            }

            if (!string.Equals(AppxPackageReader.NormalizeVersion(appx.Version), manifest.Identity.Version, StringComparison.Ordinal))
            {
                report.Errors.Add(new MsixpkgValidationIssue(
                    MsixpkgErrorCodes.AppxVersionMismatch,
                    "The inner MSIX version does not match the container manifest.",
                    appx.Version + " != " + manifest.Identity.Version));
            }

            if (!string.Equals(AppxPackageReader.NormalizeArchitecture(appx.Architecture), manifest.Identity.Architecture, StringComparison.Ordinal))
            {
                report.Errors.Add(new MsixpkgValidationIssue(
                    MsixpkgErrorCodes.AppxArchitectureMismatch,
                    "The inner MSIX architecture does not match the container manifest.",
                    appx.Architecture + " != " + manifest.Identity.Architecture));
            }

            // 容器携带的依赖包必须是内部清单声明过的依赖。
            foreach (MsixpkgFileRef dependency in manifest.Dependencies)
            {
                bool declared = false;
                foreach (AppxDependency appxDependency in appx.Dependencies)
                {
                    if (string.Equals(appxDependency.Name, dependency.Name, StringComparison.OrdinalIgnoreCase))
                    {
                        declared = true;
                        break;
                    }
                }

                if (!declared)
                {
                    report.Errors.Add(new MsixpkgValidationIssue(
                        MsixpkgErrorCodes.AppxInvalid,
                        "The container carries a dependency that the inner MSIX does not declare.",
                        dependency.Name));
                }
            }

            if (manifest.Publisher != null)
            {
                X509Certificate2Collection certificates;
                try
                {
                    certificates = AppxPackageReader.ReadSignerCertificates(msixPath);
                }
                catch (MsixpkgFormatException ex)
                {
                    report.Errors.Add(ToIssue(ex));
                    return;
                }

                bool matched = false;
                foreach (X509Certificate2 candidate in certificates)
                {
                    if (string.Equals(Thumbprints.Sha256Hex(candidate), manifest.Publisher.ThumbprintSha256, StringComparison.Ordinal))
                    {
                        matched = true;
                        break;
                    }
                }

                if (!matched)
                {
                    report.Errors.Add(new MsixpkgValidationIssue(
                        MsixpkgErrorCodes.AppxSignerMismatch,
                        "The inner MSIX is signed by a different certificate than the one carried in the container.",
                        manifest.Publisher.ThumbprintSha256));
                }
            }
        }

        private static bool IsArchitectureCompatible(string packageArchitecture, string hostArchitecture)
        {
            if (string.Equals(packageArchitecture, hostArchitecture, StringComparison.Ordinal))
            {
                return true;
            }

            // x64 主机可以运行 x86 包。
            return string.Equals(hostArchitecture, "x64", StringComparison.Ordinal)
                && string.Equals(packageArchitecture, "x86", StringComparison.Ordinal);
        }

        private static bool ContainsThumbprint(IReadOnlyCollection<string> thumbprints, string thumbprint)
        {
            foreach (string candidate in thumbprints)
            {
                if (!string.IsNullOrEmpty(candidate) && string.Equals(candidate, thumbprint, StringComparison.OrdinalIgnoreCase))
                {
                    return true;
                }
            }

            return false;
        }

        internal static bool IsVersionAtLeast(string actual, string required)
        {
            return ParseVersion(actual) >= ParseVersion(required);
        }

        private static Version ParseVersion(string value)
        {
            if (Version.TryParse(value, out Version? parsed))
            {
                return parsed!;
            }

            return new Version(0, 0);
        }

        private static byte[] HexToBytes(string hex)
        {
            var bytes = new byte[hex.Length / 2];
            for (int i = 0; i < bytes.Length; i++)
            {
                bytes[i] = Convert.ToByte(hex.Substring(i * 2, 2), 16);
            }

            return bytes;
        }

        private static MsixpkgValidationIssue ToIssue(MsixpkgFormatException ex)
        {
            return new MsixpkgValidationIssue(ex.Code, ex.Message, ex.Detail);
        }

        private static void TryDelete(string path)
        {
            try
            {
                if (File.Exists(path))
                {
                    File.Delete(path);
                }
            }
            catch (IOException)
            {
            }
            catch (UnauthorizedAccessException)
            {
            }
        }
    }
}
