#pragma once

// 运维子命令的实现：list / verify / repair / clean（docs/installer-design.md §7）。
//
// 一条硬规则：**repair 只输出建议，绝不擅自改动系统**。
// 「状态库 vs 实际系统」的差异可能是我们没预料到的场景，自动「修」等于在
// 用户不知情的情况下删证书或删包 —— 那是不可逆的。

#include <cstdint>
#include <string>
#include <vector>

#include "log.h"
#include "state.h"

namespace msixpkg {

/// 一条安装记录 + 「它现在在系统里还成不成立」。
struct InstallationStatus {
    InstallationRecord record;
    /// 包是否仍在当前用户的仓库里。
    bool package_present = false;
    /// 记录的证书指纹现在是否还在本机受信任人员存储里。
    bool certificate_present = false;
    /// ARP 卸载条目现在在哪个配置单元（空 = 不在）。
    std::string arp_hive;
};

struct VerifyIssue {
    /// "error" / "warning" / "info"
    std::string severity;
    /// 稳定的问题标识（脚本可判断）。
    std::string code;
    std::string detail;
    /// 建议的手工命令（可为空）。
    std::string suggestion;
};

struct VerifyReport {
    std::vector<InstallationStatus> installations;
    std::vector<VerifyIssue> issues;
    /// 暂存目录里没有被任何记录引用的残留目录。
    std::vector<std::string> orphan_staging;

    [[nodiscard]] bool ok() const;
    [[nodiscard]] bool has_errors() const;
};

/// 重算「状态库 vs 实际系统」。
VerifyReport verify_state(const StateStore& store, const Log& log);

/// 在 verify 的基础上给出修复建议（只读操作）。
VerifyReport repair_plan(const StateStore& store, const Log& log);

struct CleanReport {
    int staging_removed = 0;
    int logs_removed = 0;
    int temp_files_removed = 0;
    uint64_t bytes_freed = 0;
};

/// 清理暂存残留、过期日志与原子写留下的临时文件。
/// `keep_logs` 为保留的最新日志份数（默认 20）。
CleanReport clean_data(const StateStore& store, size_t keep_logs, const Log& log);

}  // namespace msixpkg
