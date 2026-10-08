using System;
using System.Collections.Generic;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;
using System.Text;
using Msixpkg.Core;

namespace Msixpkg.Core.Tests;

internal static class TestPaths
{
    static TestPaths()
    {
        var directory = new DirectoryInfo(AppContext.BaseDirectory);
        while (directory != null && !File.Exists(Path.Combine(directory.FullName, "Directory.Build.props")))
        {
            directory = directory.Parent;
        }

        RepositoryRoot = directory?.FullName
            ?? throw new InvalidOperationException("Could not locate the repository root from " + AppContext.BaseDirectory);
    }

    public static string RepositoryRoot { get; }

    public static string VectorsDirectory => Path.Combine(RepositoryRoot, "tests", "vectors");

    public static string TestCertificatePath => Path.Combine(VectorsDirectory, "test-only.pfx");

    public static string ArtifactsDirectory => Path.Combine(RepositoryRoot, "artifacts", "tests");
}

internal static class TestCertificates
{
    /// <summary>仅测试用；已在 tests/vectors/README.md 中明确禁止用于任何真实分发。</summary>
    public const string Password = "msixpkg-test-only";

    public const string PublisherDn = "CN=Msixpkg Test Publisher, O=MsixpkgInstaller, C=CN";

    private static readonly object Gate = new object();
    private static X509Certificate2? _cached;

    /// <summary>
    /// 进程内缓存 + 锁：xunit 默认并行跑测试集合，而「读不到就新建」会让多个线程各写一份
    /// 不同的 pfx，导致容器里的证书与钉扎值对不上 —— 那会表现为莫名其妙的 E_PUBLISHER_NOT_PINNED。
    /// </summary>
    public static X509Certificate2 LoadOrCreateTestCertificate()
    {
        lock (Gate)
        {
            if (_cached != null)
            {
                return _cached;
            }

            string path = TestPaths.TestCertificatePath;
            if (!File.Exists(path))
            {
                Directory.CreateDirectory(TestPaths.VectorsDirectory);
                using X509Certificate2 created = CreateEphemeral(PublisherDn, TimeSpan.FromDays(3650));
                byte[] pfx = created.Export(X509ContentType.Pfx, Password);

                // 原子落盘，避免并发进程读到写了一半的文件。
                string temporary = path + "." + Guid.NewGuid().ToString("n") + ".tmp";
                File.WriteAllBytes(temporary, pfx);
                if (File.Exists(path))
                {
                    File.Delete(temporary);
                }
                else
                {
                    File.Move(temporary, path);
                }
            }

            _cached = Load(path);
            return _cached;
        }
    }

    public static X509Certificate2 Load(string path)
    {
        return X509CertificateLoader.LoadPkcs12FromFile(
            path,
            Password,
            X509KeyStorageFlags.Exportable | X509KeyStorageFlags.EphemeralKeySet);
    }

    public static X509Certificate2 CreateEphemeral(string subject, TimeSpan validFor, bool codeSigningEku = true, DateTimeOffset? notBefore = null)
    {
        using var rsa = RSA.Create(2048);
        var request = new CertificateRequest(subject, rsa, HashAlgorithmName.SHA256, RSASignaturePadding.Pkcs1);
        request.CertificateExtensions.Add(new X509BasicConstraintsExtension(false, false, 0, false));
        request.CertificateExtensions.Add(new X509KeyUsageExtension(X509KeyUsageFlags.DigitalSignature, false));
        if (codeSigningEku)
        {
            request.CertificateExtensions.Add(new X509EnhancedKeyUsageExtension(
                new OidCollection { new Oid(MsixpkgValidator.CodeSigningEkuOid) },
                false));
        }
        else
        {
            request.CertificateExtensions.Add(new X509EnhancedKeyUsageExtension(
                new OidCollection { new Oid("1.3.6.1.5.5.7.3.1") },
                false));
        }

        DateTimeOffset start = notBefore ?? DateTimeOffset.UtcNow.AddDays(-1);
        return request.CreateSelfSigned(start, start.Add(validFor));
    }

