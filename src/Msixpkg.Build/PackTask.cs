using System;
using System.Collections.Generic;
using System.Globalization;
using Microsoft.Build.Framework;
using Microsoft.Build.Utilities;
using Msixpkg.Core;
using Msixpkg.Core.Packing;

namespace Msixpkg.Build
{
    /// <summary>
    /// 在 Appx 打包完成后把「已签名的 .msix + 依赖 + 发布者证书」组装成 .msixpkg。
    /// 进程内调用 <see cref="MsixpkgPacker"/>，与 CLI 走完全相同的实现 ——
    /// 这保证「开发机手工打包」和「构建期自动打包」不会产出两种东西。
    ///
    /// MSBuild 任务的一条铁律：Execute 绝不向外抛异常，所有失败都通过 Log 上报并以 false 返回。
    /// </summary>
    public sealed class PackTask : Task
    {
        /// <summary>
        /// 候选输入 .msix。允许给多个（显式指定 / Appx 打包目标输出 / AppxPackageDir 兜底搜索），
        /// 由任务自行挑出真实存在的那个 —— 这种解析逻辑放在 C# 里才可测试、可给可操作的错误。
        /// </summary>
        public ITaskItem[]? InputMsix { get; set; }

        [Required]
        public ITaskItem? CertificatePfx { get; set; }

        public string? CertificatePassword { get; set; }

        public ITaskItem? DependencyDirectory { get; set; }

        public ITaskItem? OutputDirectory { get; set; }

        public string? OutputFileName { get; set; }

        public int MinOsBuild { get; set; } = 19041;

        public string? InstallerMinVersion { get; set; }

        public bool Sign { get; set; } = true;

        public bool IncludeCertificate { get; set; } = true;

        public bool AllowAllUsers { get; set; } = true;

        public string? DisplayName { get; set; }

        public string? DisplayDescription { get; set; }

        public string? DisplayHomepage { get; set; }

        /// <summary>
        /// 容器时间戳（ISO-8601，如 <c>2026-01-01T00:00:00Z</c>）。留空表示用当前时间；
        /// 显式设置后，同一份输入在任意机器、任意时刻都产出完全相同的字节（可复现构建）。
        /// </summary>
        public string? CreatedUtc { get; set; }

        [Output]
        public ITaskItem? OutputFile { get; set; }

        [Output]
        public ITaskItem? CertificateFile { get; set; }

        [Output]
        public ITaskItem? PublisherThumbprint { get; set; }

        [Output]
        public ITaskItem? PackageIdentity { get; set; }

