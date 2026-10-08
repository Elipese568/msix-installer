using System;
using System.IO;
using System.Linq;
using System.Security.Cryptography.X509Certificates;
using System.Text;

namespace Msixpkg.Core.Tests;

public class ValidatorTests
{
    private static X509Certificate2 Signer => TestCertificates.LoadOrCreateTestCertificate();

    private static MsixpkgValidateOptions Options(Action<MsixpkgValidateOptions>? configure = null)
    {
        return TestValidation.Options(o =>
        {
            o.HostArchitecture = PackageFixtures.Architecture;
            o.AllowedPublisherThumbprints = new[] { Thumbprints.Sha256Hex(Signer) };
            configure?.Invoke(o);
        });
    }

    [Fact]
    public void ValidContainerPasses()
    {
        PackageFixtures.BuiltPackage package = PackageFixtures.Build(Signer);
        MsixpkgValidationReport report = MsixpkgValidator.Validate(package.ContainerPath, Options());

        Assert.True(report.IsValid, string.Join("; ", report.Errors.Select(e => e.ToString())));
        Assert.True(report.PublisherCertificateWasPinned);
        Assert.Empty(report.Warnings);
    }

    [Fact]
    public void TamperedPayloadReportsHashMismatch()
    {
        PackageFixtures.BuiltPackage package = PackageFixtures.Build(Signer);
        byte[] original;
        using (MsixpkgContainer container = MsixpkgContainer.Open(package.ContainerPath))
        {
            original = container.ReadEntryBytes(PackageFixtures.MainEntryName);
        }

        byte[] tampered = (byte[])original.Clone();
        tampered[tampered.Length - 1] ^= 0xFF;

        string tamperedPath = Path.Combine(package.Directory, "tampered.msixpkg");
        ZipTestHelpers.RepackWithReplacement(package.ContainerPath, PackageFixtures.MainEntryName, tampered, tamperedPath);

        MsixpkgValidationReport report = MsixpkgValidator.Validate(tamperedPath, Options());
        Assert.False(report.IsValid);

        // 允许两种探测路径（docs/msixpkg-format-v1.md §5 的注记）：
        // 原生实现会在 ZIP 层用 CRC-32 先拦下，报 E_CONTAINER_INVALID；
        // .NET 的 ZipArchive 不校验 CRC，于是走到清单哈希比对，报 E_HASH_MISMATCH。
        // 两者都是完整性失败，GUI 必须一并处理。
        Assert.Contains(
            report.Errors,
            e => e.Code == MsixpkgErrorCodes.HashMismatch || e.Code == MsixpkgErrorCodes.ContainerInvalid);
    }

    [Fact]
    public void MissingMainPackageReportsFileMissing()
    {
        PackageFixtures.BuiltPackage package = PackageFixtures.Build(Signer);
        string missing = Path.Combine(package.Directory, "missing.msixpkg");
        ZipTestHelpers.RepackWithoutEntry(package.ContainerPath, PackageFixtures.MainEntryName, missing);

        MsixpkgValidationReport report = MsixpkgValidator.Validate(missing, Options());
        Assert.False(report.IsValid);
        Assert.Contains(report.Errors, e => e.Code == MsixpkgErrorCodes.FileMissing);
    }

    [Fact]
    public void UndeclaredDependencyEntryReportsEntryUnknown()
    {
        PackageFixtures.BuiltPackage package = PackageFixtures.Build(Signer);
        string extra = Path.Combine(package.Directory, "undeclared.msixpkg");
        ZipTestHelpers.RepackWithExtraEntry(
            package.ContainerPath,
            "payload/x64/deps/Undeclared_2.0.0.0_x64.msix",
            Encoding.UTF8.GetBytes("pk"),
            extra);

        MsixpkgValidationReport report = MsixpkgValidator.Validate(extra, Options());
        Assert.False(report.IsValid);
        Assert.Contains(report.Errors, e => e.Code == MsixpkgErrorCodes.EntryUnknown);
    }

    [Fact]
    public void MissingSignatureFailsWhenRequired()
    {
        PackageFixtures.BuiltPackage package = PackageFixtures.Build(Signer, sign: false);
        MsixpkgValidationReport report = MsixpkgValidator.Validate(package.ContainerPath, Options());

        Assert.False(report.IsValid);
        Assert.Contains(report.Errors, e => e.Code == MsixpkgErrorCodes.SignatureMissing);
    }

