using System;
using System.Collections.Generic;
using System.Globalization;
using System.Security.Cryptography.X509Certificates;

namespace Msixpkg.Core.Appx
{
    /// <summary>
    /// 从真实 Appx 签名（AppxSignature.p7x）里提取签名者证书。
    ///
    /// 为什么不能用 <c>X509Certificate2Collection.Import</c>：真实的 AppxSignature.p7x 是
    /// AppxSip 定义的 SignedData，其 encapContentInfo 指向自定义摘要结构（AXPC/AXCD/AXCI/AXCT），
    /// Windows 的证书集合导入路径会以 CRYPT_E_NOT_FOUND 失败（实测于 VS 2026 + Win11 26300 产出的 MSIX）。
    /// 因此这里自己走一遍 DER：ContentInfo → signedData → SignedData → [0] certificates → 逐个 Certificate。
    ///
    /// 刻意不引入 System.Security.Cryptography.Pkcs：那会给 MSBuild 的 .NET Framework 宿主带来
    /// 绑定重定向风险，而 C++ 侧本来也要用 crypt32 做同一件事。
    ///
    /// 注意：这里只做**身份提取**，不验证密码学签名 —— Appx 签名的密码学校验交给 Windows 部署栈。
    /// </summary>
    public static class Pkcs7CertificateReader
    {
        private const byte TagInteger = 0x02;
        private const byte TagSequence = 0x30;
        private const byte TagSet = 0x31;
        private const byte TagOid = 0x06;

        /// <summary>1.2.840.113549.1.7.2 —— PKCS#7 signedData。</summary>
        private static readonly byte[] SignedDataOid =
        {
            0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x07, 0x02,
        };

        /// <summary>
        /// 真实 AppxSignature.p7x 的头部是 4 字节魔数 "PKCX"（实测于 VS 2026 + Win11 26300 产出的 MSIX：
        /// 50 4B 43 58 30 82 05 FA 06 09 2A 86 48 86 F7 0D 01 07 02 …），其后才是 DER ContentInfo。
        /// 也有产物直接就是 DER（本仓库的合成夹具即如此），因此这里做一次有界的头部搜索：
        /// 只在开头 16 字节内找第一个「能解析成 SEQUENCE 且 OID 是 signedData」的偏移，避免任何猜测。
        /// </summary>
        private const int MaxHeaderLength = 16;

        public static IReadOnlyList<X509Certificate2> ExtractCertificates(byte[] p7x)
        {
            if (p7x == null || p7x.Length == 0)
            {
                throw new MsixpkgFormatException(MsixpkgErrorCodes.AppxSignatureMissing, "AppxSignature.p7x is empty.");
            }

            var certificates = new List<X509Certificate2>();

            int headerLength = FindContentInfoOffset(p7x);

            // ContentInfo ::= SEQUENCE { contentType OID, [0] EXPLICIT SignedData }
            DerElement contentInfo = DerElement.Read(p7x, headerLength);

            int offset = contentInfo.ContentOffset;
            if (contentInfo.Tag != TagSequence)
            {
                throw Malformed("AppxSignature.p7x does not start with a ContentInfo SEQUENCE.");
            }
            DerElement oid = DerElement.Read(p7x, offset);
            if (oid.Tag != TagOid || !Matches(p7x, oid, SignedDataOid))
            {
                throw Malformed("AppxSignature.p7x is not a PKCS#7 signedData structure.");
            }

            offset = oid.End;
            DerElement explicitSignedData = DerElement.Read(p7x, offset);
            if (explicitSignedData.Tag != 0xA0)
            {
                throw Malformed("PKCS#7 signedData is not wrapped in an explicit [0] tag.");
            }

            DerElement signedData = DerElement.Read(p7x, explicitSignedData.ContentOffset);
            if (signedData.Tag != TagSequence)
            {
                throw Malformed("PKCS#7 SignedData is not a SEQUENCE.");
            }

            int cursor = signedData.ContentOffset;

            // version INTEGER
            DerElement version = DerElement.Read(p7x, cursor);
            if (version.Tag != TagInteger)
            {
                throw Malformed("PKCS#7 SignedData has no version.");
            }

            cursor = version.End;

            // digestAlgorithms SET
            DerElement digestAlgorithms = DerElement.Read(p7x, cursor);
            if (digestAlgorithms.Tag != TagSet)
            {
                throw Malformed("PKCS#7 SignedData has no digestAlgorithms set.");
            }

            cursor = digestAlgorithms.End;

            // encapContentInfo SEQUENCE（Appx 的 eContentType 是自定义 OID，内容可能为空 —— 不解析它）
            DerElement encapContent = DerElement.Read(p7x, cursor);
            if (encapContent.Tag != TagSequence)
            {
                throw Malformed("PKCS#7 SignedData has no encapContentInfo.");
            }

            cursor = encapContent.End;

            // certificates [0] IMPLICIT CertificateSet（可选）
            while (cursor < signedData.End)
            {
                DerElement element = DerElement.Read(p7x, cursor);
                if (element.Tag == 0xA0)
                {
                    CollectCertificates(p7x, element, certificates);
                }

                cursor = element.End;
            }

            if (certificates.Count == 0)
            {
                throw Malformed("AppxSignature.p7x contains no certificate.");
            }

            return certificates;
        }

