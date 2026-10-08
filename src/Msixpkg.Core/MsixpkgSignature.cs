using System;
using System.Globalization;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;

namespace Msixpkg.Core
{
    public static class MsixpkgSignatureAlgorithms
    {
        /// <summary>v1 写出必须使用该算法：PKCS#1 v1.5 签名是确定性的，容器字节因此可逐字节复现。</summary>
        public const string RsaPkcs1Sha256 = "RSA-PKCS1-SHA256";

        /// <summary>读支持。PSS 的盐随机，写出会产生不可复现的容器。</summary>
        public const string RsaPssSha256 = "RSA-PSS-SHA256";

        /// <summary>读支持。ECDSA 的 k 随机，写出会产生不可复现的容器。</summary>
        public const string EcdsaP256Sha256 = "ECDSA-P256-SHA256";

        public static bool IsSupported(string algorithm)
        {
            return string.Equals(algorithm, RsaPkcs1Sha256, StringComparison.Ordinal)
                || string.Equals(algorithm, RsaPssSha256, StringComparison.Ordinal)
                || string.Equals(algorithm, EcdsaP256Sha256, StringComparison.Ordinal);
        }

        /// <summary>该算法是否能产生逐字节可复现的签名。</summary>
        public static bool IsDeterministic(string algorithm)
        {
            return string.Equals(algorithm, RsaPkcs1Sha256, StringComparison.Ordinal);
        }
    }

    /// <summary>msixpkg.sig 的模型：对容器内容摘要做密码学签名，验签不涉及证书链信任。</summary>
    public sealed class MsixpkgSignature
    {
        public MsixpkgSignature(string algorithm, string keyThumbprintSha256, string contentDigestHex, string signatureBase64)
        {
            Algorithm = algorithm ?? throw new ArgumentNullException(nameof(algorithm));
            KeyThumbprintSha256 = keyThumbprintSha256 ?? throw new ArgumentNullException(nameof(keyThumbprintSha256));
            ContentDigestHex = contentDigestHex ?? throw new ArgumentNullException(nameof(contentDigestHex));
            SignatureBase64 = signatureBase64 ?? throw new ArgumentNullException(nameof(signatureBase64));
        }

        public string Algorithm { get; }

        public string KeyThumbprintSha256 { get; }

        public string ContentDigestHex { get; }

        public string SignatureBase64 { get; }
    }

    public static class MsixpkgSigner
    {
        public static string SelectAlgorithm(X509Certificate2 certificate, bool allowNonDeterministic = false)
        {
            if (certificate == null)
            {
                throw new ArgumentNullException(nameof(certificate));
            }

            using (RSA? rsa = certificate.GetRSAPrivateKey())
            {
                if (rsa != null)
                {
                    return MsixpkgSignatureAlgorithms.RsaPkcs1Sha256;
                }
            }

            using (ECDsa? ecdsa = certificate.GetECDsaPrivateKey())
            {
                if (ecdsa != null)
                {
                    if (ecdsa.KeySize != 256)
                    {
                        throw new MsixpkgFormatException(
                            MsixpkgErrorCodes.SignatureAlgorithmUnsupported,
                            "Only P-256 ECDSA keys are supported for container signing.",
                            "KeySize=" + ecdsa.KeySize.ToString(CultureInfo.InvariantCulture));
                    }

                    if (!allowNonDeterministic)
                    {
                        throw new MsixpkgFormatException(
                            MsixpkgErrorCodes.SignatureAlgorithmUnsupported,
                            "ECDSA container signatures are not byte-reproducible; use an RSA signing certificate, or opt in explicitly.",
                            MsixpkgSignatureAlgorithms.EcdsaP256Sha256);
                    }

                    return MsixpkgSignatureAlgorithms.EcdsaP256Sha256;
                }
            }

            throw new MsixpkgFormatException(
                MsixpkgErrorCodes.SignatureAlgorithmUnsupported,
                "The signing certificate has no RSA or ECDSA private key.");
        }

