#pragma once

// 「设置 → 应用」卸载入口（docs/installer-design.md §9）。
//
// 为什么必须有：用户在系统设置里卸载 MSIX 包会**绕过我们的卸载器**，
// 于是证书永远留在受信任区，直接破坏「按引用计数回收证书」这条策略。
// 有了 ARP 条目，设置里出现的是我们的条目，卸载会走 `uninstall --family-name <PFN>`。
//
// 位置：
//   HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\Msixpkg.<IdentityName>  （需管理员）
//   HKCU\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\Msixpkg.<IdentityName>  （未提权回落）
// 未提权时「装给当前用户」也要能出现在设置里，所以两处都要试；
// 副作用是系统设置里会同时出现 MSIX 条目与我们的条目，用 DisplayName 后缀区分。

#include <string>
#include <string_view>

namespace msixpkg {

struct ArpEntry {
    std::string key_name;          // Msixpkg.<IdentityName>
    std::string display_name;
    std::string display_icon;
    std::string display_version;
    std::string publisher;
    std::string uninstall_string;
    std::string install_location;  // 暂存/安装目录（可选）
};

/// 组装 ARP 条目内容（纯函数，可单测）。
/// `display_name_suffix` 用于和系统设置里 MSIX 自带的那条区分开。
ArpEntry make_arp_entry(
    std::string_view identity_name,
    std::string_view display_name,
    std::string_view version,
    std::string_view publisher,
    std::string_view executable_path,
    std::string_view family_name,
    std::string_view install_location = {});

/// ARP 键名的规范形式：`Msixpkg.<IdentityName>`。
std::string arp_key_name(std::string_view identity_name);

/// 写入条目；返回实际写入的配置单元（"HKLM" 或 "HKCU"）。两处都失败时抛 Error。
std::string write_arp_entry(const ArpEntry& entry);

/// 删除两个配置单元下的同名条目；返回是否删掉过任何一个。
bool remove_arp_entry(std::string_view key_name);

/// 查询条目当前存在于哪个配置单元（空字符串 = 不存在）。
std::string find_arp_entry_hive(std::string_view key_name);

}  // namespace msixpkg
