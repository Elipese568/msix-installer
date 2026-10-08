#pragma once

// 测试用的极简框架。刻意零依赖（不引入 Catch2/doctest/gtest）：
// 原生工程的依赖清单要保持「只有 Windows 自己的东西」，而一个注册式测试运行器
// 本身就只有百来行，不值得为此引入第三方库。

#include <cstdio>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "msixpkg/errors.h"

namespace testing {

using TestFunction = void (*)();

struct TestCase {
    std::string name;
    TestFunction function;
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> cases;
    return cases;
}

inline int& failure_count() {
    static int failures = 0;
    return failures;
}

struct Registrar {
    Registrar(const char* name, TestFunction function) {
        registry().push_back(TestCase{name, function});
    }
};

inline void report_failure(const char* file, int line, const std::string& message) {
    ++failure_count();
    std::cout << "  FAIL " << file << ":" << line << "\n       " << message << "\n" << std::flush;
}

inline int run_all() {
    int passed = 0;
    for (const TestCase& test : registry()) {
        int before = failure_count();
        const char* current = test.name.c_str();

        // 逐条打印 + flush：管道下 std::cout 是块缓冲的，若某个测试直接崩溃，
        // 没有这份进度就只能看到「零输出 + 访问违例」。
        std::cout << "[ RUN  ] " << current << std::flush;

        try {
            test.function();
        } catch (const ::msixpkg::Error& error) {
            // 带上 code 与 detail：否则「Win32 error 5」这类关键线索会被 what() 丢掉。
            report_failure("<uncaught>", 0,
                std::string("unexpected msixpkg::Error in ") + current + ": " + error.to_string());
        } catch (const std::exception& ex) {
            report_failure("<uncaught>", 0, std::string("unexpected exception in ") + current + ": " + ex.what());
        } catch (...) {
            report_failure("<uncaught>", 0, std::string("unexpected non-std exception in ") + current);
        }

        if (failure_count() == before) {
            ++passed;
            std::cout << " ok\n" << std::flush;
        } else {
            std::cout << " FAILED\n" << std::flush;
        }
    }

    std::cout << "\n总计: " << registry().size() << "  成功: " << passed
              << "  失败: " << failure_count() << "\n" << std::flush;
    return failure_count() == 0 ? 0 : 1;
}

/// 把字节序列渲染成十六进制，用于断言信息（CHECK_EQ 需要 operator<<）。
inline std::string hex(const unsigned char* data, size_t size) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(size * 2);
    for (size_t i = 0; i < size; ++i) {
        result.push_back(digits[(data[i] >> 4) & 0x0F]);
        result.push_back(digits[data[i] & 0x0F]);
    }
    return result;
}

template <typename Container>
inline std::string hex(const Container& container) {
    return hex(reinterpret_cast<const unsigned char*>(container.data()), container.size());
}

/// 把值渲染成字符串，供 CHECK_EQ 的失败信息使用。
/// 对 std::vector<uint8_t> 专门给一个十六进制重载（否则会尝试 operator<< 而编译失败）。
template <typename T>
inline std::string describe(const T& value) {
    std::ostringstream os;
    os << value;
    return os.str();
}

inline std::string describe(const std::vector<uint8_t>& bytes) {
    const size_t preview = bytes.size() < 64 ? bytes.size() : 64;
    std::string result = "bytes(" + std::to_string(bytes.size()) + ") " + hex(bytes.data(), preview);
    if (preview < bytes.size()) {
        result += "…";
    }
    return result;
}

}  // namespace testing

#define MSIXPKG_TEST(name)                                                              \
    static void msixpkg_test_##name();                                                  \
    static ::testing::Registrar msixpkg_registrar_##name(#name, &msixpkg_test_##name);   \
    static void msixpkg_test_##name()

#define CHECK(condition)                                                                \
    do {                                                                                \
        if (!(condition)) {                                                             \
            ::testing::report_failure(__FILE__, __LINE__, "CHECK failed: " #condition); \
        }                                                                               \
    } while (0)

#define CHECK_MSG(condition, message)                                                   \
    do {                                                                                \
        if (!(condition)) {                                                             \
            std::ostringstream os;                                                      \
            os << "CHECK failed: " #condition << " — " << message;                      \
            ::testing::report_failure(__FILE__, __LINE__, os.str());                    \
        }                                                                               \
    } while (0)

#define CHECK_EQ(actual, expected)                                                      \
    do {                                                                                \
        auto&& a_ = (actual);                                                           \
        auto&& e_ = (expected);                                                         \
        if (!(a_ == e_)) {                                                              \
            std::ostringstream os;                                                      \
            os << "CHECK_EQ failed: " #actual " == " #expected                          \
               << "\n         actual = " << ::testing::describe(a_)                     \
               << "\n       expected = " << ::testing::describe(e_);                    \
            ::testing::report_failure(__FILE__, __LINE__, os.str());                    \
        }                                                                               \
    } while (0)

/// 断言表达式抛出异常，且异常文本包含 expectedSubstring（用于纯解析层错误）。
#define CHECK_THROWS_MESSAGE(expression, expectedSubstring)                             \
    do {                                                                                \
        bool threw_ = false;                                                            \
        std::string got_;                                                               \
        try {                                                                           \
            (void)(expression);                                                         \
        } catch (const std::exception& error_) {                                        \
            threw_ = true;                                                              \
            got_ = error_.what();                                                       \
        }                                                                               \
        if (!threw_) {                                                                  \
            ::testing::report_failure(__FILE__, __LINE__,                               \
                "expected " #expression " to throw but it did not");                    \
        } else if (got_.find(expectedSubstring) == std::string::npos) {                  \
            ::testing::report_failure(__FILE__, __LINE__,                               \
                std::string("expected message containing \"") + expectedSubstring +     \
                "\" but got \"" + got_ + "\"");                                         \
        }                                                                               \
    } while (0)

/// 断言表达式抛出 Error 且错误码正好等于 expectedCode。
#define CHECK_THROWS_CODE(expression, expectedCode)                                     \
    do {                                                                                \
        bool threw_ = false;                                                            \
        std::string got_;                                                               \
        try {                                                                           \
            (void)(expression);                                                         \
        } catch (const ::msixpkg::Error& error_) {                                      \
            threw_ = true;                                                              \
            got_ = error_.code();                                                       \
        } catch (const std::exception& other_) {                                        \
            threw_ = true;                                                              \
            got_ = std::string("(non-msixpkg) ") + other_.what();                        \
        }                                                                               \
        if (!threw_) {                                                                  \
            ::testing::report_failure(__FILE__, __LINE__,                               \
                std::string("expected " #expression " to throw ") + expectedCode +      \
                " but it did not");                                                     \
        } else if (got_ != expectedCode) {                                              \
            ::testing::report_failure(__FILE__, __LINE__,                               \
                std::string("expected code ") + expectedCode + " but got " + got_);     \
        }                                                                               \
    } while (0)
