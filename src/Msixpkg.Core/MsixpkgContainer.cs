using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.IO.Compression;
using System.Text;

namespace Msixpkg.Core
{
    public sealed class MsixpkgLimits
    {
        public int MaxEntryCount { get; set; } = 4096;

        public long MaxContainerSize { get; set; } = 8L * 1024 * 1024 * 1024;

        public long MaxEntrySize { get; set; } = 2L * 1024 * 1024 * 1024;

        public long MaxTotalUncompressedSize { get; set; } = 8L * 1024 * 1024 * 1024;

        /// <summary>只有大于该阈值的条目才做压缩比检查，避免小文件的比值抖动造成误报。</summary>
        public long RatioGuardThreshold { get; set; } = 16L * 1024 * 1024;

        public long MaxCompressionRatio { get; set; } = 200L;

        public static MsixpkgLimits Default { get; } = new MsixpkgLimits();
    }

    public sealed class MsixpkgContainerEntry
    {
        internal MsixpkgContainerEntry(int index, string name, MsixpkgEntryKind kind, long compressedSize, long uncompressedSize)
        {
            Index = index;
            Name = name;
            Kind = kind;
            CompressedSize = compressedSize;
            UncompressedSize = uncompressedSize;
        }

        public int Index { get; }

        public string Name { get; }

        public MsixpkgEntryKind Kind { get; }

        public long CompressedSize { get; }

        public long UncompressedSize { get; }
    }

    /// <summary>只读容器访问器。Open 时完成全部结构性校验（命名白名单、重复、体积与压缩比限额）。</summary>
    public sealed class MsixpkgContainer : IDisposable
    {
        private readonly FileStream _file;
        private readonly ZipArchive _archive;
        private readonly Dictionary<string, MsixpkgContainerEntry> _byName;
        private readonly List<MsixpkgContainerEntry> _entries;
        private bool _disposed;

        private MsixpkgContainer(string path, FileStream file, ZipArchive archive, List<MsixpkgContainerEntry> entries)
        {
            Path = path;
            _file = file;
            _archive = archive;
            _entries = entries;
            _byName = new Dictionary<string, MsixpkgContainerEntry>(StringComparer.OrdinalIgnoreCase);
            foreach (MsixpkgContainerEntry entry in entries)
            {
                _byName.Add(entry.Name, entry);
            }
        }

        public string Path { get; }

        public long ContainerSize => _file.Length;

        public IReadOnlyList<MsixpkgContainerEntry> Entries => _entries;

        public static MsixpkgContainer Open(string path, MsixpkgLimits? limits = null)
        {
            if (string.IsNullOrEmpty(path))
            {
                throw new ArgumentException("Container path is required.", nameof(path));
            }

            limits ??= MsixpkgLimits.Default;

            FileInfo info = new FileInfo(path);
            if (!info.Exists)
            {
                throw new MsixpkgFormatException(MsixpkgErrorCodes.IoError, "The container file does not exist.", path);
            }

            if (info.Length == 0L)
            {
                throw new MsixpkgFormatException(MsixpkgErrorCodes.ContainerEmpty, "The container file is empty.", path);
            }

            if (info.Length > limits.MaxContainerSize)
            {
                throw new MsixpkgFormatException(
                    MsixpkgErrorCodes.ContainerTooLarge,
                    "The container exceeds the maximum allowed size.",
                    info.Length.ToString(CultureInfo.InvariantCulture));
            }

            FileStream file = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read);
            try
            {
                ZipArchive archive;
                try
                {
                    archive = new ZipArchive(file, ZipArchiveMode.Read, leaveOpen: true);
                }
                catch (InvalidDataException ex)
                {
                    throw new MsixpkgFormatException(MsixpkgErrorCodes.ContainerInvalid, "The container is not a valid ZIP archive.", ex.Message);
                }

                var entries = new List<MsixpkgContainerEntry>();
                var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
                long total = 0L;

                try
                {
                    if (archive.Entries.Count > limits.MaxEntryCount)
                    {
                        throw new MsixpkgFormatException(
                            MsixpkgErrorCodes.EntryCountExceeded,
                            "The container contains too many entries.",
                            archive.Entries.Count.ToString(CultureInfo.InvariantCulture));
                    }

                    for (int i = 0; i < archive.Entries.Count; i++)
                    {
                        ZipArchiveEntry zipEntry = archive.Entries[i];
                        string name = zipEntry.FullName;

                        MsixpkgEntryKind kind = MsixpkgPaths.Classify(name);

                        if (!seen.Add(name))
                        {
                            throw new MsixpkgFormatException(MsixpkgErrorCodes.EntryDuplicate, "The container contains duplicate entry names.", name);
                        }

                        if (zipEntry.Length > limits.MaxEntrySize)
                        {
                            throw new MsixpkgFormatException(
                                MsixpkgErrorCodes.EntryTooLarge,
                                "A container entry exceeds the maximum allowed size.",
                                name);
                        }

                        if (zipEntry.Length >= limits.RatioGuardThreshold
                            && zipEntry.CompressedLength > 0L
                            && zipEntry.Length / zipEntry.CompressedLength > limits.MaxCompressionRatio)
                        {
                            throw new MsixpkgFormatException(
                                MsixpkgErrorCodes.EntryRatioExceeded,
                                "A container entry exceeds the maximum allowed compression ratio.",
                                name);
                        }

                        total += zipEntry.Length;
                        if (total > limits.MaxTotalUncompressedSize)
                        {
                            throw new MsixpkgFormatException(
                                MsixpkgErrorCodes.TotalSizeExceeded,
                                "The container expands beyond the maximum allowed total size.");
                        }

                        entries.Add(new MsixpkgContainerEntry(i, name, kind, zipEntry.CompressedLength, zipEntry.Length));
                    }

                    return new MsixpkgContainer(path, file, archive, entries);
                }
                catch
                {
                    archive.Dispose();
                    throw;
                }
            }
            catch
            {
                file.Dispose();
                throw;
            }
        }

