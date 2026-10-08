#pragma once

// 与 C# 端（src/Msixpkg.Core/Json/）语义完全一致的最小 JSON 实现。
// 之所以两端各写一份而不是复用第三方库：C++ 侧要零第三方依赖，而 C# 侧要能进 MSBuild 的
// .NET Framework 宿主（避免绑定重定向）。两端共享的契约是 docs/msixpkg-format-v1.md §3：
// 只接受对象/数组/字符串/64 位整数/布尔/null，拒绝重复成员、浮点、前导零、裸控制字符。

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace msixpkg::json {

class ParseError final : public std::runtime_error {
public:
    ParseError(std::string message, size_t position)
        : std::runtime_error(message + " (position " + std::to_string(position) + ")"), position_(position) {}

    [[nodiscard]] size_t position() const noexcept { return position_; }

private:
    size_t position_;
};

class Value {
public:
    enum class Kind { Null, Boolean, Integer, String, Array, Object };

    Kind kind = Kind::Null;
    bool boolean = false;
    int64_t integer = 0;
    std::string text;
    std::vector<Value> items;
    std::vector<std::pair<std::string, Value>> members;

    static Value make_null() { return Value{}; }
    static Value make_bool(bool value);
    static Value make_integer(int64_t value);
    static Value make_string(std::string value);
    static Value make_array();
    static Value make_object();

    void push(Value item);
    void set(std::string name, Value value);

    [[nodiscard]] const Value* find(std::string_view name) const;
    [[nodiscard]] bool has(std::string_view name) const { return find(name) != nullptr; }

    /// 规范化的 JSON 文本（2 空格缩进、成员插入序、末尾换行）。
    /// 与 C# 端 JsonValue.ToJson 逐字节一致 —— 黄金样本里有专门的比对测试。
    [[nodiscard]] std::string to_json() const;

private:
    static void write(std::string& out, const Value& value, int indent);
};

/// 解析；失败抛 ParseError（由上层转成稳定的 msixpkg 错误码）。
Value parse(std::string_view text);

}  // namespace msixpkg::json