    [Fact]
    public void MissingSignatureIsAllowedWhenNotRequired()
    {
        PackageFixtures.BuiltPackage package = PackageFixtures.Build(Signer, sign: false);
        MsixpkgValidationReport report = MsixpkgValidator.Validate(package.ContainerPath, Options(o => o.RequireSignature = false));

        Assert.True(report.IsValid, string.Join("; ", report.Errors.Select(e => e.ToString())));
    }

    [Fact]
    public void ArchitectureMismatchIsReported()
    {
        PackageFixtures.BuiltPackage package = PackageFixtures.Build(Signer);
        MsixpkgValidationReport report = MsixpkgValidator.Validate(package.ContainerPath, Options(o => o.HostArchitecture = "arm64"));

        Assert.False(report.IsValid);
        Assert.Contains(report.Errors, e => e.Code == MsixpkgErrorCodes.ArchitectureMismatch);
    }

    [Fact]
    public void X86PackageIsAcceptedOnX64Host()
    {
        PackageFixtures.BuiltPackage package = PackageFixtures.Build(Signer, architecture: "x86");
        MsixpkgValidationReport report = MsixpkgValidator.Validate(package.ContainerPath, Options(o => o.HostArchitecture = "x64"));

        Assert.True(report.IsValid, string.Join("; ", report.Errors.Select(e => e.ToString())));
    }

    [Fact]
    public void OsTooOldIsReported()
    {
        PackageFixtures.BuiltPackage package = PackageFixtures.Build(Signer);
        MsixpkgValidationReport report = MsixpkgValidator.Validate(package.ContainerPath, Options(o => o.HostBuildNumber = 17763));

        Assert.False(report.IsValid);
        Assert.Contains(report.Errors, e => e.Code == MsixpkgErrorCodes.OsTooOld);
    }

    [Fact]
    public void InstallerTooOldIsReported()
    {
        PackageFixtures.BuiltPackage package = PackageFixtures.Build(Signer);
        MsixpkgValidationReport report = MsixpkgValidator.Validate(package.ContainerPath, Options(o => o.InstallerVersion = "0.0.1"));

        Assert.False(report.IsValid);
        Assert.Contains(report.Errors, e => e.Code == MsixpkgErrorCodes.InstallerTooOld);
    }

    [Fact]
    public void UnpinnedPublisherIsRejected()
    {
        PackageFixtures.BuiltPackage package = PackageFixtures.Build(Signer);
        MsixpkgValidationReport report = MsixpkgValidator.Validate(package.ContainerPath, Options(o =>
        {
            o.AllowedPublisherThumbprints = new[] { new string('0', 64) };
        }));

        Assert.False(report.IsValid);
        Assert.Contains(report.Errors, e => e.Code == MsixpkgErrorCodes.NotPinned);
    }

    [Fact]
    public void PinningRequiredWithoutListIsRejected()
    {
        PackageFixtures.BuiltPackage package = PackageFixtures.Build(Signer);
        MsixpkgValidationReport report = MsixpkgValidator.Validate(package.ContainerPath, Options(o =>
        {
            o.AllowedPublisherThumbprints = null;
            o.RequirePinnedPublisher = true;
        }));

        Assert.False(report.IsValid);
        Assert.Contains(report.Errors, e => e.Code == MsixpkgErrorCodes.NotPinned);
    }

    [Fact]
    public void ExpiredCertificateIsReported()
    {
        using X509Certificate2 expired = TestCertificates.CreateEphemeral(
            TestCertificates.PublisherDn,
            TimeSpan.FromDays(365),
            notBefore: DateTimeOffset.UtcNow.AddDays(-800));

        PackageFixtures.BuiltPackage package = PackageFixtures.Build(expired);
        MsixpkgValidationReport report = MsixpkgValidator.Validate(package.ContainerPath, Options(o =>
            o.AllowedPublisherThumbprints = new[] { Thumbprints.Sha256Hex(expired) }));

        Assert.False(report.IsValid);
        Assert.Contains(report.Errors, e => e.Code == MsixpkgErrorCodes.CertificateExpired);
    }

