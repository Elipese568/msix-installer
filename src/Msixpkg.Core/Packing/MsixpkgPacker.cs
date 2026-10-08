using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Security.Cryptography.X509Certificates;
using Msixpkg.Core.Appx;

namespace Msixpkg.Core.Packing
{
    public sealed class MsixpkgPackRequest
    {
        /// <summary>已签名的 .msix（单项目 MSIX 的产物）。</summary>
        public string MsixPath { get; set; } = string.Empty;

        /// <summary>用于签名该 .msix 的 PFX。证书公钥会被导出成 .cer 并内嵌进容器。</summary>
        public string PfxPath { get; set; } = string.Empty;

        public string? PfxPassword { get; set; }

        public string OutputPath { get; set; } = string.Empty;

        /// <summary>依赖包搜索目录（VS 发布产物里的 Dependencies 目录）。null 表示不搜索。</summary>
        public string? DependencyDirectory { get; set; }

        /// <summary>导出 .cer 的路径；null 表示与容器同目录同名。</summary>
        public string? CertificateOutputPath { get; set; }

        public int MinOsBuild { get; set; } = 19041;

        public string InstallerMinVersion { get; set; } = "0.1.0";

        public bool Sign { get; set; } = true;

        /// <summary>null = 自动（自签名证书内嵌，受信 CA 证书不内嵌）。</summary>
        public bool? IncludeCertificate { get; set; }

        public bool AllowAllUsers { get; set; } = true;

        public bool UninstallRemoveCertificate { get; set; } = true;

        public bool AllowNonDeterministicSignature { get; set; }

        public DateTimeOffset? CreatedUtc { get; set; }

        public string? DisplayName { get; set; }

        public string? DisplayDescription { get; set; }

        public string? DisplayHomepage { get; set; }
    }

    public sealed class MsixpkgPackResult
    {
        internal MsixpkgPackResult(MsixpkgManifest manifest, string outputPath, string certificatePath, AppxPackageInfo appx)
        {
            Manifest = manifest;
            OutputPath = outputPath;
            CertificatePath = certificatePath;
            Appx = appx;
        }

        public MsixpkgManifest Manifest { get; }

        public string OutputPath { get; }

        public string CertificatePath { get; }

        public AppxPackageInfo Appx { get; }

        public List<string> Warnings { get; } = new List<string>();
    }

    /// <summary>
    /// CLI 与 MSBuild 任务共用的打包实现，保证「开发机手工打包」与「构建期自动打包」结果一致。
    /// </summary>
    public static class MsixpkgPacker
    {
        private static readonly TimeSpan ExpiryWarningWindow = TimeSpan.FromDays(90);

