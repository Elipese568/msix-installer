using System;
using System.Collections.Generic;
using System.Globalization;
using Msixpkg.Core.Json;

namespace Msixpkg.Core
{
    public sealed class MsixpkgIdentity
    {
        public MsixpkgIdentity(string name, string publisher, string version, string architecture)
        {
            Name = name;
            Publisher = publisher;
            Version = version;
            Architecture = architecture;
        }

        /// <summary>Appx 清单 Identity/@Name，例如 "Contoso.AssetTracker"。</summary>
        public string Name { get; }

        /// <summary>Appx 清单 Identity/@Publisher，DN 原样（必须与证书 Subject 逐字符相等）。</summary>
        public string Publisher { get; }

        /// <summary>四段数字版本，例如 "1.0.0.0"。</summary>
        public string Version { get; }

        /// <summary>x64 / x86 / arm64 / neutral。</summary>
        public string Architecture { get; }
    }

    public sealed class MsixpkgFileRef
    {
        public MsixpkgFileRef(string path, string sha256, long size, string? name = null, string? version = null)
        {
            Path = path;
            Sha256 = sha256;
            Size = size;
            Name = name;
            Version = version;
        }

        public string Path { get; }

        /// <summary>小写十六进制 SHA-256（64 字符）。</summary>
        public string Sha256 { get; }

        public long Size { get; }

        /// <summary>仅依赖包使用：包标识名。</summary>
        public string? Name { get; }

        /// <summary>仅依赖包使用：包版本。</summary>
        public string? Version { get; }
    }

    public sealed class MsixpkgPublisherInfo
    {
        public MsixpkgPublisherInfo(
            string certPath,
            string subject,
            string thumbprintSha1,
            string thumbprintSha256,
            string notBeforeUtc,
            string notAfterUtc,
            bool selfSigned)
        {
            CertPath = certPath;
            Subject = subject;
            ThumbprintSha1 = thumbprintSha1;
            ThumbprintSha256 = thumbprintSha256;
            NotBeforeUtc = notBeforeUtc;
            NotAfterUtc = notAfterUtc;
            SelfSigned = selfSigned;
        }

        public string CertPath { get; }

        public string Subject { get; }

        /// <summary>小写十六进制 SHA-1（40 字符）。Windows 习惯用大写显示，存储统一小写。</summary>
        public string ThumbprintSha1 { get; }

        /// <summary>小写十六进制 SHA-256（64 字符）—— 钉扎用的就是这个值。</summary>
        public string ThumbprintSha256 { get; }

        public string NotBeforeUtc { get; }

        public string NotAfterUtc { get; }

        public bool SelfSigned { get; }
    }

    public sealed class MsixpkgDisplayInfo
    {
        public MsixpkgDisplayInfo(string? name, string? description, string? homepage, string? iconPngBase64)
        {
            Name = name;
            Description = description;
            Homepage = homepage;
            IconPngBase64 = iconPngBase64;
        }

        public string? Name { get; }

        public string? Description { get; }

        public string? Homepage { get; }

        public string? IconPngBase64 { get; }
    }

    /// <summary>msixpkg.json 的模型。字段稳定性属于公开契约。</summary>
    public sealed class MsixpkgManifest
    {
        public const int CurrentFormatVersion = 1;

        private static readonly string[] KnownMembers =
        {
            "formatVersion",
            "createdUtc",
            "identity",
            "mainPackage",
            "dependencies",
            "publisher",
            "minOsBuild",
            "installerMinVersion",
            "requiresAdmin",
            "allowAllUsers",
            "uninstallRemoveCertificate",
            "display",
        };

        public MsixpkgManifest(MsixpkgIdentity identity, MsixpkgFileRef mainPackage)
        {
            Identity = identity ?? throw new ArgumentNullException(nameof(identity));
            MainPackage = mainPackage ?? throw new ArgumentNullException(nameof(mainPackage));
            CreatedUtc = FormatIsoUtc(DateTime.UtcNow);
            InstallerMinVersion = "0.1.0";
            RequiresAdmin = true;
        }

        private MsixpkgManifest()
        {
            Identity = new MsixpkgIdentity(string.Empty, string.Empty, string.Empty, string.Empty);
            MainPackage = new MsixpkgFileRef(string.Empty, string.Empty, 0L);
        }

