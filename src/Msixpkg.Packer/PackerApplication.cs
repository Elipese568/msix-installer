using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text;
using Msixpkg.Core;
using Msixpkg.Core.Packing;

namespace Msixpkg.Packer;

internal sealed class PackerApplication
{
    public const string Version = "0.1.0";

    private readonly TextWriter _out;
    private readonly TextWriter _error;

    public PackerApplication(TextWriter output, TextWriter error)
    {
        _out = output;
        _error = error;
    }

    public int Run(string[] args)
    {
        try
        {
            if (args.Length == 0 || IsHelp(args[0]))
            {
                PrintUsage();
                return args.Length == 0 ? ExitCodes.Usage : ExitCodes.Success;
            }

            if (IsVersion(args[0]))
            {
                _out.WriteLine("msixpkg " + Version);
                return ExitCodes.Success;
            }

            string command = args[0];
            string[] rest = args.Skip(1).ToArray();

            switch (command)
            {
                case "pack":
                    return Pack(rest);
                case "verify":
                    return Verify(rest);
                case "inspect":
                    return Inspect(rest);
                case "extract":
                    return Extract(rest);
                case "export-cert":
                    return ExportCert(rest);
                case "dependencies":
                    return ListDependencies(rest);
                default:
                    _error.WriteLine("Unknown command: " + command);
                    PrintUsage();
                    return ExitCodes.Usage;
            }
        }
        catch (UsageException ex)
        {
            _error.WriteLine("usage: " + ex.Message);
            return ExitCodes.Usage;
        }
        catch (MsixpkgFormatException ex)
        {
            _error.WriteLine(ex.Code + ": " + ex.Message + (ex.Detail == null ? string.Empty : " [" + ex.Detail + "]"));
            return ex.Code == MsixpkgErrorCodes.IoError ? ExitCodes.IoError : ExitCodes.FormatError;
        }
        catch (IOException ex)
        {
            _error.WriteLine(MsixpkgErrorCodes.IoError + ": " + ex.Message);
            return ExitCodes.IoError;
        }
        catch (UnauthorizedAccessException ex)
        {
            _error.WriteLine(MsixpkgErrorCodes.IoError + ": " + ex.Message);
            return ExitCodes.IoError;
        }
        catch (Exception ex)
        {
            // 任何未预期的异常都必须变成可读的一行 + 稳定退出码，而不是让进程崩掉（0xE0434352）。
            _error.WriteLine("E_UNEXPECTED: " + ex.GetType().Name + ": " + ex.Message);
            _error.WriteLine(ex.ToString());
            return ExitCodes.Unexpected;
        }
    }

    private int Pack(string[] args)
    {
        CommandLine line = CommandLine.Parse(
            args,
            new[] { "json", "no-sign", "no-cert", "include-cert", "quiet", "allow-nondeterministic", "no-all-users" });
        string msix = line.Require("msix");
        string pfx = line.Require("pfx");
        string output = line.Get("output")
            ?? Path.Combine(
                Path.GetDirectoryName(Path.GetFullPath(msix)) ?? ".",
                Path.GetFileNameWithoutExtension(msix) + ".msixpkg");

        var request = new MsixpkgPackRequest
        {
            MsixPath = msix,
            PfxPath = pfx,
            PfxPassword = ResolvePassword(line),
            OutputPath = output,
            DependencyDirectory = line.Get("dependencies"),
            CertificateOutputPath = line.Get("cert-output"),
            MinOsBuild = line.GetInt("min-os-build", 19041),
            InstallerMinVersion = line.Get("installer-min-version") ?? Version,
            Sign = !line.Has("no-sign"),
            IncludeCertificate = line.Has("no-cert") ? false : (line.Has("include-cert") ? true : (bool?)null),
            AllowAllUsers = !line.Has("no-all-users"),
            AllowNonDeterministicSignature = line.Has("allow-nondeterministic"),
            CreatedUtc = line.Get("created-utc") == null
                ? null
                : DateTimeOffset.Parse(line.Get("created-utc")!, CultureInfo.InvariantCulture, DateTimeStyles.AssumeUniversal | DateTimeStyles.AdjustToUniversal),
            DisplayName = line.Get("display-name"),
            DisplayDescription = line.Get("display-description"),
            DisplayHomepage = line.Get("display-homepage"),
        };

        MsixpkgPackResult result = MsixpkgPacker.Pack(request);

        if (line.Has("json"))
        {
            WritePackJson(result, output);
        }
        else if (!line.Has("quiet"))
        {
            _out.WriteLine("packed  " + result.OutputPath);
            _out.WriteLine("cert    " + result.CertificatePath);
            _out.WriteLine("identity " + result.Manifest.Identity.Name + " " + result.Manifest.Identity.Version + " " + result.Manifest.Identity.Architecture);
            _out.WriteLine("publisher " + result.Manifest.Identity.Publisher);
            _out.WriteLine("deps    " + result.Manifest.Dependencies.Count.ToString(CultureInfo.InvariantCulture));
            foreach (string warning in result.Warnings)
            {
                _error.WriteLine("warning: " + warning);
            }
        }

        return ExitCodes.Success;
    }