        public MsixpkgContainerEntry? Find(string name)
        {
            return _byName.TryGetValue(name, out MsixpkgContainerEntry? entry) ? entry : null;
        }

        public MsixpkgContainerEntry Require(string name, string errorCode, string message)
        {
            MsixpkgContainerEntry? entry = Find(name);
            if (entry == null)
            {
                throw new MsixpkgFormatException(errorCode, message, name);
            }

            return entry;
        }

        public byte[] ReadEntryBytes(string name)
        {
            MsixpkgContainerEntry entry = Require(name, MsixpkgErrorCodes.FileMissing, "The container entry is missing.");
            if (entry.UncompressedSize > int.MaxValue)
            {
                throw new MsixpkgFormatException(MsixpkgErrorCodes.EntryTooLarge, "The entry is too large to read into memory.", name);
            }

            using Stream stream = OpenEntry(name);
            var buffer = new byte[(int)entry.UncompressedSize];
            int offset = 0;
            while (offset < buffer.Length)
            {
                int read = stream.Read(buffer, offset, buffer.Length - offset);
                if (read <= 0)
                {
                    throw new MsixpkgFormatException(MsixpkgErrorCodes.ContainerInvalid, "The container entry is truncated.", name);
                }

                offset += read;
            }

            return buffer;
        }

        public string ReadEntryText(string name)
        {
            return Encoding.UTF8.GetString(ReadEntryBytes(name));
        }

        public Stream OpenEntry(string name)
        {
            ZipArchiveEntry entry = _archive.GetEntry(name)
                ?? throw new MsixpkgFormatException(MsixpkgErrorCodes.FileMissing, "The container entry is missing.", name);
            return entry.Open();
        }

        public string ComputeEntrySha256(string name)
        {
            using Stream stream = OpenEntry(name);
            using var sha = System.Security.Cryptography.SHA256.Create();
            var buffer = new byte[1024 * 128];
            int read;
            while ((read = stream.Read(buffer, 0, buffer.Length)) > 0)
            {
                sha.TransformBlock(buffer, 0, read, null, 0);
            }

            sha.TransformFinalBlock(Array.Empty<byte>(), 0, 0);
            return Sha256Util.ToLowerHex(sha.Hash!);
        }

        public void ExtractEntryToFile(string name, string destinationPath)
        {
            string? directory = System.IO.Path.GetDirectoryName(System.IO.Path.GetFullPath(destinationPath));
            if (!string.IsNullOrEmpty(directory))
            {
                Directory.CreateDirectory(directory);
            }

            using Stream source = OpenEntry(name);
            using var destination = new FileStream(destinationPath, FileMode.Create, FileAccess.Write, FileShare.None);
            source.CopyTo(destination);
            destination.Flush(true);
        }

        public MsixpkgManifest ReadManifest()
        {
            return MsixpkgManifest.Parse(ReadEntryText(MsixpkgPaths.ManifestEntry));
        }

        public MsixpkgSignature? ReadSignature()
        {
            if (Find(MsixpkgPaths.SignatureEntry) == null)
            {
                return null;
            }

            return MsixpkgSignatureCodec.Parse(ReadEntryText(MsixpkgPaths.SignatureEntry));
        }

        /// <summary>按规范算法计算内容摘要的输入项。excludeSignature 时排除 msixpkg.sig 自身。</summary>
        public List<MsixpkgContentDigestEntry> ComputeDigestEntries(bool excludeSignature)
        {
            var result = new List<MsixpkgContentDigestEntry>(_entries.Count);
            foreach (MsixpkgContainerEntry entry in _entries)
            {
                if (excludeSignature && entry.Kind == MsixpkgEntryKind.Signature)
                {
                    continue;
                }

                result.Add(new MsixpkgContentDigestEntry(entry.Name, ComputeEntrySha256(entry.Name)));
            }

            return result;
        }

        public void Dispose()
        {
            if (_disposed)
            {
                return;
            }

            _disposed = true;
            _archive.Dispose();
            _file.Dispose();
        }
    }
}