        private static int FindContentInfoOffset(byte[] p7x)
        {
            int limit = Math.Min(MaxHeaderLength, Math.Max(0, p7x.Length - 2));
            for (int candidate = 0; candidate <= limit; candidate++)
            {
                if (p7x[candidate] != TagSequence)
                {
                    continue;
                }

                try
                {
                    DerElement contentInfo = DerElement.Read(p7x, candidate);
                    DerElement oid = DerElement.Read(p7x, contentInfo.ContentOffset);
                    if (oid.Tag == TagOid && Matches(p7x, oid, SignedDataOid))
                    {
                        return candidate;
                    }
                }
                catch (MsixpkgFormatException)
                {
                    // 这个偏移不是 ContentInfo，继续往后找。
                }
            }

            throw Malformed(
                "No PKCS#7 signedData ContentInfo found within the first "
                + MaxHeaderLength.ToString(CultureInfo.InvariantCulture)
                + " bytes; first bytes are " + ToHexPreview(p7x));
        }

        private static string ToHexPreview(byte[] data)
        {
            int count = Math.Min(8, data.Length);
            var builder = new System.Text.StringBuilder(count * 3);
            for (int i = 0; i < count; i++)
            {
                if (i > 0)
                {
                    builder.Append(' ');
                }

                builder.Append(data[i].ToString("x2", CultureInfo.InvariantCulture));
            }

            return builder.ToString();
        }

        private static void CollectCertificates(byte[] data, DerElement certificateSet, List<X509Certificate2> certificates)
        {
            int cursor = certificateSet.ContentOffset;
            while (cursor < certificateSet.End)
            {
                DerElement element = DerElement.Read(data, cursor);

                // Certificate ::= SEQUENCE；其它的（extendedCertificate [1]、[2]、[3]）跳过。
                if (element.Tag == TagSequence)
                {
                    byte[] raw = new byte[element.End - element.Start];
                    Buffer.BlockCopy(data, element.Start, raw, 0, raw.Length);
                    certificates.Add(new X509Certificate2(raw));
                }

                cursor = element.End;
            }
        }

        private static bool Matches(byte[] data, DerElement element, byte[] expected)
        {
            int length = element.End - element.ContentOffset;
            if (length != expected.Length)
            {
                return false;
            }

            int start = element.ContentOffset;
            for (int i = 0; i < expected.Length; i++)
            {
                if (data[start + i] != expected[i])
                {
                    return false;
                }
            }

            return true;
        }

        private static MsixpkgFormatException Malformed(string detail)
        {
            return new MsixpkgFormatException(
                MsixpkgErrorCodes.AppxInvalid,
                "AppxSignature.p7x could not be parsed as a PKCS#7 signedData structure.",
                detail);
        }

        /// <summary>最小 DER 元素读取：只支持 definite length，拒绝不确定长度与超范围偏移。</summary>
        private readonly struct DerElement
        {
            private DerElement(byte tag, int start, int contentOffset, int end)
            {
                Tag = tag;
                Start = start;
                ContentOffset = contentOffset;
                End = end;
            }

            public byte Tag { get; }

            /// <summary>TLV 的起始偏移。</summary>
            public int Start { get; }

            /// <summary>内容起始偏移。</summary>
            public int ContentOffset { get; }

            /// <summary>TLV 结束偏移（含）。</summary>
            public int End { get; }

            public static DerElement Read(byte[] data, int offset)
            {
                if (offset + 2 > data.Length)
                {
                    throw Malformed("DER element is truncated.");
                }

                byte tag = data[offset];
                int cursor = offset + 1;
                int firstLengthByte = data[cursor++];
                int length;

                if ((firstLengthByte & 0x80) == 0)
                {
                    length = firstLengthByte;
                }
                else
                {
                    int lengthBytes = firstLengthByte & 0x7F;
                    if (lengthBytes == 0)
                    {
                        throw Malformed("Indefinite-length DER is not supported.");
                    }

                    if (lengthBytes > 4 || cursor + lengthBytes > data.Length)
                    {
                        throw Malformed("DER length field is invalid.");
                    }

                    length = 0;
                    for (int i = 0; i < lengthBytes; i++)
                    {
                        length = (length << 8) | data[cursor++];
                    }

                    if (length < 0)
                    {
                        throw Malformed("DER length overflows.");
                    }
                }

                long end = (long)cursor + length;
                if (end > data.Length)
                {
                    throw Malformed(
                        "DER element claims " + length.ToString(CultureInfo.InvariantCulture)
                        + " bytes but only " + (data.Length - cursor).ToString(CultureInfo.InvariantCulture) + " remain.");
                }

                return new DerElement(tag, offset, cursor, (int)end);
            }
        }
    }
}