    private void WritePackJson(MsixpkgPackResult result, string output)
    {
        var builder = new StringBuilder();
        builder.Append("{\n");
        builder.Append("  \"output\": ").Append(JsonText.Quote(result.OutputPath)).Append(",\n");
        builder.Append("  \"certificate\": ").Append(JsonText.Quote(result.CertificatePath)).Append(",\n");
        builder.Append("  \"identity\": ").Append(JsonText.Quote(result.Manifest.Identity.Name)).Append(",\n");
        builder.Append("  \"version\": ").Append(JsonText.Quote(result.Manifest.Identity.Version)).Append(",\n");
        builder.Append("  \"architecture\": ").Append(JsonText.Quote(result.Manifest.Identity.Architecture)).Append(",\n");
        builder.Append("  \"publisher\": ").Append(JsonText.Quote(result.Manifest.Identity.Publisher)).Append(",\n");
        builder.Append("  \"publisherThumbprintSha256\": ")
            .Append(JsonText.Quote(result.Manifest.Publisher?.ThumbprintSha256 ?? string.Empty)).Append(",\n");
        builder.Append("  \"size\": ").Append(new FileInfo(output).Length.ToString(CultureInfo.InvariantCulture)).Append(",\n");
        builder.Append("  \"warnings\": [");
        for (int i = 0; i < result.Warnings.Count; i++)
        {
            builder.Append(i == 0 ? "\n    " : ",\n    ").Append(JsonText.Quote(result.Warnings[i]));
        }

        builder.Append(result.Warnings.Count == 0 ? "]\n" : "\n  ]\n");
        builder.Append("}\n");
        _out.Write(builder.ToString());
    }

    private int Verify(string[] args)
    {
        CommandLine line = CommandLine.Parse(
            args,
            new[] { "json", "quiet", "require-pin", "allow-unsigned", "no-inner-appx" },
            new[] { "pin" });
        if (line.Positional.Count != 1)
        {
            throw new UsageException("verify requires exactly one positional argument: <container.msixpkg>");
        }

        string container = line.Positional[0];
        var pins = new List<string>();
        foreach (string pin in line.GetAll("pin"))
        {
            pins.Add(pin.Replace(" ", string.Empty).ToLowerInvariant());
        }

        var options = new MsixpkgValidateOptions
        {
            InstallerVersion = line.Get("installer-version") ?? Version,
            HostBuildNumber = line.Has("host-build") ? line.GetInt("host-build", 0) : (int?)null,
            HostArchitecture = line.Get("host-arch"),
            AllowedPublisherThumbprints = pins.Count > 0 ? pins : null,
            RequirePinnedPublisher = line.Has("require-pin"),
            RequireSignature = !line.Has("allow-unsigned"),
            ValidateInnerAppx = !line.Has("no-inner-appx"),
            TempDirectory = ResolveTempDirectory(line),
        };

        MsixpkgValidationReport report = MsixpkgValidator.Validate(container, options);

        if (line.Has("json"))
        {
            WriteVerifyJson(container, report);
        }
        else if (!line.Has("quiet"))
        {
            if (report.Manifest != null)
            {
                _out.WriteLine("container " + Path.GetFullPath(container));
                _out.WriteLine("identity  " + report.Manifest.Identity.Name + " " + report.Manifest.Identity.Version + " " + report.Manifest.Identity.Architecture);
                if (report.ContentDigestHex != null)
                {
                    _out.WriteLine("digest    " + report.ContentDigestHex);
                }
            }

            foreach (string warning in report.Warnings.Select(w => w.ToString()))
            {
                _out.WriteLine("warning   " + warning);
            }

            foreach (string error in report.Errors.Select(e => e.ToString()))
            {
                _out.WriteLine("error     " + error);
            }

            _out.WriteLine(report.IsValid ? "result    valid" : "result    INVALID");
        }

        return report.IsValid ? ExitCodes.Success : ExitCodes.FormatError;
    }

