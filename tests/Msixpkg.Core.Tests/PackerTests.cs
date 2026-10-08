using System;
using System.IO;
using System.Linq;
using System.Security.Cryptography.X509Certificates;
using Msixpkg.Core.Packing;

namespace Msixpkg.Core.Tests;

public class PackerTests
{
    private static X509Certificate2 Signer => TestCertificates.LoadOrCreateTestCertificate();

    private static MsixpkgPackRequest Request(string directory, string msix, string? dependencyDirectory = null, string? password = null)
    {
        return new MsixpkgPackRequest
        {
            MsixPath = msix,
            PfxPath = TestPaths.TestCertificatePath,
            PfxPassword = password ?? TestCertificates.Password,
            OutputPath = Path.Combine(directory, "packed.msixpkg"),
            DependencyDirectory = dependencyDirectory,
            CreatedUtc = PackageFixtures.FixedCreatedUtc,
        };
    }

    [Fact]
    public void PackProducesAValidContainer()
    {
        string directory = PackageFixtures.NewTempDirectory();
        string dependencyDirectory = Path.Combine(directory, "deps");
        Directory.CreateDirectory(dependencyDirectory);
        File.Copy(PackageFixtures.CreateDependencyMsix(directory, Signer), Path.Combine(dependencyDirectory, "dep.msix"));
        string mainMsix = PackageFixtures.CreateMainMsix(directory, Signer);

        MsixpkgPackResult result = MsixpkgPacker.Pack(Request(directory, mainMsix, dependencyDirectory));

        Assert.True(File.Exists(result.OutputPath));
        Assert.True(File.Exists(result.CertificatePath));
        Assert.Single(result.Manifest.Dependencies);
        Assert.NotNull(result.Manifest.Publisher);
        Assert.Equal(Thumbprints.Sha256Hex(Signer), result.Manifest.Publisher!.ThumbprintSha256);

        MsixpkgValidationReport report = MsixpkgValidator.Validate(result.OutputPath, TestValidation.Options(o =>
        {
            o.HostArchitecture = PackageFixtures.Architecture;
            o.AllowedPublisherThumbprints = new[] { Thumbprints.Sha256Hex(Signer) };
        }));

        Assert.True(report.IsValid, string.Join("; ", report.Errors.Select(e => e.ToString())));
    }

    [Fact]
    public void ExportedCertificateMatchesTheEmbeddedOne()
    {
        string directory = PackageFixtures.NewTempDirectory();
        string mainMsix = PackageFixtures.CreateMainMsix(directory, Signer, declareDependency: false);
        MsixpkgPackResult result = MsixpkgPacker.Pack(Request(directory, mainMsix));

        using MsixpkgContainer container = MsixpkgContainer.Open(result.OutputPath);
        byte[] embedded = container.ReadEntryBytes(MsixpkgPaths.PublisherCertificateEntry);

        Assert.Equal(embedded, File.ReadAllBytes(result.CertificatePath));
    }

    [Fact]
    public void PackRejectsAnUnsignedMsix()
    {
        string directory = PackageFixtures.NewTempDirectory();
        string mainMsix = Path.Combine(directory, "unsigned.msix");
        File.WriteAllBytes(mainMsix, SampleMsixFactory.CreateUnsigned(
            PackageFixtures.IdentityName,
            Signer.Subject,
            PackageFixtures.Version,
            PackageFixtures.Architecture));

        MsixpkgFormatException ex = Assert.Throws<MsixpkgFormatException>(() => MsixpkgPacker.Pack(Request(directory, mainMsix)));
        Assert.Equal(MsixpkgErrorCodes.AppxSignatureMissing, ex.Code);
    }

    [Fact]
    public void PackRejectsAMsixSignedByAnotherCertificate()
    {
        string directory = PackageFixtures.NewTempDirectory();
        using X509Certificate2 other = TestCertificates.CreateEphemeral(TestCertificates.PublisherDn, TimeSpan.FromDays(3650));
        string mainMsix = Path.Combine(directory, "other.msix");
        File.WriteAllBytes(mainMsix, SampleMsixFactory.Create(
            PackageFixtures.IdentityName,
            other.Subject,
            PackageFixtures.Version,
            PackageFixtures.Architecture,
            other));

        MsixpkgFormatException ex = Assert.Throws<MsixpkgFormatException>(() => MsixpkgPacker.Pack(Request(directory, mainMsix)));
        Assert.Equal(MsixpkgErrorCodes.AppxSignerMismatch, ex.Code);
    }

