using System;
using System.Collections.Generic;
using System.IO;
using System.Security.Cryptography.X509Certificates;

namespace Msixpkg.Core.Tests;

/// <summary>
/// 黄金样本（tests/vectors 下）是所有实现共享的一致性基线：C# 打包器写出它，原生安装器读它。
/// 生成必须显式触发（MSIXPKG_GENERATE_VECTORS=1），普通测试运行绝不写入仓库。
/// </summary>
internal static class VectorGenerator
{
    public const string IdentityName = "Msixpkg.Sample";
    public const string Version = "1.0.0.0";
    public const string Architecture = "x64";
    public const string DependencyName = "Microsoft.VCLibs.140.00.UWPDesktop";
    public const string DependencyVersion = "14.0.33728.0";

    public static readonly DateTimeOffset CreatedUtc = new DateTimeOffset(2026, 1, 1, 0, 0, 0, TimeSpan.Zero);

    public static string ContainerFileName => IdentityName + "_" + Version + "_" + Architecture + ".msixpkg";

    public static string ContainerPath => Path.Combine(TestPaths.VectorsDirectory, ContainerFileName);

    public static string MainMsixPath => Path.Combine(TestPaths.VectorsDirectory, "sample-main.msix");

    public static string DependencyMsixPath => Path.Combine(TestPaths.VectorsDirectory, "sample-dependency.msix");

    public static bool GenerationRequested =>
        string.Equals(Environment.GetEnvironmentVariable("MSIXPKG_GENERATE_VECTORS"), "1", StringComparison.Ordinal);

    public static void EnsureGenerated()
    {
        if (GenerationRequested)
        {
            GenerateAll();
        }
    }

    public static void GenerateAll()
    {
        Directory.CreateDirectory(TestPaths.VectorsDirectory);
        X509Certificate2 signer = TestCertificates.LoadOrCreateTestCertificate();

        if (!File.Exists(MainMsixPath))
        {
            File.WriteAllBytes(MainMsixPath, SampleMsixFactory.Create(
                IdentityName,
                signer.Subject,
                Version,
                Architecture,
                signer,
                new List<(string, string)> { (DependencyName, DependencyVersion) }));
        }

        if (!File.Exists(DependencyMsixPath))
        {
            File.WriteAllBytes(DependencyMsixPath, SampleMsixFactory.Create(
                DependencyName,
                signer.Subject,
                DependencyVersion,
                Architecture,
                signer));
        }

        BuildFromCommittedInputs(ContainerPath);
    }

    /// <summary>用已提交的输入（证书 + 合成 .msix）重建容器，保证逐字节可复现。</summary>
    public static MsixpkgManifest BuildFromCommittedInputs(string outputPath)
    {
        if (!File.Exists(MainMsixPath))
        {
            throw new InvalidOperationException("Missing vector input: " + MainMsixPath + " (run with MSIXPKG_GENERATE_VECTORS=1)");
        }

        if (!File.Exists(DependencyMsixPath))
        {
            throw new InvalidOperationException("Missing vector input: " + DependencyMsixPath + " (run with MSIXPKG_GENERATE_VECTORS=1)");
        }

        X509Certificate2 signer = TestCertificates.LoadOrCreateTestCertificate();

        var builder = new MsixpkgPackageBuilder(new MsixpkgIdentity(
            IdentityName,
            signer.Subject,
            Version,
            Architecture));
        builder.SetMainPackage(MainMsixPath);
        builder.AddDependency(DependencyMsixPath, DependencyName, DependencyVersion);

        return builder.Write(outputPath, signer, new MsixpkgWriteOptions
        {
            CreatedUtc = CreatedUtc,
            MinOsBuild = 19041,
            InstallerMinVersion = "0.1.0",
            AllowAllUsers = true,
            UninstallRemoveCertificate = true,
            Display = new MsixpkgDisplayInfo("Msixpkg Sample", "Golden vector for the msixpkg v1 format.", "https://example.invalid/msixpkg", null),
        });
    }
}

public class VectorTests
{
    private static string BuildRegeneratedCopy()
    {
        string path = Path.Combine(PackageFixtures.NewTempDirectory(), VectorGenerator.ContainerFileName);
        VectorGenerator.BuildFromCommittedInputs(path);
        return path;
    }

    [Fact]
    public void GoldenVectorIsPresent()
    {
        VectorGenerator.EnsureGenerated();
        Assert.True(
            File.Exists(VectorGenerator.ContainerPath),
            "Missing golden vector. Generate it with MSIXPKG_GENERATE_VECTORS=1 dotnet test.");
    }

    [Fact]
    public void RegenerationIsByteIdenticalToCommittedVector()
    {
        VectorGenerator.EnsureGenerated();
        Assert.True(File.Exists(VectorGenerator.ContainerPath), "Missing golden vector.");

        string regenerated = BuildRegeneratedCopy();
        Assert.Equal(File.ReadAllBytes(VectorGenerator.ContainerPath), File.ReadAllBytes(regenerated));
    }

    [Fact]
    public void CommittedVectorValidatesAndRecordsTheContentDigest()
    {
        VectorGenerator.EnsureGenerated();
        Assert.True(File.Exists(VectorGenerator.ContainerPath), "Missing golden vector.");

        X509Certificate2 signer = TestCertificates.LoadOrCreateTestCertificate();
        MsixpkgValidationReport report = MsixpkgValidator.Validate(VectorGenerator.ContainerPath, TestValidation.Options(o =>
        {
            o.HostArchitecture = VectorGenerator.Architecture;
            o.AllowedPublisherThumbprints = new[] { Thumbprints.Sha256Hex(signer) };
        }));

        Assert.True(report.IsValid, string.Join("; ", report.Errors));
        Assert.Equal(VectorGenerator.IdentityName, report.Manifest!.Identity.Name);
        Assert.Equal(VectorGenerator.CreatedUtc, DateTimeOffset.Parse(report.Manifest.CreatedUtc, System.Globalization.CultureInfo.InvariantCulture).ToUniversalTime());
        Assert.Equal(Thumbprints.Sha256Hex(signer), Thumbprints.Sha256Hex(report.PublisherCertificate!));

        // 记录摘要值，便于原生侧比对同一份黄金样本。
        Assert.False(string.IsNullOrEmpty(report.ContentDigestHex));
    }

    [Fact]
    public void CommittedVectorUsesOnlyWhitelistedEntriesInSpecifiedOrder()
    {
        VectorGenerator.EnsureGenerated();
        Assert.True(File.Exists(VectorGenerator.ContainerPath), "Missing golden vector.");

        var names = ZipTestHelpers.EntryNames(VectorGenerator.ContainerPath);
        Assert.Equal(
            new[]
            {
                "msixpkg.json",
                "payload/x64/main/Msixpkg.Sample_1.0.0.0_x64.msix",
                "payload/x64/deps/Microsoft.VCLibs.140.00.UWPDesktop_14.0.33728.0_x64.msix",
                "publisher/publisher.cer",
                "msixpkg.sig",
            },
            names);
    }
}
