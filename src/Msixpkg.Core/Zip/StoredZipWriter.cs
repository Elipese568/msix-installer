using System;
using System.Collections.Generic;
using System.IO;
using System.Security.Cryptography;
using System.Text;

namespace Msixpkg.Core.Zip
{
    /// <summary>
    /// 只写 <c>store</c>（method 0）条目的最小 ZIP 写出器，产出的字节与宿主运行时无关。
    ///
    /// 为什么不用 <c>System.IO.Compression.ZipArchive</c>：同一个
    /// <c>CompressionLevel.NoCompression</c> 在两个宿主上语义不同 ——
    /// .NET (Core) 写 method 0（真正的 store），.NET Framework 写 method 8
    /// （deflate 里包一层 stored block，每个条目多 5 字节）。
    /// 于是同一个工程用 <c>dotnet build</c> 与 Visual Studio 构建会得到不同字节的容器：
    /// MSBuild 任务恰好要在这两个宿主里跑，而 docs/msixpkg-format-v1.md 规定
    /// 「读取端必须支持 store 与 deflate，写出端必须使用 store」，黄金样本的逐字节断言也依赖它。
    ///
    /// 自己写还顺带修掉一个时区隐患：条目时间戳直接由传入 <see cref="DateTimeOffset"/> 的
    /// 墙上时间换算成 DOS 时间，不经过本机时区。
    ///
    /// 布局与 ZipArchive 写出的黄金样本逐字节一致（本地头 → 数据 → 中央目录 → EOCD，
    /// 无额外字段、无数据描述符、version made by / needed 均为 20）、CRC-32 为 IEEE 反射多项式。
    /// </summary>
    internal sealed class StoredZipWriter : IDisposable
    {
        private const uint LocalHeaderSignature = 0x04034B50;
        private const uint CentralHeaderSignature = 0x02014B50;
        private const uint EndOfCentralDirectorySignature = 0x06054B50;

        /// <summary>2.0：与 ZipArchive 写出的值一致（store 条目不需要更高版本）。</summary>
        private const ushort VersionNeeded = 20;

        private const ushort VersionMadeBy = 20;

        /// <summary>通用标志位 bit 11：条目名是 UTF-8。ASCII 名字不置该位（与 ZipArchive 一致）。</summary>
        private const ushort Utf8NameFlag = 0x0800;

        private const int MaximumNameBytes = 0xFFFF;

        private readonly Stream _stream;
        private readonly bool _leaveOpen;
        private readonly List<CentralRecord> _records = new List<CentralRecord>();
        private bool _finished;

        public StoredZipWriter(Stream stream, bool leaveOpen = false)
        {
            _stream = stream ?? throw new ArgumentNullException(nameof(stream));
            if (!stream.CanWrite)
            {
                throw new ArgumentException("The stream must be writable.", nameof(stream));
            }

            if (!stream.CanSeek)
            {
                // 本地头里的 CRC 与大小必须在数据之后回填（flags 不带数据描述符），所以必须可 seek。
                throw new ArgumentException("The stream must be seekable.", nameof(stream));
            }

            _leaveOpen = leaveOpen;
        }

        /// <summary>写入一个内存条目。</summary>
        public void AddBytes(string name, byte[] content, DateTimeOffset timestamp)
        {
            if (content == null)
            {
                throw new ArgumentNullException(nameof(content));
            }

            EntryContext context = BeginEntry(name, timestamp);
            _stream.Write(content, 0, content.Length);
            EndEntry(context, Crc32.Compute(content, 0, content.Length), content.Length);
        }

        /// <summary>
        /// 把一个文件作为条目流式写入，同时算出内容的小写 SHA-256（读一遍、写一遍、算一遍）。
        /// </summary>
        public ZipEntryInfo AddFile(string name, string filePath, DateTimeOffset timestamp)
        {
            if (string.IsNullOrEmpty(filePath))
            {
                throw new ArgumentException("A source file path is required.", nameof(filePath));
            }

            EntryContext context = BeginEntry(name, timestamp);
            var buffer = new byte[1024 * 128];
            uint crc = Crc32.Seed;
            long total = 0L;

            using (var sha = SHA256.Create())
            {
                using (var source = new FileStream(filePath, FileMode.Open, FileAccess.Read, FileShare.Read))
                {
                    int read;
                    while ((read = source.Read(buffer, 0, buffer.Length)) > 0)
                    {
                        _stream.Write(buffer, 0, read);
                        crc = Crc32.Update(crc, buffer, 0, read);
                        sha.TransformBlock(buffer, 0, read, null, 0);
                        total += read;
                    }
                }

                sha.TransformFinalBlock(Array.Empty<byte>(), 0, 0);
                uint finalCrc = Crc32.Finish(crc);
                EndEntry(context, finalCrc, total);
                return new ZipEntryInfo(Sha256Util.ToLowerHex(sha.Hash!), total, finalCrc);
            }
        }