        public int FormatVersion { get; set; } = CurrentFormatVersion;

        public string CreatedUtc { get; set; } = string.Empty;

        public MsixpkgIdentity Identity { get; private set; }

        public MsixpkgFileRef MainPackage { get; private set; }

        public List<MsixpkgFileRef> Dependencies { get; } = new List<MsixpkgFileRef>();

        public MsixpkgPublisherInfo? Publisher { get; set; }

        public int MinOsBuild { get; set; } = 19041;

        public string InstallerMinVersion { get; set; } = "0.1.0";

        public bool RequiresAdmin { get; set; } = true;

        public bool AllowAllUsers { get; set; }

        public bool UninstallRemoveCertificate { get; set; } = true;

        public MsixpkgDisplayInfo? Display { get; set; }

        public static string FormatIsoUtc(DateTime utc)
        {
            return utc.ToUniversalTime().ToString("yyyy-MM-dd'T'HH:mm:ss'Z'", CultureInfo.InvariantCulture);
        }

        public string ToJson()
        {
            var root = JsonValue.NewObject();
            root.Add("formatVersion", JsonValue.Of((long)FormatVersion));
            root.Add("createdUtc", JsonValue.Of(CreatedUtc));
            root.Add("identity", WriteIdentity(Identity));
            root.Add("mainPackage", WriteFileRef(MainPackage, includeName: false));

            var deps = JsonValue.NewArray();
            foreach (MsixpkgFileRef dependency in Dependencies)
            {
                deps.Add(WriteFileRef(dependency, includeName: true));
            }

            root.Add("dependencies", deps);

            if (Publisher != null)
            {
                var publisher = JsonValue.NewObject();
                publisher.Add("certPath", JsonValue.Of(Publisher.CertPath));
                publisher.Add("subject", JsonValue.Of(Publisher.Subject));
                publisher.Add("thumbprintSha1", JsonValue.Of(Publisher.ThumbprintSha1));
                publisher.Add("thumbprintSha256", JsonValue.Of(Publisher.ThumbprintSha256));
                publisher.Add("notBeforeUtc", JsonValue.Of(Publisher.NotBeforeUtc));
                publisher.Add("notAfterUtc", JsonValue.Of(Publisher.NotAfterUtc));
                publisher.Add("selfSigned", JsonValue.Of(Publisher.SelfSigned));
                root.Add("publisher", publisher);
            }

            root.Add("minOsBuild", JsonValue.Of((long)MinOsBuild));
            root.Add("installerMinVersion", JsonValue.Of(InstallerMinVersion));
            root.Add("requiresAdmin", JsonValue.Of(RequiresAdmin));
            root.Add("allowAllUsers", JsonValue.Of(AllowAllUsers));
            root.Add("uninstallRemoveCertificate", JsonValue.Of(UninstallRemoveCertificate));

            if (Display != null)
            {
                var display = JsonValue.NewObject();
                if (Display.Name != null)
                {
                    display.Add("name", JsonValue.Of(Display.Name));
                }

                if (Display.Description != null)
                {
                    display.Add("description", JsonValue.Of(Display.Description));
                }

                if (Display.Homepage != null)
                {
                    display.Add("homepage", JsonValue.Of(Display.Homepage));
                }

                if (Display.IconPngBase64 != null)
                {
                    display.Add("iconPng", JsonValue.Of(Display.IconPngBase64));
                }

                root.Add("display", display);
            }

            return root.ToJson();
        }