        public override bool Execute()
        {
            if (CertificatePfx == null || string.IsNullOrEmpty(CertificatePfx.ItemSpec))
            {
                Log.LogError("Msixpkg: MsixpkgCertificatePfx is required (the same .pfx that signed the .msix).");
                return false;
            }

            string? inputMsix = ResolveInputMsix();
            if (inputMsix == null)
            {
                Log.LogError(
                    "Msixpkg: no input .msix was found. For a single-project MSIX this means the Appx packaging step "
                    + "did not run — build with -p:GenerateAppxPackageOnBuild=true and check AppxPackageSigningEnabled. "
                    + "Alternatively set MsixpkgInputMsix explicitly.");
                return false;
            }

            string pfxPath = CertificatePfx.GetMetadata("FullPath");

            if (!System.IO.File.Exists(pfxPath))
            {
                Log.LogError("Msixpkg: the signing .pfx does not exist.", null, null, null, 0, 0, 0, 0, pfxPath);
                return false;
            }

            // 固定时间戳（可选）：这里把「解析失败」变成一条能照做的错误，
            // 而不是让它以格式异常的形态在 Core 深处炸出来。
            DateTimeOffset? createdUtc = null;
            if (!string.IsNullOrEmpty(CreatedUtc))
            {
                if (!DateTimeOffset.TryParse(
                        CreatedUtc,
                        CultureInfo.InvariantCulture,
                        DateTimeStyles.AssumeUniversal | DateTimeStyles.AdjustToUniversal,
                        out DateTimeOffset parsedCreatedUtc))
                {
                    Log.LogError(
                        "Msixpkg: MsixpkgCreatedUtc is not a valid ISO-8601 timestamp: '{0}'. "
                        + "Expected something like 2026-01-01T00:00:00Z; leave it empty to use the current time.",
                        CreatedUtc);
                    return false;
                }

                createdUtc = parsedCreatedUtc;
            }

            string outputDirectory = OutputDirectory?.GetMetadata("FullPath") ?? string.Empty;
            if (string.IsNullOrEmpty(outputDirectory))
            {
                outputDirectory = System.IO.Path.GetDirectoryName(inputMsix);
            }

            string fileName = string.IsNullOrEmpty(OutputFileName)
                ? System.IO.Path.GetFileNameWithoutExtension(inputMsix) + ".msixpkg"
                : OutputFileName!;

            string outputPath = System.IO.Path.Combine(outputDirectory!, fileName);
            string dependencyDirectory = DependencyDirectory?.GetMetadata("FullPath") ?? string.Empty;

            var request = new MsixpkgPackRequest
            {
                MsixPath = inputMsix,
                PfxPath = pfxPath,
                PfxPassword = string.IsNullOrEmpty(CertificatePassword) ? null : CertificatePassword,
                OutputPath = outputPath,
                DependencyDirectory = string.IsNullOrEmpty(dependencyDirectory) ? null : dependencyDirectory,
                MinOsBuild = MinOsBuild,
                InstallerMinVersion = string.IsNullOrEmpty(InstallerMinVersion) ? "0.1.0" : InstallerMinVersion!,
                Sign = Sign,
                IncludeCertificate = IncludeCertificate,
                AllowAllUsers = AllowAllUsers,
                CreatedUtc = createdUtc,
                DisplayName = DisplayName,
                DisplayDescription = DisplayDescription,
                DisplayHomepage = DisplayHomepage,
            };

            try
            {
                Log.LogMessage(MessageImportance.Normal, "Msixpkg: packing {0}", inputMsix);
                MsixpkgPackResult result = MsixpkgPacker.Pack(request);

                foreach (string warning in result.Warnings)
                {
                    Log.LogWarning("Msixpkg: {0}", warning);
                }

                OutputFile = new TaskItem(result.OutputPath);
                CertificateFile = new TaskItem(result.CertificatePath);
                PublisherThumbprint = new TaskItem(result.Manifest.Publisher?.ThumbprintSha256 ?? string.Empty);
                PackageIdentity = new TaskItem(
                    result.Manifest.Identity.Name + " " + result.Manifest.Identity.Version + " " + result.Manifest.Identity.Architecture);

                Log.LogMessage(
                    MessageImportance.High,
                    "Msixpkg: {0} ({1} {2} {3})",
                    result.OutputPath,
                    result.Manifest.Identity.Name,
                    result.Manifest.Identity.Version,
                    result.Manifest.Identity.Architecture);

                return true;
            }
            catch (MsixpkgFormatException ex)
            {
                // 空密码是最常见的失败原因，单独给一条能直接照做的提示。
                if (ex.Code == MsixpkgErrorCodes.CertificateMalformed && string.IsNullOrEmpty(request.PfxPassword))
                {
                    Log.LogError(
                        "Msixpkg: no certificate password was supplied. Set MsixpkgCertificatePassword "
                        + "(or PackageCertificatePassword) — a protected .pfx cannot be opened without it.");
                }

                Log.LogError(null, ex.Code, null, inputMsix, 0, 0, 0, 0, "Msixpkg({0}): {1}{2}", ex.Code, ex.Message, FormatDetail(ex.Detail));
                return false;
            }
            catch (Exception ex)
            {
                // 任务内绝不外抛：MSBuild 会把异常变成难以理解的 MSB4018。
                Log.LogError("Msixpkg: unexpected failure while packing: {0}: {1}", ex.GetType().Name, ex.Message);
                Log.LogMessage(MessageImportance.Low, ex.ToString());
                return false;
            }
        }

        /// <summary>
        /// 从候选里挑出真实存在的 .msix：优先显式指定的 MsixpkgInputMsix，
        /// 其次 Appx 打包目标暴露的输出项，最后是 AppxPackageDir 下的兜底搜索结果（取最新写入的那个）。
        /// </summary>
        private string? ResolveInputMsix()
        {
            if (InputMsix == null || InputMsix.Length == 0)
            {
                return null;
            }

            string? newest = null;
            DateTime newestTimestamp = DateTime.MinValue;

            foreach (ITaskItem candidate in InputMsix)
            {
                if (candidate == null || string.IsNullOrEmpty(candidate.ItemSpec))
                {
                    continue;
                }

                string path = candidate.GetMetadata("FullPath") ?? candidate.ItemSpec;
                if (!System.IO.File.Exists(path))
                {
                    continue;
                }

                if (!string.Equals(System.IO.Path.GetExtension(path), ".msix", StringComparison.OrdinalIgnoreCase))
                {
                    continue;
                }

                DateTime timestamp = System.IO.File.GetLastWriteTimeUtc(path);
                if (newest == null || timestamp > newestTimestamp)
                {
                    newest = path;
                    newestTimestamp = timestamp;
                }
            }

            return newest;
        }

        private static string FormatDetail(string? detail)
        {
            return string.IsNullOrEmpty(detail) ? string.Empty : " [" + detail + "]";
        }
    }
}