    private void WriteVerifyJson(string container, MsixpkgValidationReport report)
    {
        var builder = new StringBuilder();
        builder.Append("{\n");
        builder.Append("  \"container\": ").Append(JsonText.Quote(Path.GetFullPath(container))).Append(",\n");
        builder.Append("  \"valid\": ").Append(report.IsValid ? "true" : "false").Append(",\n");
        builder.Append("  \"contentDigest\": ").Append(JsonText.Quote(report.ContentDigestHex ?? string.Empty)).Append(",\n");
        builder.Append("  \"publisherThumbprintSha256\": ")
            .Append(JsonText.Quote(report.PublisherCertificate == null ? string.Empty : Thumbprints.Sha256Hex(report.PublisherCertificate))).Append(",\n");
        builder.Append("  \"publisherPinned\": ").Append(report.PublisherCertificateWasPinned ? "true" : "false").Append(",\n");
        builder.Append("  \"errors\": [");
        for (int i = 0; i < report.Errors.Count; i++)
        {
            builder.Append(i == 0 ? "\n    " : ",\n    ").Append(JsonText.Quote(report.Errors[i].ToString()));
        }

        builder.Append(report.Errors.Count == 0 ? "],\n" : "\n  ],\n");
        builder.Append("  \"warnings\": [");
        for (int i = 0; i < report.Warnings.Count; i++)
        {
            builder.Append(i == 0 ? "\n    " : ",\n    ").Append(JsonText.Quote(report.Warnings[i].ToString()));
        }

        builder.Append(report.Warnings.Count == 0 ? "]\n" : "\n  ]\n");
        builder.Append("}\n");
        _out.Write(builder.ToString());
    }

    private int Inspect(string[] args)
    {
        CommandLine line = CommandLine.Parse(args, new[] { "json" });
        if (line.Positional.Count != 1)
        {
            throw new UsageException("inspect requires exactly one positional argument: <container.msixpkg>");
        }

        using MsixpkgContainer container = MsixpkgContainer.Open(line.Positional[0]);
        MsixpkgManifest manifest = container.ReadManifest();
        MsixpkgSignature? signature = container.ReadSignature();

        if (line.Has("json"))
        {
            var builder = new StringBuilder();
            builder.Append("{\n");
            builder.Append("  \"container\": ").Append(JsonText.Quote(Path.GetFullPath(container.Path))).Append(",\n");
            builder.Append("  \"manifest\": ").Append(manifest.ToJson().TrimEnd('\n')).Append(",\n");
            builder.Append("  \"signatureAlgorithm\": ").Append(JsonText.Quote(signature?.Algorithm ?? string.Empty)).Append(",\n");
            builder.Append("  \"entries\": [");
            for (int i = 0; i < container.Entries.Count; i++)
            {
                MsixpkgContainerEntry entry = container.Entries[i];
                builder.Append(i == 0 ? "\n    " : ",\n    ");
                builder.Append(JsonText.Quote(entry.Name + " (" + entry.UncompressedSize.ToString(CultureInfo.InvariantCulture) + " bytes)"));
            }

            builder.Append(container.Entries.Count == 0 ? "]\n" : "\n  ]\n");
            builder.Append("}\n");
            _out.Write(builder.ToString());
            return ExitCodes.Success;
        }

        _out.WriteLine("container   " + Path.GetFullPath(container.Path));
        _out.WriteLine("identity    " + manifest.Identity.Name + " " + manifest.Identity.Version + " " + manifest.Identity.Architecture);
        _out.WriteLine("publisher   " + manifest.Identity.Publisher);
        _out.WriteLine("createdUtc  " + manifest.CreatedUtc);
        _out.WriteLine("minOsBuild  " + manifest.MinOsBuild.ToString(CultureInfo.InvariantCulture));
        _out.WriteLine("deps        " + manifest.Dependencies.Count.ToString(CultureInfo.InvariantCulture));
        foreach (MsixpkgFileRef dependency in manifest.Dependencies)
        {
            _out.WriteLine("  - " + dependency.Name + " " + dependency.Version + " (" + dependency.Path + ")");
        }

        if (manifest.Publisher != null)
        {
            _out.WriteLine("cert        " + manifest.Publisher.Subject);
            _out.WriteLine("thumbprint  " + manifest.Publisher.ThumbprintSha256);
            _out.WriteLine("valid        " + manifest.Publisher.NotBeforeUtc + " .. " + manifest.Publisher.NotAfterUtc);
        }
        else
        {
            _out.WriteLine("cert        (none embedded)");
        }

        _out.WriteLine("signature   " + (signature == null ? "(unsigned)" : signature.Algorithm));
        _out.WriteLine("digest      " + MsixpkgContentDigest.ComputeHex(container.ComputeDigestEntries(excludeSignature: true)));
        _out.WriteLine("entries");
        foreach (MsixpkgContainerEntry entry in container.Entries)
        {
            _out.WriteLine("  " + entry.Kind.ToString().PadRight(20) + entry.Name
                + "  " + entry.UncompressedSize.ToString(CultureInfo.InvariantCulture) + " bytes");
        }

        return ExitCodes.Success;
    }

