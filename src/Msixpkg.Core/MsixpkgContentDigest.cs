using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Security.Cryptography;
using System.Text;

namespace Msixpkg.Core
{
    public static class Sha256Util
    {
        public static string ToLowerHex(byte[] hash)
        {
            if (hash == null)
            {
                throw new ArgumentNullException(nameof(hash));
            }

            var sb = new StringBuilder(hash.Length * 2);
            for (int i = 0; i < hash.Length; i++)
            {
                sb.Append(hash[i].ToString("x2", CultureInfo.InvariantCulture));
            }

            return sb.ToString();
        }

        public static string HashBytesHex(byte[] data)
        {
            using (var sha = SHA256.Create())
            {
                return ToLowerHex(sha.ComputeHash(data));
            }
        }

        public static string HashTextHex(string text)
        {
            return HashBytesHex(Encoding.UTF8.GetBytes(text));
        }

        public static string HashFileHex(string path)
        {
            using (var stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read))
            {
                return HashStreamHex(stream);
            }
        }

        public static string HashStreamHex(Stream stream)
        {
            using (var sha = SHA256.Create())
            {
                return ToLowerHex(sha.ComputeHash(stream));
            }
        }
    }

    public sealed class MsixpkgContentDigestEntry
    {
        public MsixpkgContentDigestEntry(string name, string sha256Hex)
        {
            Name = name ?? throw new ArgumentNullException(nameof(name));
            Sha256Hex = sha256Hex ?? throw new ArgumentNullException(nameof(sha256Hex));
        }

        public string Name { get; }

        public string Sha256Hex { get; }
    }

    /// <summary>
    /// 容器内容摘要：把所有条目（除 msixpkg.sig 自身）的名字与内容哈希按规范拼接后再 SHA-256。
    /// 字节级算法必须与 docs/msixpkg-format-v1.md 以及原生实现完全一致。
    /// </summary>
    public static class MsixpkgContentDigest
    {
        public const string DigestAlgorithm = "SHA256";

        public static string ComputeHex(IReadOnlyList<MsixpkgContentDigestEntry> entries)
        {
            return Sha256Util.ToLowerHex(Compute(entries));
        }

        public static byte[] Compute(IReadOnlyList<MsixpkgContentDigestEntry> entries)
        {
            if (entries == null)
            {
                throw new ArgumentNullException(nameof(entries));
            }

            var sorted = new List<MsixpkgContentDigestEntry>(entries);
            sorted.Sort((a, b) => string.CompareOrdinal(a.Name, b.Name));

            for (int i = 1; i < sorted.Count; i++)
            {
                if (string.CompareOrdinal(sorted[i - 1].Name, sorted[i].Name) == 0)
                {
                    throw new MsixpkgFormatException(
                        MsixpkgErrorCodes.EntryDuplicate,
                        "Duplicate entry name while computing the content digest.",
                        sorted[i].Name);
                }
            }

            using (var sha = SHA256.Create())
            using (var buffer = new MemoryStream())
            {
                foreach (MsixpkgContentDigestEntry entry in sorted)
                {
                    byte[] name = Encoding.UTF8.GetBytes(entry.Name);
                    byte[] hex = Encoding.ASCII.GetBytes(entry.Sha256Hex);
                    buffer.Write(name, 0, name.Length);
                    buffer.WriteByte(0x00);
                    buffer.Write(hex, 0, hex.Length);
                    buffer.WriteByte(0x0A);
                }

                buffer.Position = 0L;
                return sha.ComputeHash(buffer);
            }
        }
    }
}
