# msixpkg 容器格式 v1（规范性说明）

本文是 `.msixpkg` 的**规范性来源**。C# 打包器（`Msixpkg.Core` / `Msixpkg.Packer`）与 C++ 原生安装器
（`MsixpkgInstaller.Native`）都必须按本文实现；两端行为不一致时以本文为准，并同步修改两端与黄金样本。

- 状态：v1，稳定（`formatVersion = 1`）
- 参考实现：`src/Msixpkg.Core/`（C#，`netstandard2.0`，零第三方依赖）
- 一致性基线：`tests/vectors/`（黄金样本，两端共享）
- 测试：`tests/Msixpkg.Core.Tests/`（103 条）

---

## 1. 总体形状

`.msixpkg` 是一个**标准 ZIP 归档**（不是私有二进制布局），要求：

| 项 | 规定 |
|---|---|
| 容器 | ZIP（`PK\x03\x04`），无加密，无 ZIP64 以外扩展 |
| 条目名 | UTF-8，`/` 分隔，**不含目录条目**（不以 `/` 结尾） |
| 压缩 | 读取端必须支持 `store` 与 `deflate`；**写出端必须使用 `store`** |
| 条目顺序 | `msixpkg.json` 必须是**第一个**条目；其余顺序由写出端决定 |
| 条目时间戳 | 写出端统一使用 `msixpkg.json` 中 `createdUtc` 的**日期**（00:00:00Z），保证字节可复现 |
| 识别方式 | 扩展名 + 根级 `msixpkg.json`。**不设 magic 前缀**（否则标准 ZIP 工具无法打开） |

刻意选择 ZIP 而不是自研二进制布局，理由是：`.msix` 本身就是 ZIP，打包器与安装器都能复用成熟的
压缩库（C# 用 `System.IO.Compression`，C++ 用 vendored `miniz`），排障时可用 7-Zip 直接打开；
同时通过**条目白名单 + 内容摘要签名 + 严格命名规则**保证它仍然是一个受控格式。

---

## 2. 条目白名单

出现白名单以外的条目即拒绝（错误码 `E_ENTRY_UNKNOWN`，**不做前向兼容猜测**）：

```
msixpkg.json                                     必需，根级，恰一个，必须是第一个条目
payload/<arch>/main/<Name>_<Version>_<arch>.msix  必需，恰一个
payload/<arch>/deps/<Name>_<Version>_<arch>.msix  0..n
publisher/publisher.cer                          0..1，DER 编码单证书
publisher/chain.p7b                              0..1，PKCS#7 中间链（预留，v1 未使用）
msixpkg.sig                                      必需（开发构建可缺省，见 §6）
```

- `<arch>` ∈ {`x64`, `x86`, `arm64`, `neutral`}，须与 Appx 清单的 `ProcessorArchitecture` 归一化后一致。
- 依赖包的 `<Name>`/`<Version>` 来自该依赖 `.msix` 自身的包标识，**不是**主包里的 `MinVersion`。
- `payload/<arch>/{main|deps}/` 内的文件名**不参与语义**（唯一真值是 `msixpkg.json` 的声明），
  但必须落在该目录且以 `.msix` 结尾，否则条目名不合法。

### 2.1 条目名合法性（逐条强制）

拒绝以下任一情况（`E_ENTRY_NAME_INVALID`）：

1. 空、超过 512 字符；
2. 以 `/` 开头或结尾；
3. 含 `\`；
4. 含控制字符（`< 0x20` 或 `0x7F`）；
5. 含 `: * ? " < > |`（`: ` 同时堵住 NTFS 数据流 `file:ads`）；
6. 出现空路径段（`a//b`）；
7. 出现 `.` 或 `..` 路径段（堵住 zip-slip）；
8. 任一段以 `.` 结尾（Windows 会静默去掉结尾的点/空格，造成同一路径两种写法）。

大小写：条目名按 **ordinal ignore-case** 判重（`E_ENTRY_DUPLICATE`）。保留名（`msixpkg.json`、
`msixpkg.sig`、`publisher/publisher.cer`、`publisher/chain.p7b`）只接受规定的大小写形式，
`PUBLISHER/PUBLISHER.CER` 之类变体判为 `E_ENTRY_UNKNOWN`。

