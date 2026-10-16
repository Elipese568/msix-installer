using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Security.Cryptography.X509Certificates;
using Msixpkg.Core.Zip;

namespace Msixpkg.Core
{
    public sealed class MsixpkgWriteOptions
    {
        /// <summary>null 表示自动：自签名证书包含 .cer，受信 CA 证书不包含。</summary>
        public bool? IncludePublisherCertificate { get; set; }

        public bool Sign { get; set; } = true;

        /// <summary>允许非确定性签名算法（ECDSA）。默认关闭：v1 的容器应当逐字节可复现。</summary>
        public bool AllowNonDeterministicSignature { get; set; }

        public int MinOsBuild { get; set; } = 19041;

        public string InstallerMinVersion { get; set; } = "0.1.0";

        public bool AllowAllUsers { get; set; } = true;

        public bool UninstallRemoveCertificate { get; set; } = true;

        public MsixpkgDisplayInfo? Display { get; set; }

        public DateTimeOffset? CreatedUtc { get; set; }
    }

    /// <summary>把「已签名的 .msix + 依赖 + 发布者证书」组装成 .msixpkg。</summary>
    public sealed class MsixpkgPackageBuilder
    {
        // 写出统一交给 Msixpkg.Core.Zip.StoredZipWriter：v1 规定写出必须使用 store（不压缩），
        // 容器字节才完全可复现、黄金样本才能逐字节比对，跨语言一致性测试才有意义。
        // （不要退回 System.IO.Compression.ZipArchive：.NET Framework 宿主会用 deflate 写
        //   CompressionLevel.NoCompression，同一个工程在 VS 与 dotnet build 下产出不同字节。）
        // 读取端仍然必须支持 deflate —— 第三方容器可能压过。

        private readonly List<DependencyInput> _dependencies = new List<DependencyInput>();
        private readonly MsixpkgIdentity _identity;
        private MainInput? _mainPackage;

        public MsixpkgPackageBuilder(MsixpkgIdentity identity)
        {
            _identity = identity ?? throw new ArgumentNullException(nameof(identity));

            if (!MsixpkgPaths.IsKnownArchitecture(identity.Architecture))
            {
                throw new MsixpkgFormatException(
                    MsixpkgErrorCodes.ManifestSchema,
                    "Unknown architecture for the package identity.",
                    identity.Architecture);
            }
        }

        public void SetMainPackage(string filePath)
        {
            if (string.IsNullOrEmpty(filePath))
            {
                throw new ArgumentException("Main package path is required.", nameof(filePath));
            }

            _mainPackage = new MainInput(filePath);
        }

        public void AddDependency(string filePath, string packageName, string version)
        {
            if (string.IsNullOrEmpty(filePath))
            {
                throw new ArgumentException("Dependency path is required.", nameof(filePath));
            }

            if (string.IsNullOrEmpty(packageName) || string.IsNullOrEmpty(version))
            {
                throw new ArgumentException("Dependency name and version are required.");
            }

            _dependencies.Add(new DependencyInput(filePath, packageName, version));
        }

        public MsixpkgManifest BuildManifest(X509Certificate2 signer, MsixpkgWriteOptions options)
        {
            if (signer == null)
            {
                throw new ArgumentNullException(nameof(signer));
            }

            if (options == null)
            {
                throw new ArgumentNullException(nameof(options));
            }

            if (_mainPackage == null)
            {
                throw new InvalidOperationException("SetMainPackage must be called before building the manifest.");
            }

            // 发布者信息与内嵌证书都取自签名证书，二者不可能不一致。
            bool includeCertificate = options.IncludePublisherCertificate
                ?? string.Equals(signer.Subject, signer.Issuer, StringComparison.OrdinalIgnoreCase);

            string mainEntry = MsixpkgPaths.MainPackageEntry(_identity.Architecture, _identity.Name, _identity.Version);
            var mainRef = new MsixpkgFileRef(mainEntry, Sha256Util.HashFileHex(_mainPackage.FilePath), new FileInfo(_mainPackage.FilePath).Length);

            var manifest = new MsixpkgManifest(_identity, mainRef)
            {
                CreatedUtc = MsixpkgManifest.FormatIsoUtc((options.CreatedUtc ?? DateTimeOffset.UtcNow).UtcDateTime),
                MinOsBuild = options.MinOsBuild,
                InstallerMinVersion = options.InstallerMinVersion,
                RequiresAdmin = true,
                AllowAllUsers = options.AllowAllUsers,
                UninstallRemoveCertificate = options.UninstallRemoveCertificate,
                Display = options.Display,
            };

            foreach (DependencyInput dependency in _dependencies)
            {
                string entry = MsixpkgPaths.DependencyEntry(_identity.Architecture, dependency.PackageName, dependency.Version);
                manifest.Dependencies.Add(new MsixpkgFileRef(
                    entry,
                    Sha256Util.HashFileHex(dependency.FilePath),
                    new FileInfo(dependency.FilePath).Length,
                    dependency.PackageName,
                    dependency.Version));
            }

            if (includeCertificate)
            {
                manifest.Publisher = new MsixpkgPublisherInfo(
                    MsixpkgPaths.PublisherCertificateEntry,
                    signer.Subject,
                    Thumbprints.Sha1Hex(signer),
                    Thumbprints.Sha256Hex(signer),
                    MsixpkgManifest.FormatIsoUtc(signer.NotBefore.ToUniversalTime()),
                    MsixpkgManifest.FormatIsoUtc(signer.NotAfter.ToUniversalTime()),
                    string.Equals(signer.Subject, signer.Issuer, StringComparison.OrdinalIgnoreCase));
            }

            manifest.Validate();
            return manifest;
        }

