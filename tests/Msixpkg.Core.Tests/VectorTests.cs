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

    // ---- 负例向量（T7）：每个只带一个缺陷，方便两端断言「恰好因为这一条被拒」 ----

    /// <summary>未签名容器：有 msixpkg.json 与 publisher.cer，但没有 msixpkg.sig。</summary>
    public const string UnsignedContainerFileName = "negative-unsigned.msixpkg";

    /// <summary>证书已过期：签名与身份都对，只有有效期落在过去。</summary>
    public const string ExpiredContainerFileName = "negative-expired.msixpkg";

    /// <summary>错误的 EKU：证书只有 serverAuth，没有 codeSigning。</summary>
    public const string WrongEkuContainerFileName = "negative-wrong-eku.msixpkg";

    public static string UnsignedContainerPath => Path.Combine(TestPaths.VectorsDirectory, UnsignedContainerFileName);

    public static string ExpiredContainerPath => Path.Combine(TestPaths.VectorsDirectory, ExpiredContainerFileName);

    public static string WrongEkuContainerPath => Path.Combine(TestPaths.VectorsDirectory, WrongEkuContainerFileName);

    /// <summary>负例向量的证书 DN 与黄金样本相同：这样「发布者不一致」不会混进失败原因。</summary>
    public static string NegativeInputsDirectory => Path.Combine(TestPaths.ArtifactsDirectory, "negative-inputs");

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
        GenerateNegativeVectors();
    }

    /// <summary>
    /// 生成三个负例容器。每个都用「与该容器证书配套的」内层 .msix 合成，
    /// 这样失败原因只有一个：缺签名 / 证书过期 / EKU 不对。
    /// 否则内层 Appx 的发布者或签名者指纹会对不上，断言就分不清到底是被哪条规则拦下的。
    /// </summary>
    public static void GenerateNegativeVectors()
    {
        Directory.CreateDirectory(TestPaths.VectorsDirectory);

        // 1) 未签名：证书与内层 .msix 都用提交的测试证书，只把签名关掉。
        X509Certificate2 signer = TestCertificates.LoadOrCreateTestCertificate();
        BuildNegativeContainer(UnsignedContainerPath, signer, sign: false, "unsigned");

        // 2) 已过期：notBefore 放在 30 天前、有效期 1 天 —— 整段有效期都在过去。
        using (X509Certificate2 expired = TestCertificates.CreateEphemeral(
            TestCertificates.PublisherDn,
            TimeSpan.FromDays(1),
            codeSigningEku: true,
            notBefore: DateTimeOffset.UtcNow.AddDays(-30)))
        {
            BuildNegativeContainer(ExpiredContainerPath, expired, sign: true, "expired");
        }

        // 3) 错误 EKU：只有 serverAuth。
        using (X509Certificate2 wrongEku = TestCertificates.CreateEphemeral(
            TestCertificates.PublisherDn,
            TimeSpan.FromDays(3650),
            codeSigningEku: false))
        {
            BuildNegativeContainer(WrongEkuContainerPath, wrongEku, sign: true, "wrong-eku");
        }
    }

    private static void BuildNegativeContainer(string outputPath, X509Certificate2 signer, bool sign, string label)
    {
        string workspace = Path.Combine(NegativeInputsDirectory, label);
        Directory.CreateDirectory(workspace);

        string main = Path.Combine(workspace, "main.msix");
        string dependency = Path.Combine(workspace, "dependency.msix");

        // 中间产物落在工作区内：受限执行环境里 %TEMP% 可能不可写。
        File.WriteAllBytes(main, SampleMsixFactory.Create(
            IdentityName,
            signer.Subject,
            Version,
            Architecture,
            signer,
            new List<(string, string)> { (DependencyName, DependencyVersion) }));

        File.WriteAllBytes(dependency, SampleMsixFactory.Create(
            DependencyName,
            signer.Subject,
            DependencyVersion,
            Architecture,
            signer));

        var builder = new MsixpkgPackageBuilder(new MsixpkgIdentity(
            IdentityName,
            signer.Subject,
            Version,
            Architecture));
        builder.SetMainPackage(main);
        builder.AddDependency(dependency, DependencyName, DependencyVersion);

        builder.Write(outputPath, signer, new MsixpkgWriteOptions
        {
            CreatedUtc = CreatedUtc,
            MinOsBuild = 19041,
            InstallerMinVersion = "0.1.0",
            AllowAllUsers = true,
            UninstallRemoveCertificate = true,
            // 证书必须内嵌：负例要验的是「证书本身不合格」，不是「没带证书」。
            IncludePublisherCertificate = true,
            Sign = sign,
            Display = new MsixpkgDisplayInfo("Msixpkg Negative Vector", "Single-defect negative vector for the native test suite.", null, null),
        });
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

    // ---- T7：负例向量 ----

    private static MsixpkgValidationReport ValidateNegative(string path, X509Certificate2 signer)
    {
        return MsixpkgValidator.Validate(path, TestValidation.Options(o =>
        {
            o.HostArchitecture = VectorGenerator.Architecture;
            o.AllowedPublisherThumbprints = new[] { Thumbprints.Sha256Hex(signer) };
        }));
    }

    [Fact]
    public void UnsignedVectorFailsOnlyBecauseTheSignatureIsMissing()
    {
        VectorGenerator.EnsureGenerated();
        X509Certificate2 signer = TestCertificates.LoadOrCreateTestCertificate();

        MsixpkgValidationReport report = ValidateNegative(VectorGenerator.UnsignedContainerPath, signer);
        Assert.False(report.IsValid);
        Assert.Contains(report.Errors, e => e.Code == MsixpkgErrorCodes.SignatureMissing);

        // 除了缺签名，别的地方都必须是好的（否则这条负例就没法定位问题）。
        Assert.DoesNotContain(report.Errors, e => e.Code.StartsWith("E_CERTIFICATE_", StringComparison.Ordinal));
        Assert.DoesNotContain(report.Errors, e => e.Code.StartsWith("E_APPX_", StringComparison.Ordinal));
    }

    [Fact]
    public void ExpiredVectorFailsOnlyBecauseTheCertificateExpired()
    {
        VectorGenerator.EnsureGenerated();
        using X509Certificate2 expired = TestCertificates.CreateEphemeral(
            TestCertificates.PublisherDn,
            TimeSpan.FromDays(1),
            codeSigningEku: true,
            notBefore: DateTimeOffset.UtcNow.AddDays(-30));

        MsixpkgValidationReport report = ValidateNegative(VectorGenerator.ExpiredContainerPath, expired);
        Assert.False(report.IsValid);
        Assert.Contains(report.Errors, e => e.Code == MsixpkgErrorCodes.CertificateExpired);
        Assert.DoesNotContain(report.Errors, e => e.Code == MsixpkgErrorCodes.CertificateNotYetValid);
        Assert.DoesNotContain(report.Errors, e => e.Code.StartsWith("E_APPX_", StringComparison.Ordinal));
        Assert.DoesNotContain(report.Errors, e => e.Code == MsixpkgErrorCodes.SignatureInvalid);
    }

    [Fact]
    public void WrongEkuVectorFailsOnlyBecauseTheCertificateIsNotForCodeSigning()
    {
        VectorGenerator.EnsureGenerated();
        using X509Certificate2 wrongEku = TestCertificates.CreateEphemeral(
            TestCertificates.PublisherDn,
            TimeSpan.FromDays(3650),
            codeSigningEku: false);

        MsixpkgValidationReport report = ValidateNegative(VectorGenerator.WrongEkuContainerPath, wrongEku);
        Assert.False(report.IsValid);
        Assert.Contains(report.Errors, e => e.Code == MsixpkgErrorCodes.CertificateNotCodeSigning);
        Assert.DoesNotContain(report.Errors, e => e.Code.StartsWith("E_APPX_", StringComparison.Ordinal));
        Assert.DoesNotContain(report.Errors, e => e.Code == MsixpkgErrorCodes.CertificateExpired);
    }
}