        public static MsixpkgSignature Sign(byte[] contentDigest, X509Certificate2 certificate, bool allowNonDeterministic = false)
        {
            if (contentDigest == null || contentDigest.Length != 32)
            {
                throw new ArgumentException("The content digest must be a 32-byte SHA-256 digest.", nameof(contentDigest));
            }

            string algorithm = SelectAlgorithm(certificate, allowNonDeterministic);
            byte[] signature;
            if (string.Equals(algorithm, MsixpkgSignatureAlgorithms.RsaPkcs1Sha256, StringComparison.Ordinal)
                || string.Equals(algorithm, MsixpkgSignatureAlgorithms.RsaPssSha256, StringComparison.Ordinal))
            {
                using RSA rsa = certificate.GetRSAPrivateKey()!;
                RSASignaturePadding padding = string.Equals(algorithm, MsixpkgSignatureAlgorithms.RsaPssSha256, StringComparison.Ordinal)
                    ? RSASignaturePadding.Pss
                    : RSASignaturePadding.Pkcs1;
                signature = rsa.SignHash(contentDigest, HashAlgorithmName.SHA256, padding);
            }
            else
            {
                using ECDsa ecdsa = certificate.GetECDsaPrivateKey()!;
                signature = ecdsa.SignHash(contentDigest);
            }

            return new MsixpkgSignature(
                algorithm,
                Thumbprints.Sha256Hex(certificate),
                Sha256Util.ToLowerHex(contentDigest),
                Convert.ToBase64String(signature));
        }

        /// <summary>用证书公钥做纯密码学验签（不检查链与信任）。失败抛 E_SIGNATURE_INVALID。</summary>
        public static void Verify(byte[] contentDigest, MsixpkgSignature signature, X509Certificate2 certificate)
        {
            if (contentDigest == null || contentDigest.Length != 32)
            {
                throw new ArgumentException("The content digest must be a 32-byte SHA-256 digest.", nameof(contentDigest));
            }

            if (signature == null)
            {
                throw new ArgumentNullException(nameof(signature));
            }

            if (!MsixpkgSignatureAlgorithms.IsSupported(signature.Algorithm))
            {
                throw new MsixpkgFormatException(
                    MsixpkgErrorCodes.SignatureAlgorithmUnsupported,
                    "Unsupported container signature algorithm.",
                    signature.Algorithm);
            }

            byte[] raw;
            try
            {
                raw = Convert.FromBase64String(signature.SignatureBase64);
            }
            catch (FormatException ex)
            {
                throw new MsixpkgFormatException(MsixpkgErrorCodes.SignatureMalformed, "The container signature is not valid base64.", ex.Message);
            }

            bool valid;
            if (string.Equals(signature.Algorithm, MsixpkgSignatureAlgorithms.RsaPkcs1Sha256, StringComparison.Ordinal)
                || string.Equals(signature.Algorithm, MsixpkgSignatureAlgorithms.RsaPssSha256, StringComparison.Ordinal))
            {
                using RSA? rsa = certificate.GetRSAPublicKey();
                if (rsa == null)
                {
                    throw new MsixpkgFormatException(MsixpkgErrorCodes.SignatureInvalid, "The certificate has no RSA public key.");
                }

                RSASignaturePadding padding = string.Equals(signature.Algorithm, MsixpkgSignatureAlgorithms.RsaPssSha256, StringComparison.Ordinal)
                    ? RSASignaturePadding.Pss
                    : RSASignaturePadding.Pkcs1;
                valid = rsa.VerifyHash(contentDigest, raw, HashAlgorithmName.SHA256, padding);
            }
            else
            {
                using ECDsa? ecdsa = certificate.GetECDsaPublicKey();
                if (ecdsa == null)
                {
                    throw new MsixpkgFormatException(MsixpkgErrorCodes.SignatureInvalid, "The certificate has no ECDSA public key.");
                }

                valid = ecdsa.VerifyHash(contentDigest, raw);
            }

            if (!valid)
            {
                throw new MsixpkgFormatException(
                    MsixpkgErrorCodes.SignatureInvalid,
                    "The container signature does not match its content digest.");
            }
        }
    }

