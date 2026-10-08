using System;
using System.Collections.Generic;
using System.Globalization;
using System.Text;

namespace Msixpkg.Core.Json
{
    internal enum JsonKind
    {
        Null,
        Boolean,
        Integer,
        String,
        Array,
        Object,
    }

    /// <summary>
    /// 最小 JSON 文档模型：只支持本格式用到的子集（对象、数组、字符串、64 位整数、布尔、null）。
    /// 刻意不引入 System.Text.Json —— MSBuild 的 .NET Framework 宿主会对程序集绑定重定向敏感，
    /// 而 C++ 端本来就需要自己的一份实现，两端共享同一子集语义更利于一致性测试。
    /// </summary>
    internal sealed class JsonValue
    {
        private readonly List<JsonValue> _items;
        private readonly List<KeyValuePair<string, JsonValue>> _members;

        private JsonValue(JsonKind kind, bool boolean, long integer, string text)
        {
            Kind = kind;
            BooleanValue = boolean;
            IntegerValue = integer;
            StringValue = text;
            _items = new List<JsonValue>();
            _members = new List<KeyValuePair<string, JsonValue>>();
        }

        public JsonKind Kind { get; }

        public bool BooleanValue { get; }

        public long IntegerValue { get; }

        public string StringValue { get; }

        public IReadOnlyList<JsonValue> Items => _items;

        public IReadOnlyList<KeyValuePair<string, JsonValue>> Members => _members;

        public static JsonValue Null() => new JsonValue(JsonKind.Null, false, 0L, string.Empty);

        public static JsonValue Of(bool value) => new JsonValue(JsonKind.Boolean, value, 0L, string.Empty);

        public static JsonValue Of(long value) => new JsonValue(JsonKind.Integer, false, value, string.Empty);

        public static JsonValue Of(string value) =>
            new JsonValue(JsonKind.String, false, 0L, value ?? throw new ArgumentNullException(nameof(value)));

        public static JsonValue NewArray() => new JsonValue(JsonKind.Array, false, 0L, string.Empty);

        public static JsonValue NewObject() => new JsonValue(JsonKind.Object, false, 0L, string.Empty);

        public void Add(JsonValue item)
        {
            if (Kind != JsonKind.Array)
            {
                throw new InvalidOperationException("Add(JsonValue) requires an array.");
            }

            _items.Add(item ?? Null());
        }

        public void Add(string name, JsonValue value)
        {
            if (Kind != JsonKind.Object)
            {
                throw new InvalidOperationException("Add(string, JsonValue) requires an object.");
            }

            if (name == null)
            {
                throw new ArgumentNullException(nameof(name));
            }

            for (int i = 0; i < _members.Count; i++)
            {
                if (string.Equals(_members[i].Key, name, StringComparison.Ordinal))
                {
                    throw new InvalidOperationException("Duplicate member '" + name + "'.");
                }
            }

            _members.Add(new KeyValuePair<string, JsonValue>(name, value ?? Null()));
        }

        public bool TryGet(string name, out JsonValue value)
        {
            if (Kind == JsonKind.Object)
            {
                for (int i = 0; i < _members.Count; i++)
                {
                    if (string.Equals(_members[i].Key, name, StringComparison.Ordinal))
                    {
                        value = _members[i].Value;
                        return true;
                    }
                }
            }

            value = Null();
            return false;
        }

        public JsonValue? Get(string name)
        {
            return TryGet(name, out JsonValue value) && value.Kind != JsonKind.Null ? value : null;
        }

        public string ToJson()
        {
            StringBuilder sb = new StringBuilder(256);
            Write(sb, this, 0);
            sb.Append('\n');
            return sb.ToString();
        }

        private static void Write(StringBuilder sb, JsonValue value, int indent)
        {
            switch (value.Kind)
            {
                case JsonKind.Null:
                    sb.Append("null");
                    break;
                case JsonKind.Boolean:
                    sb.Append(value.BooleanValue ? "true" : "false");
                    break;
                case JsonKind.Integer:
                    sb.Append(value.IntegerValue.ToString(CultureInfo.InvariantCulture));
                    break;
                case JsonKind.String:
                    WriteString(sb, value.StringValue);
                    break;
                case JsonKind.Array:
                    WriteArray(sb, value, indent);
                    break;
                case JsonKind.Object:
                    WriteObject(sb, value, indent);
                    break;
                default:
                    throw new InvalidOperationException("Unknown JSON kind.");
            }
        }

        private static void WriteArray(StringBuilder sb, JsonValue value, int indent)
        {
            if (value.Items.Count == 0)
            {
                sb.Append("[]");
                return;
            }

            sb.Append("[\n");
            for (int i = 0; i < value.Items.Count; i++)
            {
                Indent(sb, indent + 1);
                Write(sb, value.Items[i], indent + 1);
                sb.Append(i == value.Items.Count - 1 ? "\n" : ",\n");
            }

            Indent(sb, indent);
            sb.Append(']');
        }

        private static void WriteObject(StringBuilder sb, JsonValue value, int indent)
        {
            if (value.Members.Count == 0)
            {
                sb.Append("{}");
                return;
            }

            sb.Append("{\n");
            for (int i = 0; i < value.Members.Count; i++)
            {
                Indent(sb, indent + 1);
                WriteString(sb, value.Members[i].Key);
                sb.Append(": ");
                Write(sb, value.Members[i].Value, indent + 1);
                sb.Append(i == value.Members.Count - 1 ? "\n" : ",\n");
            }

            Indent(sb, indent);
            sb.Append('}');
        }

        private static void Indent(StringBuilder sb, int indent)
        {
            for (int i = 0; i < indent; i++)
            {
                sb.Append("  ");
            }
        }

        private static void WriteString(StringBuilder sb, string text)
        {
            sb.Append('"');
            for (int i = 0; i < text.Length; i++)
            {
                char c = text[i];
                switch (c)
                {
                    case '"':
                        sb.Append("\\\"");
                        break;
                    case '\\':
                        sb.Append("\\\\");
                        break;
                    case '\n':
                        sb.Append("\\n");
                        break;
                    case '\r':
                        sb.Append("\\r");
                        break;
                    case '\t':
                        sb.Append("\\t");
                        break;
                    case '\b':
                        sb.Append("\\b");
                        break;
                    case '\f':
                        sb.Append("\\f");
                        break;
                    default:
                        if (c < 0x20 || c == 0x7F)
                        {
                            sb.Append("\\u");
                            sb.Append(((int)c).ToString("x4", CultureInfo.InvariantCulture));
                        }
                        else
                        {
                            sb.Append(c);
                        }

                        break;
                }
            }

            sb.Append('"');
        }
    }
}
