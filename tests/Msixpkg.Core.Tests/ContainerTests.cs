using System;
using System.IO;
using System.Linq;
using System.Security.Cryptography.X509Certificates;
using System.Text;

namespace Msixpkg.Core.Tests;

public class ContainerTests
{
    private static X509Certificate2 Signer => TestCertificates.LoadOrCreateTestCertificate();

    [Fact]
    public void WrittenContainerValidates()
    {
        PackageFixtures.BuiltPackage package = PackageFixtures.Build(Signer);
        MsixpkgValidationReport report = MsixpkgValidator.Validate(package.ContainerPath, TestValidation.Options(o =>
        {
            o.HostArchitecture = PackageFixtures.Architecture;
        }));

        Assert.True(report.IsValid, report.ToString());
        Assert.NotNull(report.Manifest);
        Assert.NotNull(report.PublisherCertificate);
        Assert.NotNull(report.ContentDigestHex);
    }

    [Fact]
    public void ContainerEntriesFollowTheSpecifiedOrder()
    {
        PackageFixtures.BuiltPackage package = PackageFixtures.Build(Signer);
        var names = ZipTestHelpers.EntryNames(package.ContainerPath).ToList();

        Assert.Equal(
            new[]
            {
                "msixpkg.json",
                PackageFixtures.MainEntryName,
                PackageFixtures.DependencyEntryName,
                "publisher/publisher.cer",
                "msixpkg.sig",
            },
            names);
    }

    [Fact]
    public void WriterIsByteReproducible()
    {
        string first = PackageFixtures.Build(Signer, PackageFixtures.NewTempDirectory(), "a.msixpkg").ContainerPath;
        string second = PackageFixtures.Build(Signer, PackageFixtures.NewTempDirectory(), "b.msixpkg").ContainerPath;

        Assert.Equal(File.ReadAllBytes(first), File.ReadAllBytes(second));
    }

    [Fact]
    public void DigestExcludesSignatureEntry()
    {
        PackageFixtures.BuiltPackage package = PackageFixtures.Build(Signer);
        using MsixpkgContainer container = MsixpkgContainer.Open(package.ContainerPath);

        string digest = MsixpkgContentDigest.ComputeHex(container.ComputeDigestEntries(excludeSignature: true));
        MsixpkgSignature signature = container.ReadSignature()!;

        Assert.Equal(digest, signature.ContentDigestHex);
    }

    [Fact]
    public void DigestIsOrderIndependent()
    {
        var entries = new[]
        {
            new MsixpkgContentDigestEntry("b", new string('b', 64)),
            new MsixpkgContentDigestEntry("a", new string('a', 64)),
        };
        var reversed = new[]
        {
            new MsixpkgContentDigestEntry("a", new string('a', 64)),
            new MsixpkgContentDigestEntry("b", new string('b', 64)),
        };

        Assert.Equal(MsixpkgContentDigest.ComputeHex(entries), MsixpkgContentDigest.ComputeHex(reversed));
    }

    [Fact]
    public void DeclaredHashesMatchEntryContent()
    {
        PackageFixtures.BuiltPackage package = PackageFixtures.Build(Signer);
        using MsixpkgContainer container = MsixpkgContainer.Open(package.ContainerPath);

        Assert.Equal(package.Manifest.MainPackage.Sha256, container.ComputeEntrySha256(package.Manifest.MainPackage.Path));
        Assert.Equal(package.Manifest.Dependencies[0].Sha256, container.ComputeEntrySha256(package.Manifest.Dependencies[0].Path));
    }

    [Fact]
    public void RejectsEmptyFile()
    {
        string path = Path.Combine(PackageFixtures.NewTempDirectory(), "empty.msixpkg");
        File.WriteAllBytes(path, Array.Empty<byte>());

        MsixpkgFormatException ex = Assert.Throws<MsixpkgFormatException>(() => MsixpkgContainer.Open(path));
        Assert.Equal(MsixpkgErrorCodes.ContainerEmpty, ex.Code);
    }

    [Fact]
    public void RejectsNonZipFile()
    {
        string path = Path.Combine(PackageFixtures.NewTempDirectory(), "notzip.msixpkg");
        File.WriteAllBytes(path, Encoding.UTF8.GetBytes("this is definitely not a zip archive, but it is long enough"));

        MsixpkgFormatException ex = Assert.Throws<MsixpkgFormatException>(() => MsixpkgContainer.Open(path));
        Assert.Equal(MsixpkgErrorCodes.ContainerInvalid, ex.Code);
    }

    [Fact]
    public void RejectsEntryCountAboveLimit()
    {
        PackageFixtures.BuiltPackage package = PackageFixtures.Build(Signer);
        var limits = new MsixpkgLimits { MaxEntryCount = 2 };

        MsixpkgFormatException ex = Assert.Throws<MsixpkgFormatException>(() => MsixpkgContainer.Open(package.ContainerPath, limits));
        Assert.Equal(MsixpkgErrorCodes.EntryCountExceeded, ex.Code);
    }

    [Fact]
    public void RejectsManifestThatIsNotFirst()
    {
        PackageFixtures.BuiltPackage package = PackageFixtures.Build(Signer);
        string reordered = Path.Combine(package.Directory, "reordered.msixpkg");
        ZipTestHelpers.RepackReordered(package.ContainerPath, PackageFixtures.MainEntryName, reordered);

        MsixpkgValidationReport report = MsixpkgValidator.Validate(reordered, TestValidation.Options(o => o.ValidateInnerAppx = false));
        Assert.False(report.IsValid);
        Assert.Contains(report.Errors, e => e.Code == MsixpkgErrorCodes.ManifestNotFirst);
    }

    [Fact]
    public void RejectsUnknownEntryName()
    {
        PackageFixtures.BuiltPackage package = PackageFixtures.Build(Signer);
        string extra = Path.Combine(package.Directory, "extra.msixpkg");
        ZipTestHelpers.RepackWithExtraEntry(
            package.ContainerPath,
            "docs/readme.txt",
            Encoding.UTF8.GetBytes("hello"),
            extra);

        MsixpkgFormatException ex = Assert.Throws<MsixpkgFormatException>(() => MsixpkgContainer.Open(extra));
        Assert.Equal(MsixpkgErrorCodes.EntryUnknown, ex.Code);
    }
}