    public static X509Certificate2 CreateEcEphemeral(string subject = "CN=Msixpkg EC Test Publisher")
    {
        using var ecdsa = ECDsa.Create(ECCurve.NamedCurves.nistP256);
        var request = new CertificateRequest(subject, ecdsa, HashAlgorithmName.SHA256);
        request.CertificateExtensions.Add(new X509BasicConstraintsExtension(false, false, 0, false));
        request.CertificateExtensions.Add(new X509KeyUsageExtension(X509KeyUsageFlags.DigitalSignature, false));
        request.CertificateExtensions.Add(new X509EnhancedKeyUsageExtension(
            new OidCollection { new Oid(MsixpkgValidator.CodeSigningEkuOid) },
            false));
        return request.CreateSelfSigned(DateTimeOffset.UtcNow.AddDays(-1), DateTimeOffset.UtcNow.AddYears(5));
    }
}

internal static class SampleMsixFactory
{
    /// <summary>
    /// 合成一个 .msix（ZIP + AppxManifest.xml + 载荷 + AppxSignature.p7x）。
    /// 真实 MSIX 的 p7x 内容是自定义结构；这里只需要一个能被 X509Certificate2Collection.Import
    /// 解析出证书的 PKCS#7，用于验证「签名者身份提取」这条代码路径。
    /// </summary>
    public static byte[] Create(
        string identityName,
        string publisher,
        string version,
        string architecture,
        X509Certificate2 signer,
        IReadOnlyList<(string Name, string MinVersion)>? dependencies = null,
        string payloadText = "sample-application-payload",
        string applicationId = "App")
    {
        using var buffer = new MemoryStream();
        using (var archive = new ZipArchive(buffer, ZipArchiveMode.Create, leaveOpen: true))
        {
            AddText(archive, "AppxManifest.xml", BuildManifestXml(identityName, publisher, version, architecture, dependencies, applicationId));
            AddText(archive, "App.exe", payloadText);

            var certificates = new X509Certificate2Collection(signer);
            AddBytes(archive, "AppxSignature.p7x", certificates.Export(X509ContentType.Pkcs7)!);
        }

        return buffer.ToArray();
    }

    /// <summary>未签名的 MSIX（缺少 AppxSignature.p7x）——用于验证「必须已签名」这条前置检查。</summary>
    public static byte[] CreateUnsigned(
        string identityName,
        string publisher,
        string version,
        string architecture,
        string payloadText = "unsigned-payload")
    {
        using var buffer = new MemoryStream();
        using (var archive = new ZipArchive(buffer, ZipArchiveMode.Create, leaveOpen: true))
        {
            AddText(archive, "AppxManifest.xml", BuildManifestXml(identityName, publisher, version, architecture));
            AddText(archive, "App.exe", payloadText);
        }

        return buffer.ToArray();
    }