    [Fact]
    public void PackRejectsPublisherThatDoesNotMatchTheCertificate()
    {
        string directory = PackageFixtures.NewTempDirectory();
        string mainMsix = Path.Combine(directory, "foreign.msix");
        File.WriteAllBytes(mainMsix, SampleMsixFactory.Create(
            PackageFixtures.IdentityName,
            "CN=Not The Certificate Subject, O=Other",
            PackageFixtures.Version,
            PackageFixtures.Architecture,
            Signer));

        MsixpkgFormatException ex = Assert.Throws<MsixpkgFormatException>(() => MsixpkgPacker.Pack(Request(directory, mainMsix)));
        Assert.Equal(MsixpkgErrorCodes.PublisherMismatch, ex.Code);
    }

    [Fact]
    public void PackRejectsMissingDependencyDirectory()
    {
        string directory = PackageFixtures.NewTempDirectory();
        string mainMsix = PackageFixtures.CreateMainMsix(directory, Signer, declareDependency: true);

        MsixpkgFormatException ex = Assert.Throws<MsixpkgFormatException>(() => MsixpkgPacker.Pack(Request(directory, mainMsix)));
        Assert.Equal(MsixpkgErrorCodes.AppxInvalid, ex.Code);
        Assert.Contains(PackageFixtures.DependencyName, ex.Detail);
    }

    [Fact]
    public void PackRejectsDependencyThatIsNotInTheDirectory()
    {
        string directory = PackageFixtures.NewTempDirectory();
        string dependencyDirectory = Path.Combine(directory, "deps");
        Directory.CreateDirectory(dependencyDirectory);

        // 目录里有一个包，但不是清单声明的那一个。
        File.WriteAllBytes(
            Path.Combine(dependencyDirectory, "unrelated.msix"),
            SampleMsixFactory.Create("Some.Other.Framework", Signer.Subject, "1.0.0.0", PackageFixtures.Architecture, Signer));

        string mainMsix = PackageFixtures.CreateMainMsix(directory, Signer, declareDependency: true);

        MsixpkgFormatException ex = Assert.Throws<MsixpkgFormatException>(
            () => MsixpkgPacker.Pack(Request(directory, mainMsix, dependencyDirectory)));
        Assert.Equal(MsixpkgErrorCodes.AppxInvalid, ex.Code);
        Assert.Contains(PackageFixtures.DependencyName, ex.Detail);
    }

    [Fact]
    public void PackWithoutDependenciesDeclaredNeedsNoDirectory()
    {
        string directory = PackageFixtures.NewTempDirectory();
        string mainMsix = PackageFixtures.CreateMainMsix(directory, Signer, declareDependency: false);

        MsixpkgPackResult result = MsixpkgPacker.Pack(Request(directory, mainMsix));
        Assert.Empty(result.Manifest.Dependencies);
    }

    [Fact]
    public void PackRejectsWrongPassword()
    {
        string directory = PackageFixtures.NewTempDirectory();
        string mainMsix = PackageFixtures.CreateMainMsix(directory, Signer, declareDependency: false);

        MsixpkgFormatException ex = Assert.Throws<MsixpkgFormatException>(
            () => MsixpkgPacker.Pack(Request(directory, mainMsix, password: "not-the-password")));
        Assert.Equal(MsixpkgErrorCodes.CertificateMalformed, ex.Code);
    }

    [Fact]
    public void PackWithoutCertificateProducesAContainerWithoutPublisherEntry()
    {
        string directory = PackageFixtures.NewTempDirectory();
        string mainMsix = PackageFixtures.CreateMainMsix(directory, Signer, declareDependency: false);

        MsixpkgPackRequest request = Request(directory, mainMsix);
        request.IncludeCertificate = false;
        MsixpkgPackResult result = MsixpkgPacker.Pack(request);

        Assert.Null(result.Manifest.Publisher);
        using MsixpkgContainer container = MsixpkgContainer.Open(result.OutputPath);
        Assert.Null(container.Find(MsixpkgPaths.PublisherCertificateEntry));

        // 容器签名者指纹仍然指向签名证书，但没有可内嵌的证书时不该给出钉扎值。
        Assert.True(result.Warnings.Count >= 1);
    }

    [Fact]
    public void PackNormalizesThreePartAppxVersions()
    {
        string directory = PackageFixtures.NewTempDirectory();
        string mainMsix = Path.Combine(directory, "short-version.msix");
        byte[] bytes = SampleMsixFactory.Create(
            PackageFixtures.IdentityName,
            Signer.Subject,
            "1.2.3",
            PackageFixtures.Architecture,
            Signer);
        File.WriteAllBytes(mainMsix, bytes);

        // 写出的清单版本必须是四段；Appx 允许省略尾部 0。
        MsixpkgPackRequest request = Request(directory, mainMsix);
        MsixpkgPackResult result = MsixpkgPacker.Pack(request);
        Assert.Equal("1.2.3.0", result.Manifest.Identity.Version);
    }
}