    [Fact]
    public void WrongKeyUsageCertificateIsReported()
    {
        using X509Certificate2 wrongEku = TestCertificates.CreateEphemeral(
            TestCertificates.PublisherDn,
            TimeSpan.FromDays(3650),
            codeSigningEku: false);

        PackageFixtures.BuiltPackage package = PackageFixtures.Build(wrongEku);
        MsixpkgValidationReport report = MsixpkgValidator.Validate(package.ContainerPath, Options(o =>
            o.AllowedPublisherThumbprints = new[] { Thumbprints.Sha256Hex(wrongEku) }));

        Assert.False(report.IsValid);
        Assert.Contains(report.Errors, e => e.Code == MsixpkgErrorCodes.CertificateNotCodeSigning);
    }

    [Fact]
    public void CertificateSubjectMustMatchIdentityPublisher()
    {
        PackageFixtures.BuiltPackage package = PackageFixtures.Build(
            Signer,
            identityPublisher: "CN=Someone Else, O=Other");

        MsixpkgValidationReport report = MsixpkgValidator.Validate(package.ContainerPath, Options());
        Assert.False(report.IsValid);
        Assert.Contains(report.Errors, e => e.Code == MsixpkgErrorCodes.PublisherMismatch);
    }

    [Fact]
    public void InnerAppxIdentityMustMatchContainerIdentity()
    {
        string directory = PackageFixtures.NewTempDirectory();

        // 内部 Appx 的真实标识是 Msixpkg.Inner，而容器清单声明的是 Msixpkg.Outer。
        string mainMsix = PackageFixtures.CreateMainMsix(directory, Signer, identityName: "Msixpkg.Inner");
        var builder = new MsixpkgPackageBuilder(new MsixpkgIdentity(
            "Msixpkg.Outer",
            Signer.Subject,
            PackageFixtures.Version,
            PackageFixtures.Architecture));
        builder.SetMainPackage(mainMsix);
        string containerPath = Path.Combine(directory, "identity-mismatch.msixpkg");
        builder.Write(containerPath, Signer, new MsixpkgWriteOptions { CreatedUtc = PackageFixtures.FixedCreatedUtc });

        MsixpkgValidationReport report = MsixpkgValidator.Validate(containerPath, Options());
        Assert.False(report.IsValid);
        Assert.Contains(report.Errors, e => e.Code == MsixpkgErrorCodes.AppxInvalid);
    }

    [Fact]
    public void InnerAppxSignedByDifferentCertificateIsReported()
    {
        using X509Certificate2 other = TestCertificates.CreateEphemeral(TestCertificates.PublisherDn, TimeSpan.FromDays(3650));
        string directory = PackageFixtures.NewTempDirectory();

        // 主包用 other 签名，但容器用 Signer 签名并携带 Signer 的证书。
        string mainMsix = PackageFixtures.CreateMainMsix(directory, other);
        var builder = new MsixpkgPackageBuilder(new MsixpkgIdentity(
            PackageFixtures.IdentityName,
            Signer.Subject,
            PackageFixtures.Version,
            PackageFixtures.Architecture));
        builder.SetMainPackage(mainMsix);
        string containerPath = Path.Combine(directory, "mismatch.msixpkg");
        builder.Write(containerPath, Signer, new MsixpkgWriteOptions { CreatedUtc = PackageFixtures.FixedCreatedUtc });

        MsixpkgValidationReport report = MsixpkgValidator.Validate(containerPath, Options());
        Assert.False(report.IsValid);
        Assert.Contains(report.Errors, e => e.Code == MsixpkgErrorCodes.AppxSignerMismatch);
    }

    [Fact]
    public void ReusedExtractedMainPackageIsVerifiedToo()
    {
        PackageFixtures.BuiltPackage package = PackageFixtures.Build(Signer);

        // 故意提供一个内容不同的“已解包主包”，校验必须发现它与清单声明不符。
        string fake = Path.Combine(package.Directory, "fake.msix");
        File.WriteAllBytes(fake, SampleMsixFactory.Create("Other.App", Signer.Subject, "9.9.9.9", "x64", Signer));

        MsixpkgValidationReport report = MsixpkgValidator.Validate(package.ContainerPath, Options(o =>
        {
            o.ExtractedMainPackagePath = fake;
        }));

        Assert.False(report.IsValid);
        Assert.Contains(
            report.Errors,
            e => e.Code == MsixpkgErrorCodes.HashMismatch || e.Code == MsixpkgErrorCodes.SizeMismatch);
    }
}