---

## 3. `msixpkg.json`

UTF-8，无 BOM，成员顺序固定（写出端固定顺序，读取端不依赖顺序），**未知成员一律拒绝**
（`E_MANIFEST_SCHEMA`）——这是把「格式漂移」变成显式失败的手段。

| 成员 | 类型 | 必填 | 说明 |
|---|---|---|---|
| `formatVersion` | 整数 | ✔ | v1 = `1`；更高版本拒绝（`E_FORMAT_VERSION_UNSUPPORTED`） |
| `createdUtc` | 字符串 | ✔ | `yyyy-MM-ddTHH:mm:ssZ`（严格 UTC、秒级、无小数） |
| `identity` | 对象 | ✔ | 见下 |
| `mainPackage` | 对象 | ✔ | `{path, sha256, size}` |
| `dependencies` | 数组 | ✔ | 可为空数组；每项 `{path, name, version, sha256, size}` |
| `publisher` | 对象 | ✖ | 有内嵌证书时必填，见下 |
| `minOsBuild` | 整数 | ✔ | 例如 `19041` |
| `installerMinVersion` | 字符串 | ✔ | 四段版本，例如 `0.1.0`（`Version.TryParse` 语义） |
| `requiresAdmin` | 布尔 | ✔ | 自签名模型恒 `true` |
| `allowAllUsers` | 布尔 | ✔ | 安装器是否**允许** `--all-users`（默认仍按用户安装） |
| `uninstallRemoveCertificate` | 布尔 | ✔ | 打包期默认策略；运行期以状态库为准 |
| `display` | 对象 | ✖ | `{name?, description?, homepage?, iconPng?}`，`iconPng` 为 base64 PNG |

`identity`：`{name, publisher, version, architecture}`

- `name` — Appx `Identity/@Name`
- `publisher` — Appx `Identity/@Publisher`，**DN 原样**，必须与证书 `Subject` 逐字符相等
- `version` — 四段数字，每段 0–65535
- `architecture` — §2 的 `<arch>`

`publisher`：`{certPath, subject, thumbprintSha1, thumbprintSha256, notBeforeUtc, notAfterUtc, selfSigned}`

- `certPath` 必须是字面量 `publisher/publisher.cer`
- 指纹为**小写十六进制**：`thumbprintSha1` 40 字符、`thumbprintSha256` 64 字符
  （Windows 习惯大写显示，存储统一小写；比较一律大小写不敏感）
- `notBeforeUtc` / `notAfterUtc` 与 `createdUtc` 同格式

所有 `sha256` 字段均为小写十六进制 64 字符，`size` 为正整数。

### 3.1 示例

```json
{
  "formatVersion": 1,
  "createdUtc": "2026-01-01T00:00:00Z",
  "identity": {
    "name": "Msixpkg.Sample",
    "publisher": "CN=Msixpkg Test Publisher, O=MsixpkgInstaller, C=CN",
    "version": "1.0.0.0",
    "architecture": "x64"
  },
  "mainPackage": {
    "path": "payload/x64/main/Msixpkg.Sample_1.0.0.0_x64.msix",
    "sha256": "…",
    "size": 2061
  },
  "dependencies": [
    {
      "path": "payload/x64/deps/Microsoft.VCLibs.140.00.UWPDesktop_14.0.33728.0_x64.msix",
      "sha256": "…",
      "size": 1991,
      "name": "Microsoft.VCLibs.140.00.UWPDesktop",
      "version": "14.0.33728.0"
    }
  ],
  "publisher": {
    "certPath": "publisher/publisher.cer",
    "subject": "CN=Msixpkg Test Publisher, O=MsixpkgInstaller, C=CN",
    "thumbprintSha1": "…",
    "thumbprintSha256": "…",
    "notBeforeUtc": "2026-01-01T00:00:00Z",
    "notAfterUtc": "2036-01-01T00:00:00Z",
    "selfSigned": true
  },
  "minOsBuild": 19041,
  "installerMinVersion": "0.1.0",
  "requiresAdmin": true,
  "allowAllUsers": true,
  "uninstallRemoveCertificate": true
}
```