        public static MsixpkgManifest Parse(string json)
        {
            JsonValue root;
            try
            {
                root = JsonParser.Parse(json);
            }
            catch (JsonFormatException ex)
            {
                throw new MsixpkgFormatException(
                    MsixpkgErrorCodes.ManifestSchema,
                    "msixpkg.json is not valid JSON.",
                    ex.Message);
            }

            if (root.Kind != JsonKind.Object)
            {
                throw new MsixpkgFormatException(MsixpkgErrorCodes.ManifestSchema, "msixpkg.json must be a JSON object.");
            }

            foreach (KeyValuePair<string, JsonValue> member in root.Members)
            {
                bool known = false;
                for (int i = 0; i < KnownMembers.Length; i++)
                {
                    if (string.Equals(KnownMembers[i], member.Key, StringComparison.Ordinal))
                    {
                        known = true;
                        break;
                    }
                }

                if (!known)
                {
                    throw new MsixpkgFormatException(
                        MsixpkgErrorCodes.ManifestSchema,
                        "msixpkg.json contains an unknown member.",
                        member.Key);
                }
            }

            var result = new MsixpkgManifest
            {
                FormatVersion = (int)RequireInteger(root, "formatVersion", 0, int.MaxValue),
                CreatedUtc = RequireIsoUtc(root, "createdUtc"),
                MinOsBuild = (int)RequireInteger(root, "minOsBuild", 1L, int.MaxValue),
                InstallerMinVersion = RequireString(root, "installerMinVersion"),
                RequiresAdmin = RequireBoolean(root, "requiresAdmin"),
                AllowAllUsers = RequireBoolean(root, "allowAllUsers"),
                UninstallRemoveCertificate = RequireBoolean(root, "uninstallRemoveCertificate"),
            };

            result.Identity = ParseIdentity(RequireObject(root, "identity"));
            result.MainPackage = ParseFileRef(RequireObject(root, "mainPackage"), isDependency: false);

            JsonValue dependencies = RequireMember(root, "dependencies");
            if (dependencies.Kind != JsonKind.Array)
            {
                throw Schema("'dependencies' must be an array.");
            }

            foreach (JsonValue item in dependencies.Items)
            {
                if (item.Kind != JsonKind.Object)
                {
                    throw Schema("Each 'dependencies' item must be an object.");
                }

                result.Dependencies.Add(ParseFileRef(item, isDependency: true));
            }

            if (root.TryGet("publisher", out JsonValue publisherValue) && publisherValue.Kind != JsonKind.Null)
            {
                if (publisherValue.Kind != JsonKind.Object)
                {
                    throw Schema("'publisher' must be an object.");
                }

                result.Publisher = new MsixpkgPublisherInfo(
                    RequireString(publisherValue, "certPath"),
                    RequireString(publisherValue, "subject"),
                    RequireHex(publisherValue, "thumbprintSha1", 40),
                    RequireHex(publisherValue, "thumbprintSha256", 64),
                    RequireIsoUtc(publisherValue, "notBeforeUtc"),
                    RequireIsoUtc(publisherValue, "notAfterUtc"),
                    RequireBoolean(publisherValue, "selfSigned"));
            }

            if (root.TryGet("display", out JsonValue displayValue) && displayValue.Kind != JsonKind.Null)
            {
                if (displayValue.Kind != JsonKind.Object)
                {
                    throw Schema("'display' must be an object.");
                }

                result.Display = new MsixpkgDisplayInfo(
                    OptionalString(displayValue, "name"),
                    OptionalString(displayValue, "description"),
                    OptionalString(displayValue, "homepage"),
                    OptionalString(displayValue, "iconPng"));
            }

            result.Validate();
            return result;
        }

        /// <summary>结构自检（不接触容器与磁盘），失败抛 MsixpkgFormatException。</summary>
        public void Validate()
        {
            if (FormatVersion != CurrentFormatVersion)
            {
                throw new MsixpkgFormatException(
                    MsixpkgErrorCodes.FormatVersionUnsupported,
                    "Unsupported msixpkg format version.",
                    FormatVersion.ToString(CultureInfo.InvariantCulture));
            }

            RequireIsoUtc(CreatedUtc, "createdUtc");

            if (string.IsNullOrEmpty(Identity.Name))
            {
                throw Schema("'identity.name' is empty.");
            }
            if (string.IsNullOrEmpty(Identity.Publisher))
            {
                throw Schema("'identity.publisher' is empty.");
            }

            if (!IsFourPartVersion(Identity.Version))
            {
                throw Schema("'identity.version' must be four dot-separated numbers, each 0-65535.");
            }

            if (!MsixpkgPaths.IsKnownArchitecture(Identity.Architecture))
            {
                throw Schema("'identity.architecture' is not a known architecture.");
            }

            ValidateFileRef(MainPackage, "mainPackage", expectDependency: false);

            for (int i = 0; i < Dependencies.Count; i++)
            {
                ValidateFileRef(Dependencies[i], "dependencies[" + i.ToString(CultureInfo.InvariantCulture) + "]", expectDependency: true);
            }

            if (Publisher != null)
            {
                if (!string.Equals(Publisher.CertPath, MsixpkgPaths.PublisherCertificateEntry, StringComparison.Ordinal))
                {
                    throw Schema("'publisher.certPath' must be '" + MsixpkgPaths.PublisherCertificateEntry + "'.");
                }

                if (string.IsNullOrEmpty(Publisher.Subject))
                {
                    throw Schema("'publisher.subject' is empty.");
                }

                RequireIsoUtc(Publisher.NotBeforeUtc);
                RequireIsoUtc(Publisher.NotAfterUtc);
            }

            if (MinOsBuild < 1)
            {
                throw Schema("'minOsBuild' must be positive.");
            }

            if (string.IsNullOrEmpty(InstallerMinVersion))
            {
                throw Schema("'installerMinVersion' is empty.");
            }
        }

