#pragma once

// Win32 原生安装向导（docs/installer-design.md §8）。
//
// 分工（§4 的「前端与编排分离」）：
//   gui_wizard.cpp   —— 窗口、消息循环、五页状态机、控件布局与本地化
//   gui_worker.cpp   —— 编排线程：Installer 调用、IProgressSink / IConsentProvider /
//                       ICancelSource 的 GUI 实现、只读的容器摘要收集
//   gui_manage.cpp   —— 管理页（状态库 → 列表 / 卸载 / 修复建议 / 打开日志与设置）
//   gui_strings.cpp  —— 字符串表加载与格式化
//
// 线程模型（只有两条线程在动）：
//   UI 线程      —— 所有控件都只由它碰；它从不阻塞，也从不调用 Installer。
//   编排线程     —— 唯一调用 Installer / StateStore / WinRT 部署栈的一方；
//                   与 UI 的通信只有两种：PostMessage（异步进度/结果）与
//                   一个「请求-应答」信箱（同意页的同步问答）。
// 这两条规则是「UI 不卡」与「数据不悬垂」的全部依据：
// 跨线程传递的对象一律 std::shared_ptr，事件对象绝不按引用跨线程。

#include <Windows.h>

#include <cstdint>
#include <filesystem>
#include <string>

namespace msixpkg::gui {

// 外部依赖只用一个接口，方便静态声明而不把 installer.h 拉进每个编译单元。
struct GuiHost;

/// 向导主入口（由 gui_wizard.cpp 的 wWinMain 调用）：
/// 解析命令行 → 单实例仲裁 → 建窗 → 跑消息循环 → 返回退出码。
/// 退出码与 CLI 一致：0 成功 / 2 校验失败 / 5 部署失败 / 6 取消。
int run(void* instance, const wchar_t* command_line);

}  // namespace msixpkg::gui