        public MsixpkgManifest Write(string outputPath, X509Certificate2 signer, MsixpkgWriteOptions? options = null)
        {
            options ??= new MsixpkgWriteOptions();
            MsixpkgManifest manifest = BuildManifest(signer, options);

            string? directory = Path.GetDirectoryName(Path.GetFullPath(outputPath));
            if (!string.IsNullOrEmpty(directory))
            {
                Directory.CreateDirectory(directory);
            }

            string temporaryPath = outputPath + ".tmp";
            var digestEntries = new List<MsixpkgContentDigestEntry>();

            try
            {
                using (var file = new FileStream(temporaryPath, FileMode.Create, FileAccess.ReadWrite, FileShare.None))
                using (var archive = new StoredZipWriter(file, leaveOpen: false))
                {
                    var timestamp = new DateTimeOffset(DateTime.ParseExact(
                        manifest.CreatedUtc,
                        "yyyy-MM-dd'T'HH:mm:ss'Z'",
                        CultureInfo.InvariantCulture,
                        DateTimeStyles.AdjustToUniversal | DateTimeStyles.AssumeUniversal).ToUniversalTime().Date, TimeSpan.Zero);

                    // 1) msixpkg.json 必须是第一个条目（规范要求，读者可流式取清单）。
                    byte[] manifestBytes = System.Text.Encoding.UTF8.GetBytes(manifest.ToJson());
                    archive.AddBytes(MsixpkgPaths.ManifestEntry, manifestBytes, timestamp);
                    digestEntries.Add(new MsixpkgContentDigestEntry(MsixpkgPaths.ManifestEntry, Sha256Util.HashBytesHex(manifestBytes)));

                    // 2) 主包
                    AddFileEntry(archive, manifest.MainPackage.Path, _mainPackage!.FilePath, manifest.MainPackage, timestamp);
                    digestEntries.Add(new MsixpkgContentDigestEntry(manifest.MainPackage.Path, manifest.MainPackage.Sha256));

                    // 3) 依赖包
                    for (int i = 0; i < manifest.Dependencies.Count; i++)
                    {
                        MsixpkgFileRef dependencyRef = manifest.Dependencies[i];
                        AddFileEntry(archive, dependencyRef.Path, _dependencies[i].FilePath, dependencyRef, timestamp);
                        digestEntries.Add(new MsixpkgContentDigestEntry(dependencyRef.Path, dependencyRef.Sha256));
                    }

                    // 4) 发布者证书（自签名模型）
                    if (manifest.Publisher != null)
                    {
                        byte[] der = signer.Export(X509ContentType.Cert);
                        archive.AddBytes(MsixpkgPaths.PublisherCertificateEntry, der, timestamp);
                        digestEntries.Add(new MsixpkgContentDigestEntry(MsixpkgPaths.PublisherCertificateEntry, Sha256Util.HashBytesHex(der)));
                    }

                    // 5) 容器签名：摘要排除 msixpkg.sig 自身。
                    if (options.Sign)
                    {
                        byte[] digest = MsixpkgContentDigest.Compute(digestEntries);
                        MsixpkgSignature signature = MsixpkgSigner.Sign(digest, signer, options.AllowNonDeterministicSignature);
                        byte[] signatureBytes = System.Text.Encoding.UTF8.GetBytes(MsixpkgSignatureCodec.Write(signature));
                        archive.AddBytes(MsixpkgPaths.SignatureEntry, signatureBytes, timestamp);
                    }
                }

                if (File.Exists(outputPath))
                {
                    File.Delete(outputPath);
                }

                File.Move(temporaryPath, outputPath);
            }
            catch
            {
                TryDelete(temporaryPath);
                throw;
            }

            return manifest;
        }

        /// <summary>
        /// 流式写入一个文件条目，并在**写出的同一次读取里**复算哈希与大小：
        /// 输入文件若在打包过程中被替换（TOCTOU），这里会直接失败，而不是产出一个自相矛盾的容器。
        /// </summary>
        private static void AddFileEntry(
            StoredZipWriter archive,
            string entryName,
            string sourcePath,
            MsixpkgFileRef expected,
            DateTimeOffset timestamp)
        {
            ZipEntryInfo written = archive.AddFile(entryName, sourcePath, timestamp);

            // 第二遍校验：防止输入文件在被读取期间被替换。
            if (!string.Equals(written.Sha256Hex, expected.Sha256, StringComparison.Ordinal))
            {
                throw new MsixpkgFormatException(
                    MsixpkgErrorCodes.HashMismatch,
                    "The input file changed while packaging.",
                    entryName);
            }

            if (written.Size != expected.Size)
            {
                throw new MsixpkgFormatException(
                    MsixpkgErrorCodes.SizeMismatch,
                    "The input file size changed while packaging.",
                    entryName);
            }
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

        private sealed class MainInput
        {
            public MainInput(string filePath)
            {
                FilePath = filePath;
            }

            public string FilePath { get; }
        }

        private sealed class DependencyInput
        {
            public DependencyInput(string filePath, string packageName, string version)
            {
                FilePath = filePath;
                PackageName = packageName;
                Version = version;
            }

            public string FilePath { get; }

            public string PackageName { get; }

            public string Version { get; }
        }
    }
}