        /// <summary>容器内是否应当包含发布者证书条目。</summary>
        public bool HasPublisherCertificate => Publisher != null;

        internal static string NormalizeHex(string value)
        {
            return value.ToLowerInvariant();
        }

        private static void ValidateFileRef(MsixpkgFileRef fileRef, string jsonName, bool expectDependency)
        {
            if (string.IsNullOrEmpty(fileRef.Path))
            {
                throw Schema("'" + jsonName + ".path' is empty.");
            }

            MsixpkgEntryKind kind;
            try
            {
                kind = MsixpkgPaths.Classify(fileRef.Path);
            }
            catch (MsixpkgFormatException ex)
            {
                throw new MsixpkgFormatException(ex.Code, "'" + jsonName + ".path' is not a valid entry name.", ex.Detail ?? fileRef.Path);
            }

            MsixpkgEntryKind expected = expectDependency ? MsixpkgEntryKind.Dependency : MsixpkgEntryKind.MainPackage;
            if (kind != expected)
            {
                throw Schema("'" + jsonName + ".path' is not a " + (expectDependency ? "dependency" : "main package") + " entry.");
            }

            if (!IsHex(fileRef.Sha256, 64))
            {
                throw Schema("'" + jsonName + ".sha256' must be 64 lowercase hex characters.");
            }

            if (fileRef.Size <= 0L)
            {
                throw Schema("'" + jsonName + ".size' must be positive.");
            }

            if (expectDependency)
            {
                if (string.IsNullOrEmpty(fileRef.Name) || string.IsNullOrEmpty(fileRef.Version))
                {
                    throw Schema("Dependency entries require 'name' and 'version'.");
                }
            }
        }

        private static bool IsFourPartVersion(string version)
        {
            if (string.IsNullOrEmpty(version))
            {
                return false;
            }

            string[] parts = version.Split('.');
            if (parts.Length != 4)
            {
                return false;
            }

            for (int i = 0; i < parts.Length; i++)
            {
                if (parts[i].Length == 0 || parts[i].Length > 5)
                {
                    return false;
                }

                for (int j = 0; j < parts[i].Length; j++)
                {
                    if (parts[i][j] < '0' || parts[i][j] > '9')
                    {
                        return false;
                    }
                }

                if (!int.TryParse(parts[i], NumberStyles.None, CultureInfo.InvariantCulture, out int value) || value > 65535)
                {
                    return false;
                }
            }

            return true;
        }

        internal static bool IsHex(string? value, int length)
        {
            if (value == null || value.Length != length)
            {
                return false;
            }

            for (int i = 0; i < value.Length; i++)
            {
                char c = value[i];
                bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
                if (!hex)
                {
                    return false;
                }
            }

            return true;
        }

        private static JsonValue WriteIdentity(MsixpkgIdentity identity)
        {
            var value = JsonValue.NewObject();
            value.Add("name", JsonValue.Of(identity.Name));
            value.Add("publisher", JsonValue.Of(identity.Publisher));
            value.Add("version", JsonValue.Of(identity.Version));
            value.Add("architecture", JsonValue.Of(identity.Architecture));
            return value;
        }