        public static MsixpkgPackResult Pack(MsixpkgPackRequest request)
        {
            if (request == null)
            {
                throw new ArgumentNullException(nameof(request));
            }

            RequireFile(request.MsixPath, "the input .msix");
            RequireFile(request.PfxPath, "the signing .pfx");
            if (string.IsNullOrEmpty(request.OutputPath))
            {
                throw new MsixpkgFormatException(MsixpkgErrorCodes.IoError, "An output path for the .msixpkg is required.");
            }

            AppxPackageInfo appx = AppxPackageReader.ReadManifest(request.MsixPath);
            string architecture = AppxPackageReader.NormalizeArchitecture(appx.Architecture);
            string version = AppxPackageReader.NormalizeVersion(appx.Version);

            using X509Certificate2 signer = LoadSigner(request.PfxPath, request.PfxPassword);

            // 证书 Subject 必须与清单 Publisher 逐字符相等，否则装到客户机上必然签名不受信。
            if (!string.Equals(signer.Subject, appx.Publisher, StringComparison.Ordinal))
            {
                throw new MsixpkgFormatException(
                    MsixpkgErrorCodes.PublisherMismatch,
                    "The signing certificate subject does not match the package manifest publisher.",
                    "certificate: " + signer.Subject + " | manifest: " + appx.Publisher);
            }

            EnsureMsixSignedByThisCertificate(request.MsixPath, signer);

            var identity = new MsixpkgIdentity(appx.Name, appx.Publisher, version, architecture);
            var builder = new MsixpkgPackageBuilder(identity);
            builder.SetMainPackage(request.MsixPath);

            var warnings = new List<string>();
            ResolveDependencies(request, appx, architecture, builder);

            var options = new MsixpkgWriteOptions
            {
                IncludePublisherCertificate = request.IncludeCertificate,
                Sign = request.Sign,
                MinOsBuild = request.MinOsBuild,
                InstallerMinVersion = request.InstallerMinVersion,
                AllowAllUsers = request.AllowAllUsers,
                UninstallRemoveCertificate = request.UninstallRemoveCertificate,
                AllowNonDeterministicSignature = request.AllowNonDeterministicSignature,
                CreatedUtc = request.CreatedUtc,
                Display = BuildDisplay(request),
            };

            MsixpkgManifest manifest = builder.Write(request.OutputPath, signer, options);

            string certificatePath = request.CertificateOutputPath
                ?? Path.Combine(
                    Path.GetDirectoryName(Path.GetFullPath(request.OutputPath)) ?? ".",
                    Path.GetFileNameWithoutExtension(request.OutputPath) + ".cer");
            File.WriteAllBytes(certificatePath, signer.Export(X509ContentType.Cert));

            DateTime nowUtc = DateTime.UtcNow;
            DateTime notAfter = signer.NotAfter.ToUniversalTime();
            if (notAfter <= nowUtc)
            {
                // 已过期不阻止打包（可能只是补历史产物），但必须显式告知。
                warnings.Add("The signing certificate expired at " + MsixpkgManifest.FormatIsoUtc(notAfter)
                    + "; packages signed with it cannot be installed.");
            }
            else if (notAfter - nowUtc <= ExpiryWarningWindow)
            {
                warnings.Add("The signing certificate expires at " + MsixpkgManifest.FormatIsoUtc(notAfter)
                    + " (less than 90 days); plan a certificate rotation.");
            }

            if (manifest.Publisher == null)
            {
                warnings.Add("No publisher certificate is embedded in the container (non self-signed signing certificate): "
                    + "the installer will not touch any certificate store, but the package must be signed by a "
                    + "certificate the target machine already trusts.");
            }

            var result = new MsixpkgPackResult(manifest, Path.GetFullPath(request.OutputPath), certificatePath, appx);
            result.Warnings.AddRange(warnings);
            return result;
        }

        private static MsixpkgDisplayInfo? BuildDisplay(MsixpkgPackRequest request)
        {
            if (request.DisplayName == null && request.DisplayDescription == null && request.DisplayHomepage == null)
            {
                return null;
            }

            return new MsixpkgDisplayInfo(
                request.DisplayName,
                request.DisplayDescription,
                request.DisplayHomepage,
                null);
        }

        private static X509Certificate2 LoadSigner(string pfxPath, string? password)
        {
            return MsixpkgCertificates.LoadSigningCertificateFromFile(pfxPath, password);
        }

        private static void EnsureMsixSignedByThisCertificate(string msixPath, X509Certificate2 signer)
        {
            if (!AppxPackageReader.HasSignature(msixPath))
            {
                throw new MsixpkgFormatException(
                    MsixpkgErrorCodes.AppxSignatureMissing,
                    "The input .msix is not signed. Enable AppxPackageSigningEnabled (and PackageCertificateKeyFile) before packaging.",
                    msixPath);
            }

            string expected = Thumbprints.Sha256Hex(signer);
            X509Certificate2Collection certificates = AppxPackageReader.ReadSignerCertificates(msixPath);
            foreach (X509Certificate2 candidate in certificates)
            {
                if (string.Equals(Thumbprints.Sha256Hex(candidate), expected, StringComparison.Ordinal))
                {
                    return;
                }
            }

            throw new MsixpkgFormatException(
                MsixpkgErrorCodes.AppxSignerMismatch,
                "The .msix is signed by a different certificate than the supplied .pfx.",
                "expected thumbprint " + expected);
        }

