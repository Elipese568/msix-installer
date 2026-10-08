using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.IO.Compression;
using System.Security.Cryptography.X509Certificates;

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
        /// <summary>
        /// v1 规定写出必须使用 store（不压缩）：容器字节完全可复现，
        /// 黄金样本因此可以逐字节比对，跨语言一致性测试才有意义。
        /// 读取端仍然必须支持 deflate。
        /// </summary>
        private const CompressionLevel StoreLevel = CompressionLevel.NoCompression;

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
                using (var archive = new ZipArchive(file, ZipArchiveMode.Create, leaveOpen: false))
                {
                    var timestamp = new DateTimeOffset(DateTime.ParseExact(
                        manifest.CreatedUtc,
                        "yyyy-MM-dd'T'HH:mm:ss'Z'",
                        CultureInfo.InvariantCulture,
                        DateTimeStyles.AdjustToUniversal | DateTimeStyles.AssumeUniversal).ToUniversalTime().Date, TimeSpan.Zero);

                    // 1) msixpkg.json 必须是第一个条目（规范要求，读者可流式取清单）。
                    byte[] manifestBytes = System.Text.Encoding.UTF8.GetBytes(manifest.ToJson());
                    WriteBytesEntry(archive, MsixpkgPaths.ManifestEntry, manifestBytes, StoreLevel, timestamp);
                    digestEntries.Add(new MsixpkgContentDigestEntry(MsixpkgPaths.ManifestEntry, Sha256Util.HashBytesHex(manifestBytes)));

                    // 2) 主包
                    WriteFileEntry(archive, manifest.MainPackage.Path, _mainPackage!.FilePath, manifest.MainPackage, StoreLevel, timestamp);
                    digestEntries.Add(new MsixpkgContentDigestEntry(manifest.MainPackage.Path, manifest.MainPackage.Sha256));

                    // 3) 依赖包
                    for (int i = 0; i < manifest.Dependencies.Count; i++)
                    {
                        MsixpkgFileRef dependencyRef = manifest.Dependencies[i];
                        WriteFileEntry(archive, dependencyRef.Path, _dependencies[i].FilePath, dependencyRef, StoreLevel, timestamp);
                        digestEntries.Add(new MsixpkgContentDigestEntry(dependencyRef.Path, dependencyRef.Sha256));
                    }

                    // 4) 发布者证书（自签名模型）
                    if (manifest.Publisher != null)
                    {
                        byte[] der = signer.Export(X509ContentType.Cert);
                        WriteBytesEntry(archive, MsixpkgPaths.PublisherCertificateEntry, der, StoreLevel, timestamp);
                        digestEntries.Add(new MsixpkgContentDigestEntry(MsixpkgPaths.PublisherCertificateEntry, Sha256Util.HashBytesHex(der)));
                    }

                    // 5) 容器签名：摘要排除 msixpkg.sig 自身。
                    if (options.Sign)
                    {
                        byte[] digest = MsixpkgContentDigest.Compute(digestEntries);
                        MsixpkgSignature signature = MsixpkgSigner.Sign(digest, signer, options.AllowNonDeterministicSignature);
                        byte[] signatureBytes = System.Text.Encoding.UTF8.GetBytes(MsixpkgSignatureCodec.Write(signature));
                        WriteBytesEntry(archive, MsixpkgPaths.SignatureEntry, signatureBytes, StoreLevel, timestamp);
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

        private static void WriteBytesEntry(ZipArchive archive, string name, byte[] content, CompressionLevel level, DateTimeOffset timestamp)
        {
            ZipArchiveEntry entry = archive.CreateEntry(name, level);
            entry.LastWriteTime = timestamp;
            using Stream stream = entry.Open();
            stream.Write(content, 0, content.Length);
        }

        private static void WriteFileEntry(
            ZipArchive archive,
            string entryName,
            string sourcePath,
            MsixpkgFileRef expected,
            CompressionLevel level,
            DateTimeOffset timestamp)
        {
            ZipArchiveEntry entry = archive.CreateEntry(entryName, level);
            entry.LastWriteTime = timestamp;

            using var sha = System.Security.Cryptography.SHA256.Create();
            using (var source = new FileStream(sourcePath, FileMode.Open, FileAccess.Read, FileShare.Read))
            using (Stream destination = entry.Open())
            {
                var buffer = new byte[1024 * 128];
                long copied = 0L;
                int read;
                while ((read = source.Read(buffer, 0, buffer.Length)) > 0)
                {
                    sha.TransformBlock(buffer, 0, read, null, 0);
                    destination.Write(buffer, 0, read);
                    copied += read;
                }

                sha.TransformFinalBlock(Array.Empty<byte>(), 0, 0);

                // 第二遍校验：防止输入文件在被读取期间被替换。
                string actual = Sha256Util.ToLowerHex(sha.Hash!);
                if (!string.Equals(actual, expected.Sha256, StringComparison.Ordinal))
                {
                    throw new MsixpkgFormatException(
                        MsixpkgErrorCodes.HashMismatch,
                        "The input file changed while packaging.",
                        entryName);
                }

                if (copied != expected.Size)
                {
                    throw new MsixpkgFormatException(
                        MsixpkgErrorCodes.SizeMismatch,
                        "The input file size changed while packaging.",
                        entryName);
                }
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
