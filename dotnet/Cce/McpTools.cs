using System;
using System.Text;
using CNET.Cce.Interop;

namespace CNET.Cce;

/// <summary>
/// Public wrappers over the REAL C MCP tools in cnet.so (calculator, file read,
/// persistent fact memory). These are the genuine sandboxed implementations an
/// agent controller calls — not simulations.
/// </summary>
public static class McpTools
{
    public static void MemoryInit() => CceNative.McpMemoryInit();

    public static string Calculator(string expr)
    {
        var buf = new byte[256];
        CceNative.McpCalculator(expr, buf, (nuint)buf.Length, out _);
        return Decode(buf);
    }

    public static string FileRead(string path)
    {
        var buf = new byte[2048];
        CceNative.McpFileRead(path, buf, (nuint)buf.Length, out _);
        return Decode(buf);
    }

    public static string Recall(string query)
    {
        var buf = new byte[512];
        CceNative.McpRecallFact(query, buf, (nuint)buf.Length);
        var s = Decode(buf);
        return string.IsNullOrWhiteSpace(s) ? "(no fact stored for that query)" : s;
    }

    public static void Memorize(string key, string value) =>
        CceNative.McpMemorizeFact(key, value);

    private static string Decode(byte[] b)
    {
        int n = Array.IndexOf(b, (byte)0);
        return Encoding.UTF8.GetString(b, 0, n < 0 ? b.Length : n).Trim();
    }
}