    /// <summary>msixpkg.sig 的 JSON 编解码（字段集合是契约，未知成员一律拒绝）。</summary>
    public static class MsixpkgSignatureCodec
    {
        private static readonly string[] KnownMembers =
        {
            "alg",
            "keyThumbprintSha256",
            "contentDigest",
            "signature",
        };

        public static string Write(MsixpkgSignature signature)
        {
            if (signature == null)
            {
                throw new ArgumentNullException(nameof(signature));
            }

            var value = Json.JsonValue.NewObject();
            value.Add("alg", Json.JsonValue.Of(signature.Algorithm));
            value.Add("keyThumbprintSha256", Json.JsonValue.Of(signature.KeyThumbprintSha256));
            value.Add("contentDigest", Json.JsonValue.Of(signature.ContentDigestHex));
            value.Add("signature", Json.JsonValue.Of(signature.SignatureBase64));
            return value.ToJson();
        }

        public static MsixpkgSignature Parse(string json)
        {
            Json.JsonValue root;
            try
            {
                root = Json.JsonParser.Parse(json);
            }
            catch (Json.JsonFormatException ex)
            {
                throw new MsixpkgFormatException(MsixpkgErrorCodes.SignatureMalformed, "msixpkg.sig is not valid JSON.", ex.Message);
            }

            if (root.Kind != Json.JsonKind.Object)
            {
                throw new MsixpkgFormatException(MsixpkgErrorCodes.SignatureMalformed, "msixpkg.sig must be a JSON object.");
            }

            foreach (var member in root.Members)
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
                        MsixpkgErrorCodes.SignatureMalformed,
                        "msixpkg.sig contains an unknown member.",
                        member.Key);
                }
            }

            string algorithm = ReadString(root, "alg");
            if (!MsixpkgSignatureAlgorithms.IsSupported(algorithm))
            {
                throw new MsixpkgFormatException(
                    MsixpkgErrorCodes.SignatureAlgorithmUnsupported,
                    "Unsupported container signature algorithm.",
                    algorithm);
            }

            string thumbprint = ReadString(root, "keyThumbprintSha256");
            if (!MsixpkgManifest.IsHex(thumbprint, 64))
            {
                throw new MsixpkgFormatException(MsixpkgErrorCodes.SignatureMalformed, "'keyThumbprintSha256' must be 64 lowercase hex characters.");
            }

            string digest = ReadString(root, "contentDigest");
            if (!MsixpkgManifest.IsHex(digest, 64))
            {
                throw new MsixpkgFormatException(MsixpkgErrorCodes.SignatureMalformed, "'contentDigest' must be 64 lowercase hex characters.");
            }

            return new MsixpkgSignature(algorithm, thumbprint, digest, ReadString(root, "signature"));
        }

        private static string ReadString(Json.JsonValue parent, string name)
        {
            if (!parent.TryGet(name, out Json.JsonValue value) || value.Kind != Json.JsonKind.String || value.StringValue.Length == 0)
            {
                throw new MsixpkgFormatException(MsixpkgErrorCodes.SignatureMalformed, "'" + name + "' must be a non-empty string.");
            }

            return value.StringValue;
        }
    }

    public static class Thumbprints
    {
        /// <summary>小写十六进制 SHA-1 指纹。</summary>
        public static string Sha1Hex(X509Certificate2 certificate)
        {
            if (certificate == null)
            {
                throw new ArgumentNullException(nameof(certificate));
            }

            using (var sha1 = SHA1.Create())
            {
                return Sha256Util.ToLowerHex(sha1.ComputeHash(certificate.RawData));
            }
        }

        /// <summary>小写十六进制 SHA-256 指纹（钉扎值）。</summary>
        public static string Sha256Hex(X509Certificate2 certificate)
        {
            if (certificate == null)
            {
                throw new ArgumentNullException(nameof(certificate));
            }

            return Sha256Util.HashBytesHex(certificate.RawData);
        }
    }
}
