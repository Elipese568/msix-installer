using System;

namespace Msixpkg.Core.Tests;

public class ManifestTests
{
    private static MsixpkgManifest CreateSampleManifest()
    {
        var identity = new MsixpkgIdentity("Contoso.App", "CN=Contoso, O=Contoso", "1.2.3.4", "x64");
        var manifest = new MsixpkgManifest(identity, new MsixpkgFileRef(
            MsixpkgPaths.MainPackageEntry("x64", "Contoso.App", "1.2.3.4"),
            new string('a', 64),
            1024L))
        {
            CreatedUtc = "2026-01-01T00:00:00Z",
            MinOsBuild = 19041,
            InstallerMinVersion = "0.1.0",
            RequiresAdmin = true,
            AllowAllUsers = false,
            UninstallRemoveCertificate = true,
        };

        manifest.Dependencies.Add(new MsixpkgFileRef(
            MsixpkgPaths.DependencyEntry("x64", "Microsoft.VCLibs.140.00.UWPDesktop", "14.0.33728.0"),
            new string('b', 64),
            2048L,
            "Microsoft.VCLibs.140.00.UWPDesktop",
            "14.0.33728.0"));

        manifest.Publisher = new MsixpkgPublisherInfo(
            MsixpkgPaths.PublisherCertificateEntry,
            "CN=Contoso, O=Contoso",
            new string('c', 40),
            new string('d', 64),
            "2025-01-01T00:00:00Z",
            "2035-01-01T00:00:00Z",
            true);

        manifest.Validate();
        return manifest;
    }

    [Fact]
    public void RoundTripsThroughJson()
    {
        MsixpkgManifest original = CreateSampleManifest();
        MsixpkgManifest parsed = MsixpkgManifest.Parse(original.ToJson());

        Assert.Equal(original.FormatVersion, parsed.FormatVersion);
        Assert.Equal(original.CreatedUtc, parsed.CreatedUtc);
        Assert.Equal(original.Identity.Name, parsed.Identity.Name);
        Assert.Equal(original.Identity.Publisher, parsed.Identity.Publisher);
        Assert.Equal(original.Identity.Version, parsed.Identity.Version);
        Assert.Equal(original.Identity.Architecture, parsed.Identity.Architecture);
        Assert.Equal(original.MainPackage.Path, parsed.MainPackage.Path);
        Assert.Equal(original.MainPackage.Sha256, parsed.MainPackage.Sha256);
        Assert.Equal(original.MainPackage.Size, parsed.MainPackage.Size);
        Assert.Single(parsed.Dependencies);
        Assert.Equal(original.Dependencies[0].Name, parsed.Dependencies[0].Name);
        Assert.NotNull(parsed.Publisher);
        Assert.Equal(original.Publisher!.ThumbprintSha256, parsed.Publisher!.ThumbprintSha256);
        Assert.Equal(original.MinOsBuild, parsed.MinOsBuild);
        Assert.Equal(original.AllowAllUsers, parsed.AllowAllUsers);
        Assert.Equal(original.UninstallRemoveCertificate, parsed.UninstallRemoveCertificate);
    }

    [Fact]
    public void JsonIsStableForTheSameManifest()
    {
        MsixpkgManifest original = CreateSampleManifest();
        Assert.Equal(original.ToJson(), MsixpkgManifest.Parse(original.ToJson()).ToJson());
    }

    [Fact]
    public void RejectsUnknownMember()
    {
        string json = CreateSampleManifest().ToJson().Replace("\"minOsBuild\"", "\"minOsBuildV2\"");
        MsixpkgFormatException ex = Assert.Throws<MsixpkgFormatException>(() => MsixpkgManifest.Parse(json));
        Assert.Equal(MsixpkgErrorCodes.ManifestSchema, ex.Code);
    }

    [Fact]
    public void RejectsMissingMember()
    {
        string json = CreateSampleManifest().ToJson();
        int index = json.IndexOf("\"installerMinVersion\"", StringComparison.Ordinal);
        int end = json.IndexOf('\n', index);
        json = json.Remove(index, end - index + 1);

        MsixpkgFormatException ex = Assert.Throws<MsixpkgFormatException>(() => MsixpkgManifest.Parse(json));
        Assert.Equal(MsixpkgErrorCodes.ManifestSchema, ex.Code);
    }

