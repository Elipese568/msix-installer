#include "json.h"

#include <charconv>
#include <cstdio>

namespace msixpkg::json {
namespace {

constexpr int kMaxDepth = 32;

[[noreturn]] void fail_at(std::string_view message, size_t position) {
    throw ParseError(std::string(message), position);
}

void skip_whitespace(std::string_view text, size_t& position) {
    while (position < text.size()) {
        const char c = text[position];
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
            return;
        }
        ++position;
    }
}

int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string parse_string(std::string_view text, size_t& position) {
    ++position;  // 开引号
    std::string result;
    while (true) {
        if (position >= text.size()) {
            fail_at("Unterminated string", position);
        }

        const char c = text[position++];
        if (c == '"') {
            return result;
        }

        if (c == '\\') {
            if (position >= text.size()) {
                fail_at("Unterminated escape sequence", position);
            }

            const char e = text[position++];
            switch (e) {
                case '"': result.push_back('"'); break;
                case '\\': result.push_back('\\'); break;
                case '/': result.push_back('/'); break;
                case 'b': result.push_back('\b'); break;
                case 'f': result.push_back('\f'); break;
                case 'n': result.push_back('\n'); break;
                case 'r': result.push_back('\r'); break;
                case 't': result.push_back('\t'); break;
                case 'u': {
                    if (position + 4 > text.size()) {
                        fail_at("Truncated \\u escape sequence", position);
                    }
                    int code = 0;
                    for (int i = 0; i < 4; ++i) {
                        const int digit = hex_digit(text[position + static_cast<size_t>(i)]);
                        if (digit < 0) {
                            fail_at("Invalid hex digit in \\u escape sequence", position + static_cast<size_t>(i));
                        }
                        code = (code << 4) | digit;
                    }
                    position += 4;
                    // 只做 UTF-16 → UTF-8 的基本面处理；本格式的字符串都是 ASCII 或常规 BMP 文本。
                    const auto unit = static_cast<uint16_t>(code);
                    if (unit < 0x80) {
                        result.push_back(static_cast<char>(unit));
                    } else if (unit < 0x800) {
                        result.push_back(static_cast<char>(0xC0 | (unit >> 6)));
                        result.push_back(static_cast<char>(0x80 | (unit & 0x3F)));
                    } else {
                        result.push_back(static_cast<char>(0xE0 | (unit >> 12)));
                        result.push_back(static_cast<char>(0x80 | ((unit >> 6) & 0x3F)));
                        result.push_back(static_cast<char>(0x80 | (unit & 0x3F)));
                    }
                    break;
                }
                default:
                    fail_at(std::string("Invalid escape sequence '\\") + e + "'", position);
            }
            continue;
        }

        if (static_cast<unsigned char>(c) < 0x20) {
            fail_at("Unescaped control character in string", position - 1);
        }

        result.push_back(c);
    }
}

Value parse_value(std::string_view text, size_t& position, int depth);

Value parse_object(std::string_view text, size_t& position, int depth) {
    Value result = Value::make_object();
    ++position;  // '{'

    skip_whitespace(text, position);
    if (position < text.size() && text[position] == '}') {
        ++position;
        return result;
    }

    while (true) {
        skip_whitespace(text, position);
        if (position >= text.size() || text[position] != '"') {
            fail_at("Expected a member name string", position);
        }

        std::string name = parse_string(text, position);
        if (result.find(name) != nullptr) {
            fail_at("Duplicate member '" + name + "'", position);
        }

        skip_whitespace(text, position);
        if (position >= text.size() || text[position] != ':') {
            fail_at("Expected ':' after member name", position);
        }
        ++position;

        result.set(std::move(name), parse_value(text, position, depth + 1));

        skip_whitespace(text, position);
        if (position >= text.size()) {
            fail_at("Unterminated object", position);
        }
        if (text[position] == ',') {
            ++position;
            continue;
        }
        if (text[position] == '}') {
            ++position;
            return result;
        }
        fail_at("Expected ',' or '}' in object", position);
    }
}

Value parse_array(std::string_view text, size_t& position, int depth) {
    Value result = Value::make_array();
    ++position;  // '['

    skip_whitespace(text, position);
    if (position < text.size() && text[position] == ']') {
        ++position;
        return result;
    }

    while (true) {
        result.push(parse_value(text, position, depth + 1));
        skip_whitespace(text, position);
        if (position >= text.size()) {
            fail_at("Unterminated array", position);
        }
        if (text[position] == ',') {
            ++position;
            continue;
        }
        if (text[position] == ']') {
            ++position;
            return result;
        }
        fail_at("Expected ',' or ']' in array", position);
    }
}

void expect_literal(std::string_view text, size_t& position, std::string_view literal) {
    if (position + literal.size() > text.size() ||
        text.compare(position, literal.size(), literal) != 0) {
        fail_at("Expected literal '" + std::string(literal) + "'", position);
    }
    position += literal.size();
}

Value parse_integer(std::string_view text, size_t& position) {
    const size_t start = position;
    if (position < text.size() && text[position] == '-') {
        ++position;
    }

    if (position >= text.size() || text[position] < '0' || text[position] > '9') {
        fail_at("Expected a JSON value", start);
    }

    if (text[position] == '0' && position + 1 < text.size() && text[position + 1] >= '0' && text[position + 1] <= '9') {
        fail_at("Leading zeros are not allowed", position);
    }

    int64_t value = 0;
    size_t digits = 0;
    while (position < text.size() && text[position] >= '0' && text[position] <= '9') {
        const int64_t digit = text[position] - '0';
        if (value > (INT64_MAX - digit) / 10) {
            fail_at("Integer is out of range", position);
        }
        value = value * 10 + digit;
        ++position;
        ++digits;
    }

    if (digits == 0) {
        fail_at("Expected digits", position);
    }

    if (position < text.size() && (text[position] == '.' || text[position] == 'e' || text[position] == 'E')) {
        fail_at("Fractional and exponent numbers are not supported by this format", position);
    }

    if (text[start] == '-' && value == 0) {
        fail_at("'-0' is not a valid integer", start);
    }

    return text[start] == '-' ? Value::make_integer(-value) : Value::make_integer(value);
}

Value parse_value(std::string_view text, size_t& position, int depth) {
    if (depth > kMaxDepth) {
        fail_at("Maximum nesting depth exceeded", position);
    }

    skip_whitespace(text, position);
    if (position >= text.size()) {
        fail_at("Unexpected end of JSON text", position);
    }

    switch (text[position]) {
        case '{': return parse_object(text, position, depth);
        case '[': return parse_array(text, position, depth);
        case '"': return Value::make_string(parse_string(text, position));
        case 't': expect_literal(text, position, "true"); return Value::make_bool(true);
        case 'f': expect_literal(text, position, "false"); return Value::make_bool(false);
        case 'n': expect_literal(text, position, "null"); return Value::make_null();
        default: return parse_integer(text, position);
    }
}

void write_string(std::string& out, std::string_view text) {
    out.push_back('"');
    for (const char raw : text) {
        const auto c = static_cast<unsigned char>(raw);
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            default:
                if (c < 0x20 || c == 0x7F) {
                    char buffer[8];
                    std::snprintf(buffer, sizeof(buffer), "\\u%04x", c);
                    out += buffer;
                } else {
                    out.push_back(static_cast<char>(c));
                }
                break;
        }
    }
    out.push_back('"');
}

