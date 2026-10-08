using System;
using System.IO;
using System.Security.Cryptography.X509Certificates;
using System.Text;

namespace Msixpkg.Core.Tests;

public class SignatureTests
{
    private static X509Certificate2 Signer => TestCertificates.LoadOrCreateTestCertificate();

    private static byte[] SampleDigest()
    {
        return MsixpkgContentDigest.Compute(new[]
        {
            new MsixpkgContentDigestEntry("msixpkg.json", new string('a', 64)),
            new MsixpkgContentDigestEntry("payload/x64/main/App_1.0.0.0_x64.msix", new string('b', 64)),
        });
    }

    [Fact]
    public void RsaSigningUsesTheDeterministicAlgorithm()
    {
        MsixpkgSignature signature = MsixpkgSigner.Sign(SampleDigest(), Signer);
        Assert.Equal(MsixpkgSignatureAlgorithms.RsaPkcs1Sha256, signature.Algorithm);
        Assert.Equal(Thumbprints.Sha256Hex(Signer), signature.KeyThumbprintSha256);
    }

    [Fact]
    public void SigningIsDeterministic()
    {
        byte[] digest = SampleDigest();
        MsixpkgSignature first = MsixpkgSigner.Sign(digest, Signer);
        MsixpkgSignature second = MsixpkgSigner.Sign(digest, Signer);
        Assert.Equal(first.SignatureBase64, second.SignatureBase64);
    }

    [Fact]
    public void VerifyAcceptsItsOwnSignatureWithoutAnyTrustStore()
    {
        byte[] digest = SampleDigest();
        MsixpkgSignature signature = MsixpkgSigner.Sign(digest, Signer);

        // 测试证书从未安装进任何证书存储 —— 验签必须仅依赖公钥，不依赖链信任。
        MsixpkgSigner.Verify(digest, signature, Signer);
    }

    [Fact]
    public void VerifyRejectsTamperedSignatureBytes()
    {
        byte[] digest = SampleDigest();
        MsixpkgSignature signature = MsixpkgSigner.Sign(digest, Signer);

        byte[] raw = Convert.FromBase64String(signature.SignatureBase64);
        raw[raw.Length / 2] ^= 0xFF;

        var tampered = new MsixpkgSignature(
            signature.Algorithm,
            signature.KeyThumbprintSha256,
            signature.ContentDigestHex,
            Convert.ToBase64String(raw));

        MsixpkgFormatException ex = Assert.Throws<MsixpkgFormatException>(() => MsixpkgSigner.Verify(digest, tampered, Signer));
        Assert.Equal(MsixpkgErrorCodes.SignatureInvalid, ex.Code);
    }

    [Fact]
    public void VerifyRejectsDifferentDigest()
    {
        MsixpkgSignature signature = MsixpkgSigner.Sign(SampleDigest(), Signer);
        byte[] other = new byte[32];
        Array.Fill(other, (byte)0x5A);

        Assert.Throws<MsixpkgFormatException>(() => MsixpkgSigner.Verify(other, signature, Signer));
    }

    [Fact]
    public void VerifyRejectsDifferentKey()
    {
        byte[] digest = SampleDigest();
        MsixpkgSignature signature = MsixpkgSigner.Sign(digest, Signer);
        using X509Certificate2 otherSigner = TestCertificates.CreateEphemeral(TestCertificates.PublisherDn, TimeSpan.FromDays(3650));

        Assert.Throws<MsixpkgFormatException>(() => MsixpkgSigner.Verify(digest, signature, otherSigner));
    }

    [Fact]
    public void EcdsaRequiresExplicitOptIn()
    {
        using X509Certificate2 ec = TestCertificates.CreateEcEphemeral();
        string directory = PackageFixtures.NewTempDirectory();
        string mainMsix = PackageFixtures.CreateMainMsix(directory, ec, publisher: ec.Subject);

        var builder = new MsixpkgPackageBuilder(new MsixpkgIdentity("Msixpkg.Sample", ec.Subject, PackageFixtures.Version, "x64"));
        builder.SetMainPackage(mainMsix);

        MsixpkgFormatException ex = Assert.Throws<MsixpkgFormatException>(() => builder.Write(
            Path.Combine(directory, "ec.msixpkg"),
            ec,
            new MsixpkgWriteOptions { CreatedUtc = PackageFixtures.FixedCreatedUtc }));
        Assert.Equal(MsixpkgErrorCodes.SignatureAlgorithmUnsupported, ex.Code);
    }

    [Fact]
    public void EcdsaRoundTripsWhenOptedIn()
    {
        using X509Certificate2 ec = TestCertificates.CreateEcEphemeral();
        string directory = PackageFixtures.NewTempDirectory();
        string mainMsix = PackageFixtures.CreateMainMsix(directory, ec, publisher: ec.Subject);

        var builder = new MsixpkgPackageBuilder(new MsixpkgIdentity("Msixpkg.Sample", ec.Subject, PackageFixtures.Version, "x64"));
        builder.SetMainPackage(mainMsix);
        string containerPath = Path.Combine(directory, "ec.msixpkg");
        builder.Write(containerPath, ec, new MsixpkgWriteOptions
        {
            CreatedUtc = PackageFixtures.FixedCreatedUtc,
            AllowNonDeterministicSignature = true,
        });

        MsixpkgValidationReport report = MsixpkgValidator.Validate(containerPath, TestValidation.Options(o =>
        {
            o.HostArchitecture = "x64";
            o.AllowedPublisherThumbprints = new[] { Thumbprints.Sha256Hex(ec) };
        }));

        Assert.True(report.IsValid, string.Join("; ", report.Errors));
    }

    [Fact]
    public void ContainerWithTamperedSignatureFileIsRejected()
    {
        PackageFixtures.BuiltPackage package = PackageFixtures.Build(Signer);

        string signatureJson;
        using (MsixpkgContainer container = MsixpkgContainer.Open(package.ContainerPath))
        {
            signatureJson = container.ReadEntryText(MsixpkgPaths.SignatureEntry);
        }

        MsixpkgSignature signature = MsixpkgSignatureCodec.Parse(signatureJson);
        byte[] raw = Convert.FromBase64String(signature.SignatureBase64);
        raw[4] ^= 0xFF;
        string tamperedJson = MsixpkgSignatureCodec.Write(new MsixpkgSignature(
            signature.Algorithm,
            signature.KeyThumbprintSha256,
            signature.ContentDigestHex,
            Convert.ToBase64String(raw)));

        string tamperedPath = Path.Combine(package.Directory, "bad-signature.msixpkg");
        ZipTestHelpers.RepackWithReplacement(
            package.ContainerPath,
            MsixpkgPaths.SignatureEntry,
            Encoding.UTF8.GetBytes(tamperedJson),
            tamperedPath);

        MsixpkgValidationReport report = MsixpkgValidator.Validate(tamperedPath, TestValidation.Options(o =>
        {
            o.HostArchitecture = "x64";
            o.AllowedPublisherThumbprints = new[] { Thumbprints.Sha256Hex(Signer) };
        }));

        Assert.False(report.IsValid);
        Assert.Contains(report.Errors, e => e.Code == MsixpkgErrorCodes.SignatureInvalid);
    }
}
