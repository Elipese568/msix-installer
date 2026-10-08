using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Security.Cryptography.X509Certificates;
using Msixpkg.Core.Appx;

namespace Msixpkg.Core.Tests;

/// <summary>
/// 真实 Appx 签名的回归测试。
///
/// 夹具 real-appxsignature.p7x 是从一个**真实构建**的 WinUI 3 单项目 MSIX
/// （VS 2026 / Windows 11 26300 / `dotnet build -c Release -p:Platform=x64`）里抽出来的
/// AppxSignature.p7x，所以它带着真实格式的全部特征：4 字节头 "PKCX" + DER SignedData。
///
/// 这条测试存在的原因：合成夹具用的是证书袋 PKCS#7，而 Windows 的证书集合导入路径
/// 解析不了真实 Appx 签名（CRYPT_E_NOT_FOUND）—— 只有拿真实产物才能发现这个差异。
/// </summary>
public class RealAppxSignatureTests
{
    private static string FixturePath => Path.Combine(TestPaths.VectorsDirectory, "real-appxsignature.p7x");

    [Fact]
    public void FixtureIsPresentAndCarriesThePkcxHeader()
    {
        Assert.True(File.Exists(FixturePath), "Missing fixture: " + FixturePath);

        byte[] raw = File.ReadAllBytes(FixturePath);
        Assert.True(raw.Length > 8);
        Assert.Equal((byte)'P', raw[0]);
        Assert.Equal((byte)'K', raw[1]);
        Assert.Equal((byte)'C', raw[2]);
        Assert.Equal((byte)'X', raw[3]);

        // 头部之后紧跟 DER SEQUENCE。
        Assert.Equal(0x30, raw[4]);
    }

    [Fact]
    public void ExtractsTheSignerCertificateFromTheRealAppxSignature()
    {
        byte[] raw = File.ReadAllBytes(FixturePath);

        IReadOnlyList<X509Certificate2> certificates = Pkcs7CertificateReader.ExtractCertificates(raw);

        Assert.NotEmpty(certificates);
        Assert.Contains(certificates, c => c.Subject == "CN=AppPublisher");

        // 真实签名的证书集里应能找到一个可用的代码签名证书。
        X509Certificate2 signer = certificates.First(c => c.Subject == "CN=AppPublisher");
        Assert.False(string.IsNullOrEmpty(Thumbprints.Sha256Hex(signer)));
        Assert.True(signer.NotAfter.ToUniversalTime() > new DateTime(2026, 1, 1, 0, 0, 0, DateTimeKind.Utc));
    }

    [Fact]
    public void WindowsCertificateCollectionImportCannotParseTheRealSignature()
    {
        // 这条断言把「为什么需要自己写 DER 解析」固化下来：
        // 一旦将来某个运行时版本能直接 Import 真实 Appx 签名，这条测试会失败并提醒我们简化代码。
        byte[] raw = File.ReadAllBytes(FixturePath);
        var collection = new X509Certificate2Collection();

        Assert.ThrowsAny<System.Security.Cryptography.CryptographicException>(() => collection.Import(raw));
    }

    [Fact]
    public void RejectsTruncatedAndGarbageInput()
    {
        byte[] raw = File.ReadAllBytes(FixturePath);

        Assert.Throws<MsixpkgFormatException>(() => Pkcs7CertificateReader.ExtractCertificates(raw.Take(12).ToArray()));
        Assert.Throws<MsixpkgFormatException>(() => Pkcs7CertificateReader.ExtractCertificates(new byte[] { 1, 2, 3, 4, 5, 6, 7, 8 }));
        Assert.Throws<MsixpkgFormatException>(() => Pkcs7CertificateReader.ExtractCertificates(Array.Empty<byte>()));
    }

    [Fact]
    public void ReadsTheSignerFromARealMsixThroughTheReaderApi()
    {
        // 用一个「只有 AppxManifest.xml + AppxSignature.p7x」的最小 MSIX 验证组装路径：
        // 真实 MSIX 有 60 MB，不必为了这条测试把它提交进仓库。
        string directory = PackageFixtures.NewTempDirectory();
        string msix = Path.Combine(directory, "real-signature.msix");

        using (var stream = new FileStream(msix, FileMode.Create, FileAccess.ReadWrite, FileShare.None))
        using (var archive = new System.IO.Compression.ZipArchive(stream, System.IO.Compression.ZipArchiveMode.Create))
        {
            WriteEntry(archive, "AppxManifest.xml", SampleMsixFactory.BuildManifestXml(
                "Msixpkg.RealSignatureSample", "CN=AppPublisher", "1.0.0.0", "x64"));
            WriteEntry(archive, "AppxSignature.p7x", File.ReadAllBytes(FixturePath));
        }

        X509Certificate2Collection signers = AppxPackageReader.ReadSignerCertificates(msix);
        Assert.Contains(signers.OfType<X509Certificate2>(), c => c.Subject == "CN=AppPublisher");
    }

    private static void WriteEntry(System.IO.Compression.ZipArchive archive, string name, string text)
    {
        WriteEntry(archive, name, System.Text.Encoding.UTF8.GetBytes(text));
    }

    private static void WriteEntry(System.IO.Compression.ZipArchive archive, string name, byte[] content)
    {
        System.IO.Compression.ZipArchiveEntry entry = archive.CreateEntry(name, System.IO.Compression.CompressionLevel.NoCompression);
        using Stream stream = entry.Open();
        stream.Write(content, 0, content.Length);
    }
}