void write_indent(std::string& out, int indent) {
    for (int i = 0; i < indent; ++i) {
        out += "  ";
    }
}

}  // namespace

Value Value::make_bool(bool value) {
    Value result;
    result.kind = Kind::Boolean;
    result.boolean = value;
    return result;
}

Value Value::make_integer(int64_t value) {
    Value result;
    result.kind = Kind::Integer;
    result.integer = value;
    return result;
}

Value Value::make_string(std::string value) {
    Value result;
    result.kind = Kind::String;
    result.text = std::move(value);
    return result;
}

Value Value::make_array() {
    Value result;
    result.kind = Kind::Array;
    return result;
}

Value Value::make_object() {
    Value result;
    result.kind = Kind::Object;
    return result;
}

void Value::push(Value item) {
    items.push_back(std::move(item));
}

void Value::set(std::string name, Value value) {
    members.emplace_back(std::move(name), std::move(value));
}

const Value* Value::find(std::string_view name) const {
    if (kind != Kind::Object) {
        return nullptr;
    }

    for (const auto& member : members) {
        if (member.first == name) {
            return &member.second;
        }
    }

    return nullptr;
}

std::string Value::to_json() const {
    std::string out;
    write(out, *this, 0);
    out.push_back('\n');
    return out;
}

void Value::write(std::string& out, const Value& value, int indent) {
    switch (value.kind) {
        case Kind::Null:
            out += "null";
            break;
        case Kind::Boolean:
            out += value.boolean ? "true" : "false";
            break;
        case Kind::Integer:
            out += std::to_string(value.integer);
            break;
        case Kind::String:
            write_string(out, value.text);
            break;
        case Kind::Array:
            if (value.items.empty()) {
                out += "[]";
                break;
            }
            out += "[\n";
            for (size_t i = 0; i < value.items.size(); ++i) {
                write_indent(out, indent + 1);
                write(out, value.items[i], indent + 1);
                out += (i + 1 == value.items.size()) ? "\n" : ",\n";
            }
            write_indent(out, indent);
            out.push_back(']');
            break;
        case Kind::Object:
            if (value.members.empty()) {
                out += "{}";
                break;
            }
            out += "{\n";
            for (size_t i = 0; i < value.members.size(); ++i) {
                write_indent(out, indent + 1);
                write_string(out, value.members[i].first);
                out += ": ";
                write(out, value.members[i].second, indent + 1);
                out += (i + 1 == value.members.size()) ? "\n" : ",\n";
            }
            write_indent(out, indent);
            out.push_back('}');
            break;
    }
}

Value parse(std::string_view text) {
    size_t position = 0;
    Value value = parse_value(text, position, 0);
    skip_whitespace(text, position);
    if (position != text.size()) {
        fail_at("Unexpected trailing content", position);
    }
    return value;
}

}  // namespace msixpkg::json
