using System;
using System.Collections.Generic;
using System.Globalization;

namespace Msixpkg.Packer;

/// <summary>
/// 手写的最小参数解析：本工具的参数集固定且很小，引入 System.CommandLine 只会带来
/// netstandard 之外的依赖与绑定重定向风险（打包逻辑还要被 MSBuild 进程内加载）。
/// </summary>
internal sealed class CommandLine
{
    private readonly List<string> _positional;
    private readonly Dictionary<string, string> _options;
    private readonly HashSet<string> _repeated;

    private CommandLine(List<string> positional, Dictionary<string, string> options, HashSet<string> repeated)
    {
        _positional = positional;
        _options = options;
        _repeated = repeated;
    }

    public IReadOnlyList<string> Positional => _positional;

    public static CommandLine Parse(
        IEnumerable<string> args,
        IReadOnlyCollection<string> flags,
        IReadOnlyCollection<string>? repeatable = null)
    {
        var positional = new List<string>();
        var options = new Dictionary<string, string>(StringComparer.Ordinal);
        var repeated = new HashSet<string>(repeatable ?? Array.Empty<string>(), StringComparer.Ordinal);
        var flagSet = new HashSet<string>(flags, StringComparer.Ordinal);

        using IEnumerator<string> enumerator = args.GetEnumerator();
        while (enumerator.MoveNext())
        {
            string current = enumerator.Current;

            if (!current.StartsWith("--", StringComparison.Ordinal))
            {
                positional.Add(current);
                continue;
            }

            string name = current.Substring(2);
            string? inlineValue = null;
            int equals = name.IndexOf('=');
            if (equals >= 0)
            {
                inlineValue = name.Substring(equals + 1);
                name = name.Substring(0, equals);
            }

            if (flagSet.Contains(name))
            {
                if (inlineValue != null)
                {
                    throw new UsageException("Flag --" + name + " does not take a value.");
                }

                options[name] = string.Empty;
                continue;
            }

            string value;
            if (inlineValue != null)
            {
                value = inlineValue;
            }
            else
            {
                if (!enumerator.MoveNext())
                {
                    throw new UsageException("Option --" + name + " requires a value.");
                }

                value = enumerator.Current;
            }

            if (options.TryGetValue(name, out string? existing))
            {
                if (!repeated.Contains(name))
                {
                    throw new UsageException("Option --" + name + " was specified more than once.");
                }

                options[name] = existing + "\u0000" + value;
            }
            else
            {
                options[name] = value;
            }
        }

        return new CommandLine(positional, options, repeated);
    }

    public bool Has(string name) => _options.ContainsKey(name);

    public string? Get(string name) => _options.TryGetValue(name, out string? value) ? value : null;

    public IReadOnlyList<string> GetAll(string name)
    {
        string? value = Get(name);
        if (value == null)
        {
            return Array.Empty<string>();
        }

        return value.Split('\u0000');
    }

    public int GetInt(string name, int fallback)
    {
        string? value = Get(name);
        if (value == null)
        {
            return fallback;
        }

        if (!int.TryParse(value, NumberStyles.None, CultureInfo.InvariantCulture, out int parsed))
        {
            throw new UsageException("Option --" + name + " must be an integer.");
        }

        return parsed;
    }

    public string Require(string name)
    {
        string? value = Get(name);
        if (string.IsNullOrEmpty(value))
        {
            throw new UsageException("Option --" + name + " is required.");
        }

        return value;
    }
}

internal sealed class UsageException : Exception
{
    public UsageException(string message)
        : base(message)
    {
    }
}