    public static string BuildManifestXml(        string identityName,
        string publisher,
        string version,
        string architecture,
        IReadOnlyList<(string Name, string MinVersion)>? dependencies = null,
        string applicationId = "App")
    {
        var builder = new StringBuilder();
        builder.AppendLine("<?xml version=\"1.0\" encoding=\"utf-8\"?>");
        builder.AppendLine("<Package xmlns=\"http://schemas.microsoft.com/appx/manifest/foundation/windows10\">");
        builder.AppendLine("  <Identity Name=\"" + Escape(identityName) + "\" Publisher=\"" + Escape(publisher)
            + "\" Version=\"" + Escape(version) + "\" ProcessorArchitecture=\"" + Escape(architecture) + "\" />");
        builder.AppendLine("  <Properties>");
        builder.AppendLine("    <DisplayName>Msixpkg Sample</DisplayName>");
        builder.AppendLine("    <PublisherDisplayName>MsixpkgInstaller</PublisherDisplayName>");
        builder.AppendLine("    <Logo>Assets\\StoreLogo.png</Logo>");
        builder.AppendLine("  </Properties>");
        builder.AppendLine("  <Dependencies>");
        builder.AppendLine("    <TargetDeviceFamily Name=\"Windows.Desktop\" MinVersion=\"10.0.19041.0\" MaxVersionTested=\"10.0.26100.0\" />");
        if (dependencies != null)
        {
            foreach ((string name, string minVersion) in dependencies)
            {
                builder.AppendLine("    <PackageDependency Name=\"" + Escape(name) + "\" MinVersion=\"" + Escape(minVersion) + "\" />");
            }
        }

        builder.AppendLine("  </Dependencies>");
        builder.AppendLine("  <Applications>");
        builder.AppendLine("    <Application Id=\"" + Escape(applicationId) + "\" Executable=\"App.exe\" EntryPoint=\"App.App\" />");
        builder.AppendLine("  </Applications>");
        builder.AppendLine("</Package>");
        return builder.ToString();
    }

    private static void AddText(ZipArchive archive, string name, string text)
    {
        AddBytes(archive, name, Encoding.UTF8.GetBytes(text));
    }

    private static void AddBytes(ZipArchive archive, string name, byte[] content)
    {
        ZipArchiveEntry entry = archive.CreateEntry(name, CompressionLevel.NoCompression);
        entry.LastWriteTime = new DateTimeOffset(2026, 1, 1, 0, 0, 0, TimeSpan.Zero);
        using Stream stream = entry.Open();
        stream.Write(content, 0, content.Length);
    }

    private static string Escape(string value)
    {
        return System.Security.SecurityElement.Escape(value) ?? value;
    }
}

internal static class TestValidation
{
    /// <summary>
    /// 统一的校验选项工厂。TempDirectory 必须指向工作区内的 artifacts 目录 ——
    /// 受限执行环境下系统临时目录可能不可写，而校验器默认用 Path.GetTempPath()。
    /// </summary>
    public static MsixpkgValidateOptions Options(Action<MsixpkgValidateOptions>? configure = null)
    {
        var options = new MsixpkgValidateOptions
        {
            HostBuildNumber = 26100,
            InstallerVersion = "0.1.0",
            TempDirectory = Path.Combine(TestPaths.ArtifactsDirectory, "tmp"),
        };

        configure?.Invoke(options);
        return options;
    }
}

