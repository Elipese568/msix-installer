using System;
using System.IO;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;

namespace Msixpkg.Core.Packing
{
    /// <summary>
    /// 统一的证书加载入口。存在的理由很具体：
    /// 1) netstandard2.0 的引用程序集没有 <c>X509CertificateLoader</c>，而 .NET 9 上旧的
    ///    <c>X509Certificate2(byte[], string, X509KeyStorageFlags)</c> 路径已被标记过时，
    ///    且对标志组合的校验更严（<c>Exportable | EphemeralKeySet</c> 直接抛 ArgumentException）。
    /// 2) 不同的运行时/受约束环境对具体标志的支持不一致（例如 EphemeralKeySet 可能因访问被拒而失败），
    ///    而标志只影响私钥容器是否落盘，不影响签名正确性，因此按候选顺序尝试是合理策略。
    /// </summary>
    public static class MsixpkgCertificates
    {
        /// <summary>
        /// netstandard2.0 的引用程序集不含该枚举成员，但它的值在所有支持它的运行时上都固定为 0x20
        /// （.NET Core 2.1+ / .NET Framework 4.7.2+）。注意不是 0x2000 —— 用错值会被
        /// X509CertificateLoader 的严格校验直接拒绝（ArgumentException: Value of flags is invalid）。
        /// </summary>
        private const int EphemeralKeySetValue = 0x20;

        private static readonly X509KeyStorageFlags[] Candidates =
        {
            // 组合优先：这是本机验证过的、唯一既能不落盘持久化私钥容器、又能真正使用私钥的形态。
            // 其余组合在受约束环境里会加载成功但私钥不可用（表现为签名时报「密钥集不存在」）。
            (X509KeyStorageFlags)EphemeralKeySetValue | X509KeyStorageFlags.Exportable,
            (X509KeyStorageFlags)EphemeralKeySetValue,
            X509KeyStorageFlags.Exportable,
            X509KeyStorageFlags.DefaultKeySet,
        };

        public static X509Certificate2 LoadSigningCertificateFromFile(string pfxPath, string? password)
        {
            if (string.IsNullOrEmpty(pfxPath))
            {
                throw new MsixpkgFormatException(MsixpkgErrorCodes.IoError, "A PFX path is required.");
            }

            byte[] pfx;
            try
            {
                pfx = File.ReadAllBytes(pfxPath);
            }
            catch (IOException ex)
            {
                throw new MsixpkgFormatException(MsixpkgErrorCodes.IoError, "The signing certificate (.pfx) could not be read.", ex.Message);
            }

            return LoadSigningCertificate(pfx, password);
        }

        public static X509Certificate2 LoadSigningCertificate(byte[] pfx, string? password)
        {
            if (pfx == null || pfx.Length == 0)
            {
                throw new MsixpkgFormatException(MsixpkgErrorCodes.CertificateMalformed, "The signing certificate (.pfx) is empty.");
            }

            CryptographicException? firstFailure = null;

            foreach (X509KeyStorageFlags flags in Candidates)
            {
                X509Certificate2? candidate = null;
                try
                {
                    candidate = Create(pfx, password, flags);

                    // 关键：某些标志组合能“加载成功”却得到一个无法使用的私钥
                    // （表现为后续签名时报「密钥集不存在」）。必须在这里就探测私钥是否真的可用，
                    // 否则错误会延迟到签名阶段，且信息完全无法指向证书加载方式。
                    if (HasUsablePrivateKey(candidate))
                    {
                        return candidate;
                    }

                    firstFailure ??= new CryptographicException("The private key of the loaded certificate is not usable.");
                    candidate.Dispose();
                    candidate = null;
                }
                catch (ArgumentException)
                {
                    // 该运行时拒绝这组标志，试下一个。
                    candidate?.Dispose();
                }
                catch (CryptographicException ex)
                {
                    // 可能是密码错误，也可能只是这组标志在当前环境不可用；保留首个失败原因后继续尝试。
                    firstFailure ??= ex;
                    candidate?.Dispose();
                }
            }

            throw new MsixpkgFormatException(
                MsixpkgErrorCodes.CertificateMalformed,
                "The signing certificate (.pfx) could not be loaded, or its private key is not usable in this environment. Check the password.",
                firstFailure?.Message ?? string.Empty);
        }

        private static bool HasUsablePrivateKey(X509Certificate2 certificate)
        {
            if (!certificate.HasPrivateKey)
            {
                return false;
            }

            try
            {
                using RSA? rsa = certificate.GetRSAPrivateKey();
                if (rsa != null)
                {
                    return true;
                }

                using ECDsa? ecdsa = certificate.GetECDsaPrivateKey();
                return ecdsa != null;
            }
            catch (CryptographicException)
            {
                return false;
            }
        }

        private static X509Certificate2 Create(byte[] pfx, string? password, X509KeyStorageFlags flags)
        {
#if NET9_0_OR_GREATER
            return X509CertificateLoader.LoadPkcs12(pfx, password, flags);
#else
#pragma warning disable SYSLIB0057
            return new X509Certificate2(pfx, password, flags);
#pragma warning restore SYSLIB0057
#endif
        }

        /// <summary>读取不含私钥的 DER 公钥证书（容器内 publisher.cer 的形态）。</summary>
        public static X509Certificate2 LoadPublicCertificate(byte[] der)
        {
            if (der == null || der.Length == 0)
            {
                throw new MsixpkgFormatException(MsixpkgErrorCodes.CertificateMalformed, "The certificate data is empty.");
            }

            try
            {
                return new X509Certificate2(der);
            }
            catch (CryptographicException ex)
            {
                throw new MsixpkgFormatException(MsixpkgErrorCodes.CertificateMalformed, "The certificate could not be parsed.", ex.Message);
            }
        }
    }
}
