using System;
using System.Collections.Generic;
using System.IO;
using System.IO.Compression;
using System.Text;
using Msixpkg.Core;
using Msixpkg.Core.Zip;

namespace Msixpkg.Core.Tests;

/// <summary>
/// <see cref="StoredZipWriter"/> 的契约。整体字节是否与黄金样本一致由 <see cref="VectorTests"/> 断言，
/// 这里钉住的是让那个断言成立的三条底层性质：
///   1) 只写 store（method 0）—— 不能用 System.IO.Compression.ZipArchive（.NET Framework 宿主
///      会把 CompressionLevel.NoCompression 写成 method 8，同一个工程在 VS 与 dotnet build 下字节不同）；
///   2) 本地头 / 中央目录不带额外字段与数据描述符，version 20；
///   3) 条目时间戳取传入时间戳的墙上时间，不经过本机时区（否则不同时区的机器产出不同字节）。
/// </summary>
public class ZipWriterTests
{
    [Fact]
    public void Crc32MatchesTheStandardCheckValue()
    {
        byte[] check = Encoding.ASCII.GetBytes("123456789");
        Assert.Equal(0xCBF43926u, Crc32.Compute(check, 0, check.Length));
    }

    [Fact]
    public void Crc32IsTheSameWhetherComputedInOnePieceOrIncrementally()
    {
        byte[] data = Encoding.UTF8.GetBytes("the quick brown fox jumps over the lazy dog");

        uint whole = Crc32.Compute(data, 0, data.Length);
        uint running = Crc32.Seed;
        for (int i = 0; i < data.Length; i++)
        {
            running = Crc32.Update(running, data, i, 1);
        }

        Assert.Equal(whole, Crc32.Finish(running));
    }

    [Fact]
    public void EntriesAreStoredAndReadBackIntact()
    {
        byte[] asciiContent = Encoding.UTF8.GetBytes("plain payload");
        byte[] unicodeContent = new byte[] { 0x00, 0x01, 0xFE, 0xFF, 0x7F };
        var timestamp = new DateTimeOffset(2026, 1, 1, 0, 0, 0, TimeSpan.Zero);

        byte[] archiveBytes;
        using (var buffer = new MemoryStream())
        {
            using (var writer = new StoredZipWriter(buffer, leaveOpen: true))
            {
                writer.AddBytes("ascii.txt", asciiContent, timestamp);
                writer.AddBytes("目录/中文.txt", unicodeContent, timestamp);
            }

            archiveBytes = buffer.ToArray();
        }

        List<ZipRecord> records = ZipProbe.ReadCentralDirectory(archiveBytes);
        Assert.Equal(2, records.Count);

        Assert.Equal("ascii.txt", records[0].Name);
        Assert.Equal(0, records[0].Method);
        Assert.Equal(0, records[0].Flags);
        Assert.Equal(0, records[0].ExtraFieldLength);
        Assert.Equal(asciiContent.Length, records[0].Size);

        // 非 ASCII 名字必须置 UTF-8 标志位（0x0800），否则读取端只能按 CP437 猜。
        Assert.Equal("目录/中文.txt", records[1].Name);
        Assert.Equal(0x0800, records[1].Flags);
        Assert.Equal(0, records[1].Method);

        foreach (ZipRecord record in records)
        {
            Assert.Equal(0, ZipProbe.ReadLocalMethod(archiveBytes, record.LocalHeaderOffset));
            Assert.Equal(0, ZipProbe.ReadLocalExtraFieldLength(archiveBytes, record.LocalHeaderOffset));
        }

        using var stream = new MemoryStream(archiveBytes);
        using var archive = new ZipArchive(stream, ZipArchiveMode.Read);
        Assert.Equal(asciiContent, ReadAll(archive.GetEntry("ascii.txt")!));
        Assert.Equal(unicodeContent, ReadAll(archive.GetEntry("目录/中文.txt")!));
    }

    [Fact]
    public void EntryTimestampUsesTheWallClockNotTheMachineTimeZone()
    {
        var utc = new DateTimeOffset(2026, 1, 1, 0, 0, 0, TimeSpan.Zero);
        var sameWallClockOtherOffset = new DateTimeOffset(2026, 1, 1, 0, 0, 0, TimeSpan.FromHours(8));

        byte[] first = WriteSingleEntry("a.txt", utc);
        byte[] second = WriteSingleEntry("a.txt", sameWallClockOtherOffset);

        // 两次写出的本地头时间字段必须一致：ZIP 的 DOS 时间没有时区，写端不换算。
        Assert.Equal(
            ZipProbe.ReadLocalDosTime(first),
            ZipProbe.ReadLocalDosTime(second));
        Assert.Equal(
            ZipProbe.ReadLocalDosDate(first),
            ZipProbe.ReadLocalDosDate(second));
    }

    [Fact]
    public void TimestampsOutsideTheDosRangeAreRejected()
    {
        var tooEarly = new DateTimeOffset(1979, 12, 31, 23, 59, 59, TimeSpan.Zero);

        MsixpkgFormatException error = Assert.Throws<MsixpkgFormatException>(() => WriteSingleEntry("a.txt", tooEarly));
        Assert.Equal(MsixpkgErrorCodes.ManifestSchema, error.Code);
    }