    [Fact]
    public void RejectsMalformedJson()
    {
        MsixpkgFormatException ex = Assert.Throws<MsixpkgFormatException>(() => MsixpkgManifest.Parse("{ not json"));
        Assert.Equal(MsixpkgErrorCodes.ManifestSchema, ex.Code);
    }

    [Fact]
    public void RejectsUnsupportedFormatVersion()
    {
        string json = CreateSampleManifest().ToJson().Replace("\"formatVersion\": 1", "\"formatVersion\": 2");
        MsixpkgFormatException ex = Assert.Throws<MsixpkgFormatException>(() => MsixpkgManifest.Parse(json));
        Assert.Equal(MsixpkgErrorCodes.FormatVersionUnsupported, ex.Code);
    }

    [Fact]
    public void RejectsUppercaseHex()
    {
        var identity = new MsixpkgIdentity("Contoso.App", "CN=Contoso", "1.0.0.0", "x64");
        var manifest = new MsixpkgManifest(identity, new MsixpkgFileRef(
            MsixpkgPaths.MainPackageEntry("x64", "Contoso.App", "1.0.0.0"),
            "ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789",
            1L));

        MsixpkgFormatException ex = Assert.Throws<MsixpkgFormatException>(() => manifest.Validate());
        Assert.Equal(MsixpkgErrorCodes.ManifestSchema, ex.Code);
    }

    [Theory]
    [InlineData("1.0.0")]
    [InlineData("1.0.0.0.0")]
    [InlineData("1.0.0.x")]
    [InlineData("1.0.0.70000")]
    [InlineData("")]
    public void RejectsInvalidVersions(string version)
    {
        var identity = new MsixpkgIdentity("Contoso.App", "CN=Contoso", version, "x64");
        var manifest = new MsixpkgManifest(identity, new MsixpkgFileRef(
            MsixpkgPaths.MainPackageEntry("x64", "Contoso.App", "1.0.0.0"),
            new string('a', 64),
            1L));

        Assert.Throws<MsixpkgFormatException>(() => manifest.Validate());
    }

    [Fact]
    public void RejectsNonDependencyPathForDependency()
    {
        var identity = new MsixpkgIdentity("Contoso.App", "CN=Contoso", "1.0.0.0", "x64");
        var manifest = new MsixpkgManifest(identity, new MsixpkgFileRef(
            MsixpkgPaths.MainPackageEntry("x64", "Contoso.App", "1.0.0.0"),
            new string('a', 64),
            1L));

        manifest.Dependencies.Add(new MsixpkgFileRef(
            MsixpkgPaths.MainPackageEntry("x64", "Other.App", "1.0.0.0"),
            new string('b', 64),
            1L,
            "Other.App",
            "1.0.0.0"));

        Assert.Throws<MsixpkgFormatException>(() => manifest.Validate());
    }

    [Fact]
    public void RejectsUnknownArchitecture()
    {
        var identity = new MsixpkgIdentity("Contoso.App", "CN=Contoso", "1.0.0.0", "mips");
        var manifest = new MsixpkgManifest(identity, new MsixpkgFileRef(
            MsixpkgPaths.MainPackageEntry("mips", "Contoso.App", "1.0.0.0"),
            new string('a', 64),
            1L));

        Assert.Throws<MsixpkgFormatException>(() => manifest.Validate());
    }

    [Fact]
    public void RejectsBadTimestamp()
    {
        MsixpkgManifest manifest = CreateSampleManifest();
        manifest.CreatedUtc = "2026-01-01 00:00:00";
        Assert.Throws<MsixpkgFormatException>(() => manifest.Validate());
    }

    [Fact]
    public void RejectsWrongPublisherCertPath()
    {
        MsixpkgManifest original = CreateSampleManifest();
        string json = original.ToJson().Replace(
            "\"certPath\": \"publisher/publisher.cer\"",
            "\"certPath\": \"publisher/chain.p7b\"");
        Assert.Throws<MsixpkgFormatException>(() => MsixpkgManifest.Parse(json));
    }
}
