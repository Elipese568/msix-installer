#pragma once

// MSAA（IAccessible）暴露（docs/self-drawn-ui-framework.md §6）。
//
// 标准控件移除后，Narrator 看到的就是一个空 Pane。这里把 Widget 树暴露成
// IAccessible 子对象：根对象 = 客户区，子对象 = 可见 Widget + Table 的每一行。

#include <Windows.h>

#include <memory>

struct IAccessible;

namespace msixpkg::ui {

class Host;

struct AccessibleReleaser {
    void operator()(IAccessible* value) const noexcept;
};

using AccessiblePtr = std::unique_ptr<IAccessible, AccessibleReleaser>;

AccessiblePtr create_accessibility_root(Host* host);

}  // namespace msixpkg::ui