    private int Extract(string[] args)
    {
        CommandLine line = CommandLine.Parse(args, new[] { "json" });
        if (line.Positional.Count != 1)
        {
            throw new UsageException("extract requires exactly one positional argument: <container.msixpkg>");
        }

        string output = line.Require("output");
        Directory.CreateDirectory(output);

        using MsixpkgContainer container = MsixpkgContainer.Open(line.Positional[0]);
        var written = new List<string>();
        foreach (MsixpkgContainerEntry entry in container.Entries)
        {
            string target = Path.Combine(output, entry.Name.Replace('/', Path.DirectorySeparatorChar));
            container.ExtractEntryToFile(entry.Name, target);
            written.Add(target);
        }

        if (line.Has("json"))
        {
            var builder = new StringBuilder("{\n  \"files\": [");
            for (int i = 0; i < written.Count; i++)
            {
                builder.Append(i == 0 ? "\n    " : ",\n    ").Append(JsonText.Quote(written[i]));
            }

            builder.Append(written.Count == 0 ? "]\n}\n" : "\n  ]\n}\n");
            _out.Write(builder.ToString());
        }
        else
        {
            foreach (string file in written)
            {
                _out.WriteLine(file);
            }
        }

        return ExitCodes.Success;
    }

    private int ExportCert(string[] args)
    {
        CommandLine line = CommandLine.Parse(args, new[] { "json" });
        string pfx = line.Require("pfx");
        string output = line.Require("output");

        using var certificate = MsixpkgCertificates.LoadSigningCertificateFromFile(pfx, ResolvePassword(line));

        byte[] der = certificate.Export(System.Security.Cryptography.X509Certificates.X509ContentType.Cert);
        EnsureParentDirectory(output);
        File.WriteAllBytes(output, der);

        if (line.Has("json"))
        {
            _out.Write("{\n  \"output\": " + JsonText.Quote(Path.GetFullPath(output))
                + ",\n  \"subject\": " + JsonText.Quote(certificate.Subject)
                + ",\n  \"thumbprintSha256\": " + JsonText.Quote(Thumbprints.Sha256Hex(certificate))
                + "\n}\n");
        }
        else
        {
            _out.WriteLine("exported " + Path.GetFullPath(output));
            _out.WriteLine("subject  " + certificate.Subject);
            _out.WriteLine("thumbprint " + Thumbprints.Sha256Hex(certificate));
        }

        return ExitCodes.Success;
    }

    private int ListDependencies(string[] args)
    {
        CommandLine line = CommandLine.Parse(args, new[] { "json" });
        if (line.Positional.Count != 1)
        {
            throw new UsageException("dependencies requires exactly one positional argument: <directory>");
        }

        foreach (string description in MsixpkgPacker.DescribeDependencies(line.Positional[0]))
        {
            _out.WriteLine(description);
        }

        return ExitCodes.Success;
    }

    private static void EnsureParentDirectory(string filePath)    {
        string? directory = Path.GetDirectoryName(Path.GetFullPath(filePath));
        if (!string.IsNullOrEmpty(directory))
        {
            Directory.CreateDirectory(directory);
        }
    }

