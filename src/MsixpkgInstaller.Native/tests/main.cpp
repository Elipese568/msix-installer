#include <Windows.h>

#include <iostream>

#include "test_harness.h"

int main() {
    // 控制台按 UTF-8 输出，保证中文与错误码不被代码页破坏。
    ::SetConsoleOutputCP(CP_UTF8);

    std::cout << "msixpkg 原生核心测试\n夹具目录: " << MSIXPKG_VECTORS_DIR << "\n\n";
    return ::testing::run_all();
}