        /// <summary>写中央目录与 EOCD。重复调用是安全的。</summary>
        public void Finish()
        {
            if (_finished)
            {
                return;
            }

            _finished = true;

            long centralDirectoryOffset = _stream.Position;
            if (_records.Count > ushort.MaxValue || centralDirectoryOffset > uint.MaxValue)
            {
                throw new MsixpkgFormatException(
                    MsixpkgErrorCodes.ContainerTooLarge,
                    "The container is too large for a ZIP32 central directory.");
            }

            foreach (CentralRecord record in _records)
            {
                WriteU32(CentralHeaderSignature);
                WriteU16(VersionMadeBy);
                WriteU16(VersionNeeded);
                WriteU16(record.Flags);
                WriteU16(0); // method: store
                WriteU16(record.DosTime);
                WriteU16(record.DosDate);
                WriteU32(record.Crc32);
                WriteU32((uint)record.Size);
                WriteU32((uint)record.Size);
                WriteU16((ushort)record.Name.Length);
                WriteU16(0); // extra field length
                WriteU16(0); // comment length
                WriteU16(0); // disk number start
                WriteU16(0); // internal attributes
                WriteU32(0); // external attributes
                WriteU32((uint)record.LocalHeaderOffset);
                _stream.Write(record.Name, 0, record.Name.Length);
            }

            long centralDirectorySize = _stream.Position - centralDirectoryOffset;

            WriteU32(EndOfCentralDirectorySignature);
            WriteU16(0); // this disk
            WriteU16(0); // disk with the central directory
            WriteU16((ushort)_records.Count);
            WriteU16((ushort)_records.Count);
            WriteU32((uint)centralDirectorySize);
            WriteU32((uint)centralDirectoryOffset);
            WriteU16(0); // comment length
        }

        public void Dispose()
        {
            Finish();
            if (!_leaveOpen)
            {
                _stream.Dispose();
            }
        }

        private EntryContext BeginEntry(string name, DateTimeOffset timestamp)
        {
            if (string.IsNullOrEmpty(name))
            {
                throw new ArgumentException("An entry name is required.", nameof(name));
            }

            if (_finished)
            {
                throw new InvalidOperationException("The central directory has already been written.");
            }

            byte[] nameBytes = Encoding.UTF8.GetBytes(name);
            if (nameBytes.Length == 0 || nameBytes.Length > MaximumNameBytes)
            {
                throw new MsixpkgFormatException(
                    MsixpkgErrorCodes.EntryNameInvalid,
                    "An entry name must be between 1 and 65535 UTF-8 bytes.",
                    name);
            }

            ushort flags = HasNonAsciiByte(nameBytes) ? Utf8NameFlag : (ushort)0;
            ToDosDateTime(timestamp, out ushort dosTime, out ushort dosDate);

            long headerOffset = _stream.Position;
            WriteU32(LocalHeaderSignature);
            WriteU16(VersionNeeded);
            WriteU16(flags);
            WriteU16(0); // method: store
            WriteU16(dosTime);
            WriteU16(dosDate);
            WriteU32(0); // CRC-32：数据写完回填
            WriteU32(0); // 压缩后大小：store 条目等于原始大小
            WriteU32(0); // 原始大小
            WriteU16((ushort)nameBytes.Length);
            WriteU16(0); // extra field length
            _stream.Write(nameBytes, 0, nameBytes.Length);

            return new EntryContext(headerOffset, nameBytes, flags, dosTime, dosDate);
        }

        private void EndEntry(EntryContext context, uint crc, long size)
        {
            if (size < 0 || size > uint.MaxValue)
            {
                throw new MsixpkgFormatException(
                    MsixpkgErrorCodes.EntryTooLarge,
                    "A container entry exceeds the 4 GiB ZIP32 limit.",
                    Encoding.UTF8.GetString(context.Name));
            }

            long afterData = _stream.Position;

            // 回填本地头的 CRC / 压缩大小 / 原始大小（偏移 14 / 18 / 22）。
            _stream.Position = context.HeaderOffset + 14;
            WriteU32(crc);
            WriteU32((uint)size);
            WriteU32((uint)size);
            _stream.Position = afterData;

            _records.Add(new CentralRecord(context, crc, size));
        }

        private void WriteU16(ushort value)
        {
            _stream.WriteByte((byte)(value & 0xFF));
            _stream.WriteByte((byte)((value >> 8) & 0xFF));
        }

