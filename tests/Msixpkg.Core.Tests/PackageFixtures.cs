using System;
using System.Collections.Generic;
using System.IO;
using System.Security.Cryptography.X509Certificates;

namespace Msixpkg.Core.Tests;

/// <summary>构造一次性（非黄金）容器：每次都在临时目录里重新合成 .msix。</summary>
internal static class PackageFixtures
{
    public const string IdentityName = "Msixpkg.Sample";
    public const string Version = "1.0.0.0";
    public const string Architecture = "x64";
    public const string DependencyName = "Microsoft.VCLibs.140.00.UWPDesktop";
    public const string DependencyVersion = "14.0.33728.0";
    public const string DependencyEntryName = "payload/x64/deps/Microsoft.VCLibs.140.00.UWPDesktop_14.0.33728.0_x64.msix";
    public const string MainEntryName = "payload/x64/main/Msixpkg.Sample_1.0.0.0_x64.msix";

    public static readonly DateTimeOffset FixedCreatedUtc = new DateTimeOffset(2026, 1, 1, 0, 0, 0, TimeSpan.Zero);

    public static string NewTempDirectory()
    {
        string path = Path.Combine(TestPaths.ArtifactsDirectory, "tmp", Guid.NewGuid().ToString("n"));
        Directory.CreateDirectory(path);
        return path;
    }

    public static string CreateMainMsix(string directory, X509Certificate2 signer, string? identityName = null, string? publisher = null, string? architecture = null, bool declareDependency = true)
    {
        string path = Path.Combine(directory, "sample-main.msix");
        var dependencies = declareDependency
            ? new List<(string, string)> { (DependencyName, DependencyVersion) }
            : new List<(string, string)>();
        byte[] bytes = SampleMsixFactory.Create(
            identityName ?? IdentityName,
            publisher ?? signer.Subject,
            Version,
            architecture ?? Architecture,
            signer,
            dependencies);
        File.WriteAllBytes(path, bytes);
        return path;
    }

    public static string CreateDependencyMsix(string directory, X509Certificate2 signer)
    {
        string path = Path.Combine(directory, "sample-dependency.msix");
        File.WriteAllBytes(path, SampleMsixFactory.Create(DependencyName, signer.Subject, DependencyVersion, Architecture, signer));
        return path;
    }

    public sealed class BuiltPackage
    {
        public required string ContainerPath { get; init; }

        public required MsixpkgManifest Manifest { get; init; }

        public required string MainMsixPath { get; init; }

        public required string DependencyMsixPath { get; init; }

        public required string Directory { get; init; }
    }

    public static BuiltPackage Build(
        X509Certificate2 signer,
        string? directory = null,
        string? containerName = "sample.msixpkg",
        string? identityName = null,
        string? identityPublisher = null,
        string? architecture = null,
        bool includeDependency = true,
        bool includeCertificate = true,
        bool sign = true,
        DateTimeOffset? createdUtc = null)
    {
        directory ??= NewTempDirectory();
        string architectureValue = architecture ?? Architecture;
        string mainMsix = CreateMainMsix(directory, signer, identityName, identityPublisher, architectureValue, includeDependency);

        var builder = new MsixpkgPackageBuilder(new MsixpkgIdentity(
            identityName ?? IdentityName,
            identityPublisher ?? signer.Subject,
            Version,
            architectureValue));
        builder.SetMainPackage(mainMsix);

        string dependencyMsix = string.Empty;
        if (includeDependency)
        {
            dependencyMsix = CreateDependencyMsix(directory, signer);
            builder.AddDependency(dependencyMsix, DependencyName, DependencyVersion);
        }

        string containerPath = Path.Combine(directory, containerName!);
        var options = new MsixpkgWriteOptions
        {
            IncludePublisherCertificate = includeCertificate,
            Sign = sign,
            CreatedUtc = createdUtc ?? FixedCreatedUtc,
        };

        MsixpkgManifest manifest = builder.Write(containerPath, signer, options);
        return new BuiltPackage
        {
            ContainerPath = containerPath,
            Manifest = manifest,
            MainMsixPath = mainMsix,
            DependencyMsixPath = dependencyMsix,
            Directory = directory,
        };
    }
}