    /// <summary>
    /// 校验需要把内层 .msix 落到临时目录。系统临时目录在受约束的环境里可能不可写，
    /// 因此按「显式指定 → ProgramData → 系统临时目录 → 当前目录」顺序探测第一个可写位置。
    /// </summary>
    private static string ResolveTempDirectory(CommandLine line)
    {
        string? explicitTemp = line.Get("temp");
        if (!string.IsNullOrEmpty(explicitTemp))
        {
            return explicitTemp!;
        }

        foreach (string candidate in new[]
        {
            Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.CommonApplicationData), "Msixpkg", "temp"),
            Path.GetTempPath(),
            Path.Combine(Directory.GetCurrentDirectory(), "artifacts", "temp"),
        })
        {
            if (IsWritable(candidate))
            {
                return candidate;
            }
        }

        throw new MsixpkgFormatException(
            MsixpkgErrorCodes.IoError,
            "No writable temporary directory was found. Use --temp <dir> to specify one.");
    }

    private static bool IsWritable(string directory)
    {
        try
        {
            Directory.CreateDirectory(directory);
            string probe = Path.Combine(directory, "msixpkg-write-probe-" + Guid.NewGuid().ToString("n") + ".tmp");
            using (var stream = new FileStream(probe, FileMode.CreateNew, FileAccess.Write, FileShare.None))
            {
                stream.WriteByte(0);
            }

            File.Delete(probe);
            return true;
        }
        catch (IOException)
        {
            return false;
        }
        catch (UnauthorizedAccessException)
        {
            return false;
        }
    }

    private static string? ResolvePassword(CommandLine line)
    {
        string? environment = line.Get("password-env");
        if (!string.IsNullOrEmpty(environment))
        {
            return Environment.GetEnvironmentVariable(environment!);
        }

        string? file = line.Get("password-file");
        if (!string.IsNullOrEmpty(file))
        {
            return File.ReadAllText(file!).TrimEnd('\r', '\n');
        }

        return line.Get("password");
    }

    private static bool IsHelp(string value) => value is "-h" or "--help" or "help" or "/?";

    private static bool IsVersion(string value) => value is "-v" or "--version" or "version";

    private void PrintUsage()
    {
        _out.WriteLine("msixpkg " + Version + " — .msixpkg packer and verifier");
        _out.WriteLine();
        _out.WriteLine("  msixpkg pack --msix <App.msix> --pfx <App.pfx> [options]");
        _out.WriteLine("      --output <file.msixpkg>        默认与 .msix 同目录同名");
        _out.WriteLine("      --password | --password-env VAR | --password-file FILE");
        _out.WriteLine("      --dependencies <dir>           依赖包（框架包）搜索目录");
        _out.WriteLine("      --cert-output <file.cer>       导出公钥证书的路径");
        _out.WriteLine("      --min-os-build <n>             默认 19041");
        _out.WriteLine("      --installer-min-version <ver>  默认 " + Version);
        _out.WriteLine("      --created-utc <iso>            固定时间戳（可复现构建）");
        _out.WriteLine("      --no-cert | --include-cert     是否内嵌发布者证书（默认自签名内嵌）");
        _out.WriteLine("      --no-sign                      产出未签名容器（仅调试）");
        _out.WriteLine("      --display-name/--display-description/--display-homepage");
        _out.WriteLine("      --json");
        _out.WriteLine();
        _out.WriteLine("  msixpkg verify <file.msixpkg> [--pin <sha256>]... [--require-pin]");
        _out.WriteLine("      --host-arch <x64|x86|arm64>  --host-build <n>  --installer-version <ver>");
        _out.WriteLine("      --allow-unsigned  --no-inner-appx  --temp <dir>  --json  --quiet");
        _out.WriteLine();
        _out.WriteLine("  msixpkg inspect <file.msixpkg> [--json]");
        _out.WriteLine("  msixpkg extract <file.msixpkg> --output <dir> [--json]");
        _out.WriteLine("  msixpkg export-cert --pfx <file.pfx> --output <file.cer>");
        _out.WriteLine("  msixpkg dependencies <dir>");
        _out.WriteLine();
        _out.WriteLine("  退出码: 0 成功 / 2 格式或校验失败 / 3 用法错误 / 4 I/O 错误");
    }
}

internal static class ExitCodes
{
    public const int Success = 0;
    public const int Unexpected = 1;
    public const int FormatError = 2;
    public const int Usage = 3;
    public const int IoError = 4;
}

internal static class JsonText
{
    public static string Quote(string value)
    {
        if (value == null)
        {
            return "null";
        }

        var builder = new StringBuilder(value.Length + 2);
        builder.Append('"');
        foreach (char c in value)
        {
            switch (c)
            {
                case '"':
                    builder.Append("\\\"");
                    break;
                case '\\':
                    builder.Append("\\\\");
                    break;
                case '\n':
                    builder.Append("\\n");
                    break;
                case '\r':
                    builder.Append("\\r");
                    break;
                case '\t':
                    builder.Append("\\t");
                    break;
                case '\b':
                    builder.Append("\\b");
                    break;
                case '\f':
                    builder.Append("\\f");
                    break;
                default:
                    if (c < 0x20)
                    {
                        builder.Append("\\u").Append(((int)c).ToString("x4", CultureInfo.InvariantCulture));
                    }
                    else
                    {
                        builder.Append(c);
                    }

                    break;
            }
        }

        builder.Append('"');
        return builder.ToString();
    }
}
