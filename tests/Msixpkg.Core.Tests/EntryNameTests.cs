using System.Collections.Generic;

namespace Msixpkg.Core.Tests;

public class EntryNameTests
{
    [Theory]
    [InlineData("msixpkg.json")]
    [InlineData("payload/x64/main/App_1.0.0.0_x64.msix")]
    [InlineData("payload/arm64/deps/Microsoft.VCLibs.140.00.UWPDesktop_14.0.33728.0_arm64.msix")]
    [InlineData("publisher/publisher.cer")]
    [InlineData("publisher/chain.p7b")]
    [InlineData("msixpkg.sig")]
    public void AcceptsWhitelistedNames(string name)
    {
        MsixpkgPaths.ValidateEntryName(name);
        MsixpkgPaths.Classify(name);
    }

    [Theory]
    [InlineData("../evil.msix")]
    [InlineData("payload/../../evil.msix")]
    [InlineData("/absolute.msix")]
    [InlineData("C:/drive.msix")]
    [InlineData("payload\\x64\\main\\App.msix")]
    [InlineData("payload/x64/main/App.msix:ads")]
    [InlineData("payload/x64/main/App.msix.")]
    [InlineData("payload//main/App.msix")]
    [InlineData("payload/x64/main/")]
    [InlineData("")]
    public void RejectsUnsafeNames(string name)
    {
        Assert.Throws<MsixpkgFormatException>(() => MsixpkgPaths.ValidateEntryName(name));
    }

    [Theory]
    [InlineData("payload/x64/other/App.msix")]
    [InlineData("payload/x64/main/App.zip")]
    [InlineData("payload/x64/main/nested/App.msix")]
    [InlineData("payload/x64/App.msix")]
    [InlineData("random/file.txt")]
    public void RejectsEntriesOutsideWhitelist(string name)
    {
        MsixpkgFormatException ex = Assert.Throws<MsixpkgFormatException>(() => MsixpkgPaths.Classify(name));
        Assert.Equal(MsixpkgErrorCodes.EntryUnknown, ex.Code);
    }

    [Fact]
    public void ReservedNameCaseVariantIsRejected()
    {
        MsixpkgFormatException ex = Assert.Throws<MsixpkgFormatException>(() => MsixpkgPaths.Classify("publisher/PUBLISHER.CER"));
        Assert.Equal(MsixpkgErrorCodes.EntryUnknown, ex.Code);
    }

    [Theory]
    [InlineData("x64")]
    [InlineData("x86")]
    [InlineData("arm64")]
    [InlineData("neutral")]
    public void KnownArchitecturesAreAccepted(string architecture)
    {
        Assert.True(MsixpkgPaths.IsKnownArchitecture(architecture));
    }

    [Fact]
    public void BuiltNamesClassifyToExpectedKinds()
    {
        Assert.Equal(
            MsixpkgEntryKind.MainPackage,
            MsixpkgPaths.Classify(MsixpkgPaths.MainPackageEntry("x64", "Contoso.App", "1.0.0.0")));

        Assert.Equal(
            MsixpkgEntryKind.Dependency,
            MsixpkgPaths.Classify(MsixpkgPaths.DependencyEntry("arm64", "Microsoft.WindowsAppRuntime.1.7", "7000.1.0.0")));

        Assert.Equal("payload/x64/main/Contoso.App_1.0.0.0_x64.msix", MsixpkgPaths.MainPackageEntry("x64", "Contoso.App", "1.0.0.0"));
    }

    [Fact]
    public void NamingHelpersRejectPathSeparators()
    {
        Assert.Throws<System.ArgumentException>(() => MsixpkgPaths.MainPackageEntry("x64", "Contoso/App", "1.0.0.0"));
        Assert.Throws<System.ArgumentException>(() => MsixpkgPaths.DependencyEntry("x64", "Name", "1.0.0.0\\x"));
    }

    [Fact]
    public void WhitelistCoversEveryKind()
    {
        var expected = new Dictionary<string, MsixpkgEntryKind>
        {
            ["msixpkg.json"] = MsixpkgEntryKind.Manifest,
            ["msixpkg.sig"] = MsixpkgEntryKind.Signature,
            ["payload/x64/main/App_1.0.0.0_x64.msix"] = MsixpkgEntryKind.MainPackage,
            ["payload/x64/deps/Dep_1.0.0.0_x64.msix"] = MsixpkgEntryKind.Dependency,
            ["publisher/publisher.cer"] = MsixpkgEntryKind.PublisherCertificate,
            ["publisher/chain.p7b"] = MsixpkgEntryKind.PublisherChain,
        };

        foreach (KeyValuePair<string, MsixpkgEntryKind> pair in expected)
        {
            Assert.Equal(pair.Value, MsixpkgPaths.Classify(pair.Key));
        }
    }
}
