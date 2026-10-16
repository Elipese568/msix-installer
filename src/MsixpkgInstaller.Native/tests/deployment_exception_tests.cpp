// 部署层的「WinRT 异常防火墙」回归测试。
//
// 实测踩过的坑：`winrt::hresult_error` 是**不继承 std::exception** 的普通结构体。
// 它一旦逃出部署层，上层 `catch (const std::exception&)` 接不住，进程会
// `std::terminate()` → `abort()` → 退出码 3，且 stdout 缓冲来不及 flush，
// 用户看到的是「什么都没输出、进程莫名消失」。
//
// 因此本用例断言：部署层的对外函数**只**抛 msixpkg::Error（或正常返回），
// 绝不把 WinRT 异常类型泄露给调用方。

#include <Windows.h>

#include <iostream>
#include <string>
#include <vector>

#include "deployment.h"
#include "test_harness.h"

namespace {

/// 调用一个部署层函数，返回它抛出的「非 msixpkg::Error、也非 std::exception」的异常。
/// 返回 true 表示有异常逃逸（就是我们要防的那种）。
template <typename Function>
bool leaks_foreign_exception(Function&& function, std::string& note) {
    try {
        function();
        note = "returned normally";
        return false;
    } catch (const msixpkg::Error& error) {
        note = "msixpkg::Error " + error.code();
        return false;
    } catch (const std::exception& error) {
        note = std::string("std::exception ") + error.what();
        return false;
    } catch (...) {
        note = "a non-standard exception escaped";
        return true;
    }
}

}  // namespace

MSIXPKG_TEST(deployment_never_leaks_winrt_exceptions_to_the_caller) {
    std::string note;

    // 枚举包仓库：这正是在受限进程里触发过 abort 的那条路径。
    const bool packages_leaked = leaks_foreign_exception(
        [] {
            const std::vector<msixpkg::RegisteredPackage> packages = msixpkg::find_registered_packages();
            (void)packages;
        },
        note);
    if (packages_leaked) {
        CHECK_MSG(false, std::string("find_registered_packages: ") + note);
    } else {
        std::cout << "\n  find_registered_packages: " << note << std::flush;
    }

    // 单包查询。
    const bool registered_leaked = leaks_foreign_exception(
        [] {
            const bool registered = msixpkg::is_package_registered("No.Such.Package_0000000000000");
            (void)registered;
        },
        note);
    if (registered_leaked) {
        CHECK_MSG(false, std::string("is_package_registered: ") + note);
    } else {
        std::cout << "\n  is_package_registered: " << note << std::flush;
    }

    // 卸载一个不存在的包族名：必须正常返回「没什么可卸」，或者抛出 msixpkg::Error。
    const bool uninstall_leaked = leaks_foreign_exception(
        [] {
            msixpkg::UninstallRequest request;
            request.family_name = "No.Such.Package_0000000000000";
            const msixpkg::UninstallResult result = msixpkg::uninstall_package(request);
            (void)result;
        },
        note);
    if (uninstall_leaked) {
        CHECK_MSG(false, std::string("uninstall_package: ") + note);
    } else {
        std::cout << "\n  uninstall_package: " << note << std::flush;
    }
}

MSIXPKG_TEST(deployment_install_rejects_a_missing_package_before_touching_winrt) {
    // 路径不存在时必须在调用部署栈**之前**就拒绝：既不浪费一次 AppXSvc 往返，
    // 也保证这条最常见的用法错误走稳定错误码。
    msixpkg::InstallRequest request;
    request.main_package = std::filesystem::path(MSIXPKG_REPO_ROOT) / "artifacts" / "native-tests" /
                           "no-such-package.msix";

    std::string note;
    const bool leaked = leaks_foreign_exception(
        [&request] {
            const msixpkg::InstallResult result = msixpkg::install_packages(request);
            (void)result;
        },
        note);
    CHECK_MSG(!leaked, note);
    CHECK_THROWS_CODE(msixpkg::install_packages(request), msixpkg::codes::FileMissing);
}