---

## 4. `msixpkg.sig` 与内容摘要

### 4.1 内容摘要（逐字节算法）

```
entries  = 容器内除 msixpkg.sig 以外的所有条目
lines    = 对 entries 按「条目名的 UTF-8 字节序」升序排序后，依次拼接：
               UTF8(entryName) || 0x00 || ASCII(lowercase_hex_sha256(entryContent)) || 0x0A
contentDigest = SHA256(lines)            // 32 字节
```

要点：哈希的是**条目内容（解压后）**，不是 ZIP 里的压缩字节；分隔符是 `0x00`，行尾是 `0x0A`；
排序是 UTF-8 字节序（对 ASCII 条目名即普通序数比较）。条目名不允许重复，故排序稳定。

### 4.2 `msixpkg.sig` 内容

```json
{
  "alg": "RSA-PKCS1-SHA256",
  "keyThumbprintSha256": "…",
  "contentDigest": "…",
  "signature": "…"
}
```

| 成员 | 说明 |
|---|---|
| `alg` | `RSA-PKCS1-SHA256`（v1 写出唯一允许值）/ `RSA-PSS-SHA256` / `ECDSA-P256-SHA256`（仅读） |
| `keyThumbprintSha256` | 签名证书指纹（小写十六进制 64 字符） |
| `contentDigest` | §4.1 结果的小写十六进制 |
| `signature` | 对 `contentDigest` **32 字节原始值**签名后的 base64 |

签名算法细节：RSA 用 SHA-256 + 对应 padding；ECDSA 用 SHA-256、P-256、`r||s` 原始格式（IE 长度为 64 字节）。

**为什么写出只允许 `RSA-PKCS1-SHA256`**：PKCS#1 v1.5 是确定性签名，容器因此**逐字节可复现**，
黄金样本才能做字节级比对，构建产物也可复现。PSS 的盐、ECDSA 的 `k` 都是随机的，会让同一份输入
产出不同字节 —— 读取端仍然必须支持这两种算法（可能来自其它实现的产物）。
若确需 ECDSA 写出，须显式打开 `MsixpkgWriteOptions.AllowNonDeterministicSignature`（默认为关闭）。

### 4.3 验签范围

验签**只做密码学验证，不检查证书链信任**。原因是鸡生蛋：容器由尚未受信任的自签名证书签名，
任何基于链信任的 API（如 `WinVerifyTrust` 的默认策略）都会在导入信任之前失败。

信任由另外两条独立机制建立：

1. **发布者钉扎**：签名者指纹必须命中白名单（`HKLM\SOFTWARE\MsixpkgInstaller\AllowedPublishers`
   或命令行 `--trust-publisher`）；
2. **部署栈校验**：最终由 Windows 在 `AddPackageByUriAsync` 时校验内层 `.msix` 的 Appx 签名。

---

## 5. 安装器校验顺序

按下表顺序执行，任一步失败即中止（同一类错误报第一条）。

| # | 检查 | 失败错误码 |
|---|---|---|
| 1 | 文件存在、非空、≤ 8 GiB | `E_IO` / `E_CONTAINER_EMPTY` / `E_CONTAINER_TOO_LARGE` |
| 2 | 可解析为 ZIP | `E_CONTAINER_INVALID` |
| 3 | 条目数 ≤ 4096 | `E_ENTRY_COUNT_EXCEEDED` |
| 4 | 单条目解压后 ≤ 2 GiB | `E_ENTRY_TOO_LARGE` |
| 5 | 解压总量 ≤ 8 GiB | `E_TOTAL_SIZE_EXCEEDED` |
| 6 | 解压 > 16 MiB 的条目压缩比 ≤ 200 | `E_ENTRY_RATIO_EXCEEDED` |
| 7 | 条目名合法（§2.1） | `E_ENTRY_NAME_INVALID` |
| 8 | 条目名无（忽略大小写）重复 | `E_ENTRY_DUPLICATE` |
| 9 | 条目在白名单内 | `E_ENTRY_UNKNOWN` |