        private void WriteU32(uint value)
        {
            _stream.WriteByte((byte)(value & 0xFF));
            _stream.WriteByte((byte)((value >> 8) & 0xFF));
            _stream.WriteByte((byte)((value >> 16) & 0xFF));
            _stream.WriteByte((byte)((value >> 24) & 0xFF));
        }

        private static bool HasNonAsciiByte(byte[] data)
        {
            for (int i = 0; i < data.Length; i++)
            {
                if (data[i] >= 0x80)
                {
                    return true;
                }
            }

            return false;
        }

        /// <summary>
        /// DOS 日期时间：ZIP 只到 2 秒粒度。用传入时间戳的**墙上时间**（不换算本机时区），
        /// 否则同一份输入在两台时区不同的机器上会产出不同字节。
        /// </summary>
        private static void ToDosDateTime(DateTimeOffset timestamp, out ushort dosTime, out ushort dosDate)
        {
            DateTime wallClock = timestamp.DateTime;
            if (wallClock.Year < 1980 || wallClock.Year > 2107)
            {
                throw new MsixpkgFormatException(
                    MsixpkgErrorCodes.ManifestSchema,
                    "The container timestamp must be between 1980-01-01 and 2107-12-31 to fit the ZIP DOS time format.",
                    MsixpkgManifest.FormatIsoUtc(wallClock));
            }

            dosTime = (ushort)((wallClock.Second / 2) | (wallClock.Minute << 5) | (wallClock.Hour << 11));
            dosDate = (ushort)(wallClock.Day | (wallClock.Month << 5) | ((wallClock.Year - 1980) << 9));
        }

        private readonly struct EntryContext
        {
            public EntryContext(long headerOffset, byte[] name, ushort flags, ushort dosTime, ushort dosDate)
            {
                HeaderOffset = headerOffset;
                Name = name;
                Flags = flags;
                DosTime = dosTime;
                DosDate = dosDate;
            }

            public long HeaderOffset { get; }

            public byte[] Name { get; }

            public ushort Flags { get; }

            public ushort DosTime { get; }

            public ushort DosDate { get; }
        }

        private readonly struct CentralRecord
        {
            public CentralRecord(EntryContext context, uint crc32, long size)
            {
                LocalHeaderOffset = context.HeaderOffset;
                Name = context.Name;
                Flags = context.Flags;
                DosTime = context.DosTime;
                DosDate = context.DosDate;
                Crc32 = crc32;
                Size = size;
            }

            public long LocalHeaderOffset { get; }

            public byte[] Name { get; }

            public ushort Flags { get; }

            public ushort DosTime { get; }

            public ushort DosDate { get; }

            public uint Crc32 { get; }

            public long Size { get; }
        }
    }

    /// <summary>一个条目的写入结果：内容哈希与字节数（调用方用它做哈希/大小一致性校验）。</summary>
    internal readonly struct ZipEntryInfo
    {
        public ZipEntryInfo(string sha256Hex, long size, uint crc32)
        {
            Sha256Hex = sha256Hex;
            Size = size;
            Crc32 = crc32;
        }

        public string Sha256Hex { get; }

        public long Size { get; }

        public uint Crc32 { get; }
    }

    /// <summary>ZIP 用的 CRC-32（IEEE 802.3，反射多项式 0xEDB88320）。</summary>
    internal static class Crc32
    {
        /// <summary>增量计算的起始值。</summary>
        public const uint Seed = 0xFFFFFFFFu;

        private static readonly uint[] Table = BuildTable();

        public static uint Compute(byte[] data, int offset, int count)
        {
            return Finish(Update(Seed, data, offset, count));
        }

        /// <summary>把一段数据折进运行中的 CRC（<paramref name="crc"/> 取自 <see cref="Seed"/> 或上次的返回值）。</summary>
        public static uint Update(uint crc, byte[] data, int offset, int count)
        {
            uint accumulator = crc;
            int end = offset + count;
            for (int i = offset; i < end; i++)
            {
                accumulator = Table[(accumulator ^ data[i]) & 0xFF] ^ (accumulator >> 8);
            }

            return accumulator;
        }

        public static uint Finish(uint crc)
        {
            return crc ^ 0xFFFFFFFFu;
        }

        private static uint[] BuildTable()
        {
            var table = new uint[256];
            for (uint i = 0; i < 256; i++)
            {
                uint value = i;
                for (int bit = 0; bit < 8; bit++)
                {
                    value = (value & 1) != 0 ? 0xEDB88320u ^ (value >> 1) : value >> 1;
                }

                table[i] = value;
            }

            return table;
        }
    }
}