internal static class ZipTestHelpers
{
    /// <summary>逐字节复制容器，并把某个条目替换成别的字节 —— 用于构造哈希不匹配的负例。</summary>
    public static void RepackWithReplacement(string sourcePath, string entryName, byte[] replacement, string destinationPath)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(destinationPath))!);
        using var source = new FileStream(sourcePath, FileMode.Open, FileAccess.Read, FileShare.Read);
        using var sourceArchive = new ZipArchive(source, ZipArchiveMode.Read);
        using var destination = new FileStream(destinationPath, FileMode.Create, FileAccess.ReadWrite, FileShare.None);
        using var destinationArchive = new ZipArchive(destination, ZipArchiveMode.Create);

        foreach (ZipArchiveEntry entry in sourceArchive.Entries)
        {
            ZipArchiveEntry target = destinationArchive.CreateEntry(entry.FullName, CompressionLevel.NoCompression);
            target.LastWriteTime = entry.LastWriteTime;
            using Stream input = entry.Open();
            using Stream output = target.Open();
            if (string.Equals(entry.FullName, entryName, StringComparison.Ordinal))
            {
                output.Write(replacement, 0, replacement.Length);
            }
            else
            {
                input.CopyTo(output);
            }
        }
    }

    /// <summary>逐字节复制容器，并追加一个清单未声明的条目 —— 用于构造「多余条目」负例。</summary>
    public static void RepackWithExtraEntry(string sourcePath, string extraEntryName, byte[] extraContent, string destinationPath)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(destinationPath))!);
        using var source = new FileStream(sourcePath, FileMode.Open, FileAccess.Read, FileShare.Read);
        using var sourceArchive = new ZipArchive(source, ZipArchiveMode.Read);
        using var destination = new FileStream(destinationPath, FileMode.Create, FileAccess.ReadWrite, FileShare.None);
        using var destinationArchive = new ZipArchive(destination, ZipArchiveMode.Create);

        foreach (ZipArchiveEntry entry in sourceArchive.Entries)
        {
            ZipArchiveEntry target = destinationArchive.CreateEntry(entry.FullName, CompressionLevel.NoCompression);
            target.LastWriteTime = entry.LastWriteTime;
            using Stream input = entry.Open();
            using Stream output = target.Open();
            input.CopyTo(output);
        }

        ZipArchiveEntry extra = destinationArchive.CreateEntry(extraEntryName, CompressionLevel.NoCompression);
        extra.LastWriteTime = new DateTimeOffset(2026, 1, 1, 0, 0, 0, TimeSpan.Zero);
        using Stream extraStream = extra.Open();
        extraStream.Write(extraContent, 0, extraContent.Length);
    }

    public static IReadOnlyList<string> EntryNames(string path)
    {
        using var stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read);
        using var archive = new ZipArchive(stream, ZipArchiveMode.Read);
        return archive.Entries.Select(e => e.FullName).ToList();
    }

    /// <summary>逐字节复制容器但丢弃某个条目 —— 用于构造「声明了但缺失」的负例。</summary>
    public static void RepackWithoutEntry(string sourcePath, string entryToRemove, string destinationPath)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(destinationPath))!);
        using var source = new FileStream(sourcePath, FileMode.Open, FileAccess.Read, FileShare.Read);
        using var sourceArchive = new ZipArchive(source, ZipArchiveMode.Read);
        using var destination = new FileStream(destinationPath, FileMode.Create, FileAccess.ReadWrite, FileShare.None);
        using var destinationArchive = new ZipArchive(destination, ZipArchiveMode.Create);

        foreach (ZipArchiveEntry entry in sourceArchive.Entries)
        {
            if (!string.Equals(entry.FullName, entryToRemove, StringComparison.Ordinal))
            {
                CopyEntry(entry, destinationArchive);
            }
        }
    }

    /// <summary>把指定条目挪到最前面 —— 用于构造「清单不是第一个条目」的负例。</summary>
    public static void RepackReordered(string sourcePath, string firstEntryName, string destinationPath)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(destinationPath))!);
        using var source = new FileStream(sourcePath, FileMode.Open, FileAccess.Read, FileShare.Read);
        using var sourceArchive = new ZipArchive(source, ZipArchiveMode.Read);
        using var destination = new FileStream(destinationPath, FileMode.Create, FileAccess.ReadWrite, FileShare.None);
        using var destinationArchive = new ZipArchive(destination, ZipArchiveMode.Create);

        ZipArchiveEntry first = sourceArchive.GetEntry(firstEntryName)
            ?? throw new InvalidOperationException("Entry not found: " + firstEntryName);

        CopyEntry(first, destinationArchive);
        foreach (ZipArchiveEntry entry in sourceArchive.Entries)
        {
            if (!string.Equals(entry.FullName, firstEntryName, StringComparison.Ordinal))
            {
                CopyEntry(entry, destinationArchive);
            }
        }
    }

    private static void CopyEntry(ZipArchiveEntry entry, ZipArchive destination)
    {
        ZipArchiveEntry target = destination.CreateEntry(entry.FullName, CompressionLevel.NoCompression);
        target.LastWriteTime = entry.LastWriteTime;
        using Stream input = entry.Open();
        using Stream output = target.Open();
        input.CopyTo(output);
    }
}