    [Fact]
    public void CommittedGoldenContainerUsesOnlyStoredEntries()
    {
        VectorGenerator.EnsureGenerated();
        Assert.True(File.Exists(VectorGenerator.ContainerPath), "Missing golden vector.");

        byte[] bytes = File.ReadAllBytes(VectorGenerator.ContainerPath);
        List<ZipRecord> records = ZipProbe.ReadCentralDirectory(bytes);

        Assert.Equal(5, records.Count);
        Assert.All(records, record => Assert.Equal(0, record.Method));
        Assert.All(records, record => Assert.Equal(0, ZipProbe.ReadLocalMethod(bytes, record.LocalHeaderOffset)));
    }

    private static byte[] WriteSingleEntry(string name, DateTimeOffset timestamp)
    {
        using var buffer = new MemoryStream();
        using (var writer = new StoredZipWriter(buffer, leaveOpen: true))
        {
            writer.AddBytes(name, Encoding.UTF8.GetBytes("x"), timestamp);
        }

        return buffer.ToArray();
    }

    private static byte[] ReadAll(ZipArchiveEntry entry)
    {
        using Stream stream = entry.Open();
        using var memory = new MemoryStream();
        stream.CopyTo(memory);
        return memory.ToArray();
    }
}

internal readonly struct ZipRecord
{
    public ZipRecord(string name, int method, int flags, int extraFieldLength, long localHeaderOffset, int size)
    {
        Name = name;
        Method = method;
        Flags = flags;
        ExtraFieldLength = extraFieldLength;
        LocalHeaderOffset = localHeaderOffset;
        Size = size;
    }

    public string Name { get; }

    public int Method { get; }

    public int Flags { get; }

    public int ExtraFieldLength { get; }

    public long LocalHeaderOffset { get; }

    public int Size { get; }
}

/// <summary>测试专用：直接读 ZIP 的本地头 / 中央目录字段（ZipArchive 不暴露 method 与标志位）。</summary>
internal static class ZipProbe
{
    private const int EndOfCentralDirectorySize = 22;
    private const int CentralHeaderSize = 46;

    public static List<ZipRecord> ReadCentralDirectory(byte[] bytes)
    {
        int eocd = FindEndOfCentralDirectory(bytes);
        int count = ReadU16(bytes, eocd + 8);
        int offset = (int)ReadU32(bytes, eocd + 16);
        var records = new List<ZipRecord>(count);

        for (int i = 0; i < count; i++)
        {
            AssertSignature(bytes, offset, 0x02014B50);
            int flags = ReadU16(bytes, offset + 8);
            int method = ReadU16(bytes, offset + 10);
            int size = (int)ReadU32(bytes, offset + 24);
            int nameLength = ReadU16(bytes, offset + 28);
            int extraLength = ReadU16(bytes, offset + 30);
            int commentLength = ReadU16(bytes, offset + 32);
            long localHeaderOffset = ReadU32(bytes, offset + 42);
            string name = Encoding.UTF8.GetString(bytes, offset + CentralHeaderSize, nameLength);

            records.Add(new ZipRecord(name, method, flags, extraLength, localHeaderOffset, size));
            offset += CentralHeaderSize + nameLength + extraLength + commentLength;
        }

        return records;
    }

    public static int ReadLocalMethod(byte[] bytes, long localHeaderOffset)
    {
        AssertSignature(bytes, (int)localHeaderOffset, 0x04034B50);
        return ReadU16(bytes, (int)localHeaderOffset + 8);
    }

    public static int ReadLocalExtraFieldLength(byte[] bytes, long localHeaderOffset)
    {
        AssertSignature(bytes, (int)localHeaderOffset, 0x04034B50);
        return ReadU16(bytes, (int)localHeaderOffset + 28);
    }

    public static int ReadLocalDosTime(byte[] bytes) => ReadU16(bytes, 10);

    public static int ReadLocalDosDate(byte[] bytes) => ReadU16(bytes, 12);

    private static int FindEndOfCentralDirectory(byte[] bytes)
    {
        for (int i = bytes.Length - EndOfCentralDirectorySize; i >= 0; i--)
        {
            if (bytes[i] == 0x50 && bytes[i + 1] == 0x4B && bytes[i + 2] == 0x05 && bytes[i + 3] == 0x06)
            {
                return i;
            }
        }

        throw new InvalidOperationException("The archive has no end-of-central-directory record.");
    }

    private static void AssertSignature(byte[] bytes, int offset, uint expected)
    {
        uint actual = ReadU32(bytes, offset);
        if (actual != expected)
        {
            throw new InvalidOperationException(
                "Unexpected ZIP signature at " + offset + ": 0x" + actual.ToString("X8") + ".");
        }
    }

    private static int ReadU16(byte[] bytes, int offset) => bytes[offset] | (bytes[offset + 1] << 8);

    private static uint ReadU32(byte[] bytes, int offset)
    {
        return (uint)(bytes[offset] | (bytes[offset + 1] << 8) | (bytes[offset + 2] << 16) | (bytes[offset + 3] << 24));
    }
}
