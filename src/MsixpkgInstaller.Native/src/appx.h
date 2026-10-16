#pragma once

// 内层 .msix 的读取：AppxManifest.xml 的标识信息，以及 AppxSignature.p7x 的签名者证书。
//
// AppxManifest.xml 只用一个定向属性扫描器，不引入 XML 库：我们需要的只有 Identity 的四个属性
// 和 PackageDependency/@Name 列表，而这些都在根元素的固定位置。扫描器会先剥掉 XML 注释，
// 并对元素名做边界检查，避免把 <IdentityFoo 误当成 <Identity。

#include <string>
#include <string_view>
#include <vector>

#include "certs.h"
#include "internal.h"
#include "zip.h"

namespace msixpkg {

struct AppxIdentity {
    std::string name;
    std::string publisher;
    std::string version;
    std::string architecture;
    std::vector<std::string> dependency_names;
};

/// 读取内层 .msix 的 AppxManifest.xml 并解析标识；缺文件或属性缺失抛 E_APPX_*。
AppxIdentity read_appx_identity(const zip::Archive& inner);

/// 读取内层 .msix 的 AppxSignature.p7x 并提取签名者证书。
std::vector<Certificate> read_appx_signer_certificates(const zip::Archive& inner);

/**
 * 内层 AppxManifest 里**第一个** `Application/@Id`。
 *
 * 只用于「启动应用」：AUMID = `<PackageFamilyName>!<ApplicationId>`。
 * 读不到（没有 Application 元素 / 没有 Id 属性 / XML 不完整）返回空串 ——
 * 调用方据此禁用启动按钮，而不是把它当成安装失败。
 * 多 Application 的包只取第一个：主入口是它，其余入口留给将来的「应用列表」。
 */
std::string read_appx_main_application_id(const zip::Archive& inner);

/// Appx 版本归一化成四段（Appx 允许省略尾部 0）。
std::string normalize_appx_version(std::string_view version);

/// Appx 架构写法归一化（amd64 → x64 等）。
std::string normalize_appx_architecture(std::string_view architecture);

}  // namespace msixpkg
