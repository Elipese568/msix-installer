# tests/vectors —— 两端共享的一致性基线

这些文件是 C# 打包器与 C++ 原生安装器**共同读取**的黄金样本。任何格式改动都必须让两端的回归一起红，
强制做出显式决定，而不是悄悄漂移。

| 文件 | 说明 |
|---|---|
| `test-only.pfx` | 测试专用自签名证书。密码：`msixpkg-test-only`。**绝不可用于任何真实分发** |
| `sample-main.msix` | 合成主包：`AppxManifest.xml` + 载荷 + `AppxSignature.p7x`（证书袋形态） |
| `sample-dependency.msix` | 合成依赖包（扮演 `Microsoft.VCLibs.140.00.UWPDesktop`） |
| `Msixpkg.Sample_1.0.0.0_x64.msixpkg` | 黄金容器，由上面三者按规范组装 |
| `real-appxsignature.p7x` | **真实** Appx 签名：从一个真机构建的 WinUI 3 单项目 MSIX 里抽出（1538 字节，`PKCX` 头 + DER SignedData，签名者 `CN=AppPublisher`） |

黄金容器的 SHA-256（审计用）：

```
EA97288615F600731F659E5DF8C44F138759691889C09B3D340A6918F781A986
```

内容摘要（`docs/msixpkg-format-v1.md` §4.1，原生侧可直接比对）：

```
e67cf3483677ed9d4f33b23e2c0ff7e20ca2648124dd1f248772bea13513b4fc
```

签名者指纹（SHA-256，安装器的钉扎白名单值）：

```
9cd702035dd941aa6fe12870550ffc61633281da08839cfb0dfb3a95e1b913b9
```

打包器 CLI 可以用固定时间戳复现出完全相同的字节：

```powershell
src\Msixpkg.Packer\bin\Debug\net9.0\msixpkg.exe pack `
  --msix tests\vectors\sample-main.msix --pfx tests\vectors\test-only.pfx --password msixpkg-test-only `
  --dependencies tests\vectors --output artifacts\smoke\reproduced.msixpkg `
  --created-utc 2026-01-01T00:00:00Z `
  --display-name "Msixpkg Sample" --display-description "Golden vector for the msixpkg v1 format." `
  --display-homepage https://example.invalid/msixpkg
# → 与 tests\vectors\Msixpkg.Sample_1.0.0.0_x64.msixpkg 逐字节相同
```

## 重新生成

普通测试运行**只读**这些文件。重新生成需要显式触发：

```powershell
$env:MSIXPKG_GENERATE_VECTORS = '1'
tests\Msixpkg.Core.Tests\bin\Debug\net9.0\Msixpkg.Core.Tests.exe
```

生成规则：

- `test-only.pfx` / `sample-*.msix` **只在缺失时创建**，之后固定不变（`SignedCms` 的 signingTime
  会让每次签名产出不同字节，因此合成包必须固化为文件，不能在每次测试里重新生成）；
- 黄金容器**每次重新构建**，因此 `RegenerationIsByteIdenticalToCommittedVector` 能检测格式漂移。

## 关于 `AppxSignature.p7x`

两种形态都有夹具，两种都必须支持：

| 形态 | 夹具 | 特征 |
|---|---|---|
| 真实 Appx 签名 | `real-appxsignature.p7x` | 4 字节魔数 `PKCX` + DER `ContentInfo`（OID `1.2.840.113549.1.7.2`） |
| 证书袋 PKCS#7 | `sample-main.msix` 内嵌 | 直接 DER，`X509Certificate2Collection.Import` 能读 |

实测结论：Windows 的证书集合导入路径**读不了真实 Appx 签名**（`CRYPT_E_NOT_FOUND`），
所以两端都必须自己走 DER（C# 见 `src/Msixpkg.Core/Appx/Pkcs7CertificateReader.cs`，
原生用 crypt32）。`RealAppxSignatureTests` 里有一条测试专门断言「`Import` 会失败」，
一旦将来某个运行时版本能读了，它会红并提醒我们简化代码。

## 注意事项

- `.gitattributes` 已把 `*.msixpkg` / `*.msix` / `*.pfx` 标记为 binary，禁止行尾转换与文本 diff。
- 黄金容器使用 `store`（不压缩）写出，条目时间戳固定为 `createdUtc` 的日期，
  所以同一份输入在任意机器、任意 .NET 版本上都应产出完全相同的字节。