        private static JsonValue WriteFileRef(MsixpkgFileRef fileRef, bool includeName)
        {
            var value = JsonValue.NewObject();
            value.Add("path", JsonValue.Of(fileRef.Path));
            value.Add("sha256", JsonValue.Of(fileRef.Sha256));
            value.Add("size", JsonValue.Of(fileRef.Size));
            if (includeName)
            {
                value.Add("name", JsonValue.Of(fileRef.Name ?? string.Empty));
                value.Add("version", JsonValue.Of(fileRef.Version ?? string.Empty));
            }

            return value;
        }

        private static MsixpkgIdentity ParseIdentity(JsonValue value)
        {
            return new MsixpkgIdentity(
                RequireString(value, "name"),
                RequireString(value, "publisher"),
                RequireString(value, "version"),
                RequireString(value, "architecture"));
        }

        private static MsixpkgFileRef ParseFileRef(JsonValue value, bool isDependency)
        {
            return new MsixpkgFileRef(
                RequireString(value, "path"),
                RequireHex(value, "sha256", 64),
                RequireInteger(value, "size", 1L, long.MaxValue),
                isDependency ? RequireString(value, "name") : null,
                isDependency ? RequireString(value, "version") : null);
        }

        private static JsonValue RequireMember(JsonValue parent, string name)
        {
            if (!parent.TryGet(name, out JsonValue value))
            {
                throw Schema("Missing required member '" + name + "'.");
            }

            return value;
        }

        private static JsonValue RequireObject(JsonValue parent, string name)
        {
            JsonValue value = RequireMember(parent, name);
            if (value.Kind != JsonKind.Object)
            {
                throw Schema("'" + name + "' must be an object.");
            }

            return value;
        }

        private static string RequireString(JsonValue parent, string name)
        {
            JsonValue value = RequireMember(parent, name);
            if (value.Kind != JsonKind.String || value.StringValue.Length == 0)
            {
                throw Schema("'" + name + "' must be a non-empty string.");
            }

            return value.StringValue;
        }

        private static string? OptionalString(JsonValue parent, string name)
        {
            if (!parent.TryGet(name, out JsonValue value) || value.Kind == JsonKind.Null)
            {
                return null;
            }

            if (value.Kind != JsonKind.String)
            {
                throw Schema("'" + name + "' must be a string.");
            }

            return value.StringValue;
        }

        private static bool RequireBoolean(JsonValue parent, string name)
        {
            JsonValue value = RequireMember(parent, name);
            if (value.Kind != JsonKind.Boolean)
            {
                throw Schema("'" + name + "' must be a boolean.");
            }

            return value.BooleanValue;
        }

        private static long RequireInteger(JsonValue parent, string name, long min, long max)
        {
            JsonValue value = RequireMember(parent, name);
            if (value.Kind != JsonKind.Integer || value.IntegerValue < min || value.IntegerValue > max)
            {
                throw Schema("'" + name + "' must be an integer in range.");
            }

            return value.IntegerValue;
        }

        private static string RequireHex(JsonValue parent, string name, int length)
        {
            string value = RequireString(parent, name);
            if (!IsHex(value, length))
            {
                throw Schema("'" + name + "' must be " + length.ToString(CultureInfo.InvariantCulture) + " lowercase hex characters.");
            }

            return value;
        }

        private static string RequireIsoUtc(JsonValue parent, string name)
        {
            return RequireIsoUtc(RequireString(parent, name), name);
        }

        private static string RequireIsoUtc(string value)
        {
            return RequireIsoUtc(value, "timestamp");
        }

        private static string RequireIsoUtc(string value, string name)
        {
            if (!DateTime.TryParseExact(
                    value,
                    "yyyy-MM-dd'T'HH:mm:ss'Z'",
                    CultureInfo.InvariantCulture,
                    DateTimeStyles.AdjustToUniversal | DateTimeStyles.AssumeUniversal,
                    out _))
            {
                throw Schema("'" + name + "' must be an ISO-8601 UTC timestamp like 2026-10-08T12:00:00Z.");
            }

            return value;
        }

        private static MsixpkgFormatException Schema(string detail)
        {
            return new MsixpkgFormatException(MsixpkgErrorCodes.ManifestSchema, detail, detail);
        }
    }
}
