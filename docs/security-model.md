# 安全模型

`.msixpkg` 的核心便利也是它唯一的真实风险：**安装器会把容器内嵌的证书写进受信任存储**。
这等价于把「代码签名信任」的发放权交给了文件本身。本文记录威胁、防护与审计面。

## 1. 威胁与防护

| 威胁 | 防护 |
|---|---|
| 攻击者自造 `.msixpkg` 夹带自己的根证书，从而获得任意代码的签名信任 | **发布者指纹钉扎**：`HKLM\SOFTWARE\MsixpkgInstaller\AllowedPublishers` 存允许的 SHA-256 指纹白名单（管理员预先部署）。未命中 → 默认拒绝；仅 `--trust-publisher <thumbprint>` + 同意 UI 才 TOFU，并写审计日志 |
| 偷换载荷：容器签名者、内嵌证书、实际 `.msix` 签名者三者不一致 | **四重身份交叉校验**：容器签名者 == `publisher.cer` 指纹 == 内层 `AppxSignature.p7x` 签名者指纹 == Appx 清单 `Publisher` |
| 只验容器签名是否足够 | 不够也不必要：容器签名只用于「在导入信任之前确认来源身份」；**真正的内容密码学校验由 Windows 部署栈在 `AddPackageByUriAsync` 时完成** |
| 信任范围过大 | 只写 `LocalMachine\TrustedPeople`，**永不写 `Root`**；只写 `LocalMachine`（部署栈不认用户级存储） |
| TOCTOU：校验通过后、安装之前替换暂存文件 | 暂存目录 `%ProgramData%\Msixpkg\staging\<random>\`，ACL 仅 Administrators + SYSTEM；若校验复用了已解包的主包，**必须重新核对大小与 SHA-256**（`E_SIZE_MISMATCH` / `E_HASH_MISMATCH`）；Windows 11 build 23504+ 再叠加 `AddPackageOptions.ExpectedDigests` |
| ZIP 炸弹 / 路径穿越 / ADS / 大小写重复 | 格式规范 §2.1、§5 的条目白名单与限额，拒绝优先于容错 |
| 卸载误删他人信任的证书 | 引用计数 + Publisher 比对 + `certExistedBefore` 判定（见 §3） |
| 开发期后门 | `--allow-unsigned` 只在 `#ifdef MSIXPKG_DEV` 下编译进 Debug 配置，Release 构建不存在这条路径 |

## 2. 为什么容器签名不查链信任

鸡生蛋：容器由**尚未受信任**的自签名证书签名，任何基于链信任的 API（如 `WinVerifyTrust` 的默认策略）
都会在「把证书导入信任」之前失败。因此：

1. 验签只做**密码学验证**（`BCryptVerifySignature` / `RSA.VerifyHash`），公钥取自容器内嵌证书；
2. 信任由两条独立机制建立：**发布者钉扎** + **Windows 部署栈对 Appx 签名的校验**；
3. 因此容器签名的算法不必是 PSS —— 选 PKCS#1 v1.5 还有一个额外好处：签名确定性，
   容器字节可复现，黄金样本能做字节级比对（见格式规范 §4.2）。

## 3. 证书回收（卸载）

用户已确认的策略：**仅当「本次导入且无其他包依赖」时移除**。四条判定全满足才删：

| 条件 | 含义 |
|---|---|
| `certAddedByInstaller == true` | 证书是本次安装导入的，不是机器上本来就有的 |
| 无其它 `installId` 引用同一指纹 | 同一个证书可能被多个 `.msixpkg` 共用 |
| 无其它已注册包的 `Publisher` == 该证书 `Subject` | 别把别人正在用的信任锚删掉 |
| 策略允许且已获同意 | 或安装时用了 `--quiet` 且策略中已记录该决定 |

`certExistedBefore == true` 时**永不删除** —— 我们无权处置别人放进去的信任。

## 4. 需要记录进审计日志的字段

每次安装/卸载/信任变更都要能回答：「谁在什么时候、因为什么、把哪个指纹写进了哪个存储」。

- 容器路径 + 容器 SHA-256 + 内容摘要
- 容器签名算法 + 签名者 SHA-256 指纹
- 内层 `.msix` 的 Identity（Name/Version/Architecture/Publisher）+ 主包 SHA-256
- 证书 Subject、SHA-1/SHA-256 指纹、有效期、`selfSigned`
- 目标存储名（恒为 `TrustedPeople`）+ 操作（add / 已在 / delete / 跳过删除 + 跳过原因）
- 钉扎判定结果（命中白名单 / TOFU / 拒绝）
- 提权路径（UAC 是否被取消）、退出码、每个 HRESULT
- 操作者 SID（若可获取）与时间戳

## 5. 分发建议

| 场景 | 建议 |
|---|---|
| 内部/测试分发 | 自签名证书 + 钉扎（本文方案）。注意证书过期会导致全部客户机装不上 |
| 公网分发 | 受信 CA 或 Azure Artifact Signing。容器**不内嵌证书**，安装器完全不碰证书存储，安装/卸载流程与本文其余部分相同 |
| 微软商店 | 由商店签名，不使用本方案 |

`MsixpkgWriteOptions.IncludePublisherCertificate = false`（或 MSBuild 的
`MsixpkgIncludeCertificate=false`）即可产出「无证书条目」的容器；格式已按证书可选设计，
安装器会跳过全部证书逻辑并给出警告。
