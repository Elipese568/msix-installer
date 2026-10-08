using System.Collections.Generic;
using System.Text;

namespace Msixpkg.Core.Json
{
    internal static class JsonParser
    {
        private const int MaxDepth = 32;

        public static JsonValue Parse(string text)
        {
            if (text == null)
            {
                throw new JsonFormatException("JSON text is null.", 0);
            }

            int position = 0;
            JsonValue value = ParseValue(text, ref position, 0);
            SkipWhitespace(text, ref position);
            if (position != text.Length)
            {
                throw new JsonFormatException("Unexpected trailing content.", position);
            }

            return value;
        }

        private static JsonValue ParseValue(string text, ref int position, int depth)
        {
            if (depth > MaxDepth)
            {
                throw new JsonFormatException("Maximum nesting depth exceeded.", position);
            }

            SkipWhitespace(text, ref position);
            if (position >= text.Length)
            {
                throw new JsonFormatException("Unexpected end of JSON text.", position);
            }

            char c = text[position];
            switch (c)
            {
                case '{':
                    return ParseObject(text, ref position, depth);
                case '[':
                    return ParseArray(text, ref position, depth);
                case '"':
                    return JsonValue.Of(ParseString(text, ref position));
                case 't':
                    Expect(text, ref position, "true");
                    return JsonValue.Of(true);
                case 'f':
                    Expect(text, ref position, "false");
                    return JsonValue.Of(false);
                case 'n':
                    Expect(text, ref position, "null");
                    return JsonValue.Null();
                default:
                    return ParseInteger(text, ref position);
            }
        }

        private static JsonValue ParseObject(string text, ref int position, int depth)
        {
            var result = JsonValue.NewObject();
            position++; // '{'

            SkipWhitespace(text, ref position);
            if (position < text.Length && text[position] == '}')
            {
                position++;
                return result;
            }

            while (true)
            {
                SkipWhitespace(text, ref position);
                if (position >= text.Length || text[position] != '"')
                {
                    throw new JsonFormatException("Expected a member name string.", position);
                }

                string name = ParseString(text, ref position);
                if (result.TryGet(name, out _))
                {
                    throw new JsonFormatException("Duplicate member '" + name + "'.", position);
                }

                SkipWhitespace(text, ref position);
                if (position >= text.Length || text[position] != ':')
                {
                    throw new JsonFormatException("Expected ':' after member name.", position);
                }

                position++;
                JsonValue member = ParseValue(text, ref position, depth + 1);
                result.Add(name, member);

                SkipWhitespace(text, ref position);
                if (position >= text.Length)
                {
                    throw new JsonFormatException("Unterminated object.", position);
                }

                if (text[position] == ',')
                {
                    position++;
                    continue;
                }

                if (text[position] == '}')
                {
                    position++;
                    return result;
                }

                throw new JsonFormatException("Expected ',' or '}' in object.", position);
            }
        }

        private static JsonValue ParseArray(string text, ref int position, int depth)
        {
            var result = JsonValue.NewArray();
            position++; // '['

            SkipWhitespace(text, ref position);
            if (position < text.Length && text[position] == ']')
            {
                position++;
                return result;
            }

            while (true)
            {
                result.Add(ParseValue(text, ref position, depth + 1));
                SkipWhitespace(text, ref position);
                if (position >= text.Length)
                {
                    throw new JsonFormatException("Unterminated array.", position);
                }

                if (text[position] == ',')
                {
                    position++;
                    continue;
                }

                if (text[position] == ']')
                {
                    position++;
                    return result;
                }

                throw new JsonFormatException("Expected ',' or ']' in array.", position);
            }
        }

