using System;
using System.Text;

namespace Msixpkg.Packer;

internal static class Program
{
    private static int Main(string[] args)
    {
        // 让中文与错误码在任何控制台代码页下都能正确输出。
        Console.OutputEncoding = Encoding.UTF8;
        Console.InputEncoding = Encoding.UTF8;

        var application = new PackerApplication(Console.Out, Console.Error);
        return application.Run(args);
    }
}