        private static void ResolveDependencies(
            MsixpkgPackRequest request,
            AppxPackageInfo appx,
            string architecture,
            MsixpkgPackageBuilder builder)
        {
            if (appx.Dependencies.Count == 0)
            {
                return;
            }

            if (string.IsNullOrEmpty(request.DependencyDirectory))
            {
                throw new MsixpkgFormatException(
                    MsixpkgErrorCodes.AppxInvalid,
                    "The package declares dependencies but no dependency directory was given. "
                    + "Set MsixpkgDependencyDir (or WindowsAppSDKSelfContained=true).",
                    DescribeMissingDependencies(appx, null));
            }

            List<string> candidates = FindDependencyCandidates(request.DependencyDirectory!);

            foreach (AppxDependency dependency in appx.Dependencies)
            {
                string? match = FindMatchingDependency(candidates, dependency.Name, architecture);
                if (match == null)
                {
                    throw new MsixpkgFormatException(
                        MsixpkgErrorCodes.AppxInvalid,
                        "A declared package dependency could not be found in the dependency directory.",
                        dependency.Name + " (searched " + request.DependencyDirectory + ")");
                }

                AppxPackageInfo dependencyInfo = AppxPackageReader.ReadManifest(match);
                builder.AddDependency(match, dependencyInfo.Name, AppxPackageReader.NormalizeVersion(dependencyInfo.Version));
            }
        }

        private static string DescribeMissingDependencies(AppxPackageInfo appx, string? directory)
        {
            var names = new List<string>();
            foreach (AppxDependency dependency in appx.Dependencies)
            {
                names.Add(dependency.Name);
            }

            return string.Join(", ", names) + (directory == null ? string.Empty : " (searched " + directory + ")");
        }

        private static List<string> FindDependencyCandidates(string directory)
        {
            if (!Directory.Exists(directory))
            {
                throw new MsixpkgFormatException(
                    MsixpkgErrorCodes.IoError,
                    "The dependency directory does not exist.",
                    directory);
            }

            var candidates = new List<string>(Directory.GetFiles(directory, "*.msix", SearchOption.AllDirectories));
            candidates.Sort(StringComparer.Ordinal);
            return candidates;
        }

        private static string? FindMatchingDependency(List<string> candidates, string packageName, string architecture)
        {
            string? fallback = null;

            foreach (string candidate in candidates)
            {
                AppxPackageInfo info;
                try
                {
                    info = AppxPackageReader.ReadManifest(candidate);
                }
                catch (MsixpkgFormatException)
                {
                    // 目录里可能有无关文件；无法解析的直接跳过，而不是让整个打包失败。
                    continue;
                }

                if (!string.Equals(info.Name, packageName, StringComparison.OrdinalIgnoreCase))
                {
                    continue;
                }

                string candidateArchitecture = AppxPackageReader.NormalizeArchitecture(info.Architecture);
                if (string.Equals(candidateArchitecture, architecture, StringComparison.Ordinal)
                    || string.Equals(candidateArchitecture, "neutral", StringComparison.Ordinal))
                {
                    return candidate;
                }

                fallback ??= candidate;
            }

            return fallback;
        }

        private static void RequireFile(string path, string description)
        {
            if (string.IsNullOrEmpty(path))
            {
                throw new MsixpkgFormatException(
                    MsixpkgErrorCodes.IoError,
                    "A path is required for " + description + ".");
            }

            if (!File.Exists(path))
            {
                throw new MsixpkgFormatException(
                    MsixpkgErrorCodes.IoError,
                    "The file for " + description + " does not exist.",
                    path);
            }
        }

        /// <summary>把依赖目录里的候选包摘要成一行行文本，供 CLI/任务打印。</summary>
        public static IEnumerable<string> DescribeDependencies(string dependencyDirectory)
        {
            foreach (string candidate in FindDependencyCandidates(dependencyDirectory))
            {
                AppxPackageInfo info;
                try
                {
                    info = AppxPackageReader.ReadManifest(candidate);
                }
                catch (MsixpkgFormatException)
                {
                    continue;
                }

                yield return string.Format(
                    CultureInfo.InvariantCulture,
                    "{0} {1} {2}",
                    info.Name,
                    info.Version,
                    AppxPackageReader.NormalizeArchitecture(info.Architecture));
            }
        }
    }
}
