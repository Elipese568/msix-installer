using System;
using System.IO;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;
using Msixpkg.Core.Packing;

namespace Msixpkg.Core.Tests;

/// <summary>
/// 证书加载的回归测试。存在的理由是一次真实踩坑：把 EphemeralKeySet 的值写成 0x2000（正确值 0x20），
/// 导致 .NET 9 的加载器抛 ArgumentException；而退回 Exportable 时加载“成功”却得到一个不可用的私钥，
/// 错误一直延迟到签名阶段才以「密钥集不存在」的形式暴露。
/// 因此这里必须断言：加载出来的证书能真正完成一次签名。
/// </summary>
public class CertificateLoadingTests
{
    [Fact]
    public void LoadingTheSigningCertificateYieldsAUsablePrivateKey()
    {
        X509Certificate2 certificate = MsixpkgCertificates.LoadSigningCertificateFromFile(
            TestPaths.TestCertificatePath,
            TestCertificates.Password);

        Assert.True(certificate.HasPrivateKey);

        using RSA? rsa = certificate.GetRSAPrivateKey();
        Assert.NotNull(rsa);

        byte[] digest = SHA256.HashData(new byte[] { 1, 2, 3 });
        byte[] signature = rsa!.SignHash(digest, HashAlgorithmName.SHA256, RSASignaturePadding.Pkcs1);
        Assert.True(rsa.VerifyHash(digest, signature, HashAlgorithmName.SHA256, RSASignaturePadding.Pkcs1));
    }

    [Fact]
    public void WrongPasswordIsReportedAsCertificateMalformed()
    {
        MsixpkgFormatException ex = Assert.Throws<MsixpkgFormatException>(
            () => MsixpkgCertificates.LoadSigningCertificateFromFile(TestPaths.TestCertificatePath, "definitely-not-the-password"));

        Assert.Equal(MsixpkgErrorCodes.CertificateMalformed, ex.Code);
    }

    [Fact]
    public void MissingFileIsReportedAsIoError()
    {
        string missing = Path.Combine(TestPaths.ArtifactsDirectory, "does-not-exist.pfx");
        MsixpkgFormatException ex = Assert.Throws<MsixpkgFormatException>(
            () => MsixpkgCertificates.LoadSigningCertificateFromFile(missing, "x"));

        Assert.Equal(MsixpkgErrorCodes.IoError, ex.Code);
    }

    [Fact]
    public void PublicCertificateRoundTripsFromDer()
    {
        X509Certificate2 shared = TestCertificates.LoadOrCreateTestCertificate();
        byte[] der = shared.Export(X509ContentType.Cert);

        using X509Certificate2 loaded = MsixpkgCertificates.LoadPublicCertificate(der);
        Assert.Equal(Thumbprints.Sha256Hex(shared), Thumbprints.Sha256Hex(loaded));
        Assert.False(loaded.HasPrivateKey);
    }
}