> **关于 CRC-32 与 `E_HASH_MISMATCH` 的关系（跨实现差异，已实测）**：ZIP 条目自带 CRC-32。
> 原生实现会在读取条目时校验它，因此「翻转载荷里的一个字节」在原生侧先以
> `E_CONTAINER_INVALID`（CRC 失配）被拒；而 .NET 的 `System.IO.Compression` 不校验 CRC，
> 同一个文件在 C# 侧会走到第 17 步的清单哈希比对，报 `E_HASH_MISMATCH`。
> 两者都是「完整性失败」，**允许任一路径**；但 GUI 与自动化必须把
> `E_CONTAINER_INVALID` / `E_HASH_MISMATCH` / `E_SIZE_MISMATCH` 一并视为同一类故障。

| 10 | `msixpkg.json` 存在且是第一个条目 | `E_MANIFEST_NOT_FIRST` |
| 11 | 清单 JSON 与 schema 合法（§3） | `E_MANIFEST_SCHEMA` |
| 12 | 条目集合 == 清单声明的集合（不多不少） | `E_ENTRY_UNKNOWN` |
| 13 | `formatVersion == 1` | `E_FORMAT_VERSION_UNSUPPORTED` |
| 14 | `installerMinVersion ≤ 安装器版本` | `E_INSTALLER_TOO_OLD` |
| 15 | `minOsBuild ≤ 本机构建号` | `E_OS_TOO_OLD` |
| 16 | 架构与本机兼容（相同，或 x64 主机 + x86 包） | `E_ARCHITECTURE_MISMATCH` |
| 17 | 每个声明文件存在、大小一致、SHA-256 一致 | `E_FILE_MISSING` / `E_SIZE_MISMATCH` / `E_HASH_MISMATCH` |
| 18 | 证书可解析、指纹与清单一致、Subject == `identity.publisher` | `E_CERTIFICATE_MALFORMED` / `E_PUBLISHER_MISMATCH` |
| 19 | 证书 EKU 含代码签名 `1.3.6.1.5.5.7.3.3`（缺 EKU 扩展 → 警告；有扩展但无此项 → 失败） | `E_CERTIFICATE_NOT_CODE_SIGNING` |
| 20 | 当前时间在证书有效期内（剩余 < 90 天 → 警告） | `E_CERTIFICATE_NOT_YET_VALID` / `E_CERTIFICATE_EXPIRED` |
| 21 | 发布者指纹命中钉扎白名单（配置了白名单或要求钉扎时） | `E_PUBLISHER_NOT_PINNED` |
| 22 | `msixpkg.sig` 存在（要求签名时）且可解析 | `E_SIGNATURE_MISSING` / `E_SIGNATURE_MALFORMED` |
| 23 | 签名内 `contentDigest` == 本地重算值 | `E_SIGNATURE_INVALID` |
| 24 | 签名密钥指纹 == 证书指纹 | `E_SIGNATURE_THUMBPRINT_MISMATCH` |
| 25 | 签名密码学校验通过 | `E_SIGNATURE_INVALID` |
| 26 | 内层 `.msix` 可解析、有 `AppxManifest.xml` | `E_APPX_INVALID` / `E_APPX_MANIFEST_MISSING` |
| 27 | 内层 Name/Publisher/Version/Architecture 与清单一致 | `E_APPX_INVALID` / `E_APPX_PUBLISHER_MISMATCH` / `E_APPX_VERSION_MISMATCH` / `E_APPX_ARCHITECTURE_MISMATCH` |
| 28 | 每个 `dependencies[].name` 在内层清单的 `PackageDependency` 中出现过 | `E_APPX_INVALID` |
| 29 | 内层 `AppxSignature.p7x` 的签名者证书指纹 == 容器内证书指纹 | `E_APPX_SIGNER_MISMATCH` |
| 30 | 若安装器复用了已解包的主包：该文件的大小与 SHA-256 必须与清单一致（防 TOCTOU） | `E_SIZE_MISMATCH` / `E_HASH_MISMATCH` |