        private static string ParseString(string text, ref int position)
        {
            position++; // opening quote
            var sb = new StringBuilder();
            while (true)
            {
                if (position >= text.Length)
                {
                    throw new JsonFormatException("Unterminated string.", position);
                }

                char c = text[position++];
                if (c == '"')
                {
                    return sb.ToString();
                }

                if (c == '\\')
                {
                    if (position >= text.Length)
                    {
                        throw new JsonFormatException("Unterminated escape sequence.", position);
                    }

                    char e = text[position++];
                    switch (e)
                    {
                        case '"':
                            sb.Append('"');
                            break;
                        case '\\':
                            sb.Append('\\');
                            break;
                        case '/':
                            sb.Append('/');
                            break;
                        case 'b':
                            sb.Append('\b');
                            break;
                        case 'f':
                            sb.Append('\f');
                            break;
                        case 'n':
                            sb.Append('\n');
                            break;
                        case 'r':
                            sb.Append('\r');
                            break;
                        case 't':
                            sb.Append('\t');
                            break;
                        case 'u':
                            sb.Append(ParseUnicodeEscape(text, ref position));
                            break;
                        default:
                            throw new JsonFormatException("Invalid escape sequence '\\" + e + "'.", position);
                    }

                    continue;
                }

                if (c < 0x20)
                {
                    throw new JsonFormatException("Unescaped control character in string.", position - 1);
                }

                sb.Append(c);
            }
        }

        private static char ParseUnicodeEscape(string text, ref int position)
        {
            if (position + 4 > text.Length)
            {
                throw new JsonFormatException("Truncated \\u escape sequence.", position);
            }

            int code = 0;
            for (int i = 0; i < 4; i++)
            {
                int digit = HexDigit(text[position + i]);
                if (digit < 0)
                {
                    throw new JsonFormatException("Invalid hex digit in \\u escape sequence.", position + i);
                }

                code = (code << 4) | digit;
            }

            position += 4;
            return (char)code;
        }

        private static int HexDigit(char c)
        {
            if (c >= '0' && c <= '9')
            {
                return c - '0';
            }

            if (c >= 'a' && c <= 'f')
            {
                return c - 'a' + 10;
            }

            if (c >= 'A' && c <= 'F')
            {
                return c - 'A' + 10;
            }

            return -1;
        }

        private static JsonValue ParseInteger(string text, ref int position)
        {
            int start = position;
            if (position < text.Length && text[position] == '-')
            {
                position++;
            }

            if (position >= text.Length || text[position] < '0' || text[position] > '9')
            {
                throw new JsonFormatException("Expected a JSON value.", start);
            }

            if (text[position] == '0' && position + 1 < text.Length && text[position + 1] >= '0' && text[position + 1] <= '9')
            {
                throw new JsonFormatException("Leading zeros are not allowed.", position);
            }

            long value = 0L;
            int digits = 0;
            while (position < text.Length && text[position] >= '0' && text[position] <= '9')
            {
                checked
                {
                    value = (value * 10L) + (text[position] - '0');
                }

                position++;
                digits++;
            }

            if (digits == 0)
            {
                throw new JsonFormatException("Expected digits.", position);
            }

            if (position < text.Length && (text[position] == '.' || text[position] == 'e' || text[position] == 'E'))
            {
                throw new JsonFormatException("Fractional and exponent numbers are not supported by this format.", position);
            }

            if (text[start] == '-' && value == 0L)
            {
                throw new JsonFormatException("'-0' is not a valid integer.", start);
            }

            return JsonValue.Of(value);
        }

        private static void Expect(string text, ref int position, string literal)
        {
            if (position + literal.Length > text.Length ||
                string.CompareOrdinal(text, position, literal, 0, literal.Length) != 0)
            {
                throw new JsonFormatException("Expected literal '" + literal + "'.", position);
            }

            position += literal.Length;
        }

        private static void SkipWhitespace(string text, ref int position)
        {
            while (position < text.Length)
            {
                char c = text[position];
                if (c != ' ' && c != '\t' && c != '\r' && c != '\n')
                {
                    return;
                }

                position++;
            }
        }
    }

    internal sealed class JsonFormatException : System.Exception
    {
        public JsonFormatException(string message, int position)
            : base(message + " (position " + position.ToString(System.Globalization.CultureInfo.InvariantCulture) + ")")
        {
            Position = position;
        }

        public JsonFormatException(string message)
            : base(message)
        {
        }

        public int Position { get; }
    }
}