> 第 29 步只做**身份提取**：Appx 的 CMS 签名内容是 AppxSip 定义的私有结构，不是普通 detached CMS，
> 因此从这里取「签名者证书」并与容器证书比指纹；真正的内容密码学校验交给 Windows 部署栈。
>
> **真实 `AppxSignature.p7x` 的实测格式**（VS 2026 + Win11 26300 + SDK.BuildTools.MSIX 1.7 产出）：
> 前 4 字节是魔数 `PKCX`（`50 4B 43 58`），其后才是 DER `ContentInfo`（`30 82 …` + OID
> `1.2.840.113549.1.7.2`）。Windows 的 `X509Certificate2Collection.Import` 对它会以
> `CRYPT_E_NOT_FOUND` 失败，**必须自己走 DER**：ContentInfo → signedData → SignedData →
> `[0] certificates` → 逐个 `Certificate`（SEQUENCE）。C# 实现见
> `src/Msixpkg.Core/Appx/Pkcs7CertificateReader.cs`；原生实现用 crypt32 的
> `CryptQueryObject` + `CryptMsgGetParam(CMSG_CERT_PARAM)` 做同一件事。
> 夹具与回归：`tests/vectors/real-appxsignature.p7x`。


---

## 6. 开发构建（未签名容器）

`MsixpkgWriteOptions.Sign = false` 可产出不带 `msixpkg.sig` 的容器，供调试使用。
安装器必须显式传入「不要求签名」才会接受，且**原生实现只能在 `#ifdef MSIXPKG_DEV` 下提供该开关**，
Release 构建不得存在这条路径。

---

## 7. 与初版设计的三处偏差（含理由）

| 项 | 初版 | 落地 | 理由 |
|---|---|---|---|
| 容器写出压缩 | deflate | **store** | 字节可复现，黄金样本可做字节级比对；MSIX 本身已压缩，再压收益极小 |
| 容器签名算法 | RSA-PSS-SHA256 | **RSA-PKCS1-SHA256** | PSS 随机盐破坏可复现性；PKCS#1 v1.5 用于「绑来源身份」场景足够 |
| 版本迭代策略 | 未知成员可忽略 | **未知成员拒绝** | 把格式漂移变成显式失败，而不是静默降级；需要扩展就升 `formatVersion` |

---

## 8. 错误码清单

`E_CONTAINER_INVALID` `E_CONTAINER_EMPTY` `E_CONTAINER_TOO_LARGE` `E_ENTRY_COUNT_EXCEEDED`
`E_ENTRY_TOO_LARGE` `E_ENTRY_RATIO_EXCEEDED` `E_TOTAL_SIZE_EXCEEDED` `E_ENTRY_NAME_INVALID`
`E_ENTRY_DUPLICATE` `E_ENTRY_UNKNOWN` `E_MANIFEST_MISSING` `E_MANIFEST_NOT_FIRST` `E_MANIFEST_SCHEMA`
`E_FORMAT_VERSION_UNSUPPORTED` `E_INSTALLER_TOO_OLD` `E_OS_TOO_OLD` `E_ARCHITECTURE_MISMATCH`
`E_FILE_MISSING` `E_HASH_MISMATCH` `E_SIZE_MISMATCH` `E_SIGNATURE_MISSING` `E_SIGNATURE_MALFORMED`
`E_SIGNATURE_ALGORITHM_UNSUPPORTED` `E_SIGNATURE_INVALID` `E_SIGNATURE_THUMBPRINT_MISMATCH`
`E_CERTIFICATE_MISSING` `E_CERTIFICATE_MALFORMED` `E_CERTIFICATE_EXPIRED` `E_CERTIFICATE_NOT_YET_VALID`
`E_CERTIFICATE_NOT_CODE_SIGNING` `E_PUBLISHER_MISMATCH` `E_PUBLISHER_NOT_PINNED` `E_APPX_INVALID`
`E_APPX_MANIFEST_MISSING` `E_APPX_SIGNATURE_MISSING` `E_APPX_SIGNER_MISMATCH` `E_APPX_PUBLISHER_MISMATCH`
`E_APPX_VERSION_MISMATCH` `E_APPX_ARCHITECTURE_MISMATCH` `E_IO`

原生实现必须使用**完全相同的字符串**，不得本地化错误码本身（本地化只针对展示文案）。
