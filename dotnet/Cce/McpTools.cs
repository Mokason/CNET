using System;
using System.Text;
using CNET.Cce.Interop;

namespace CNET.Cce;

/// <summary>
/// Public wrappers over the REAL C MCP tools in cnet.so (calculator, file read,
/// web/wiki lookup, persistent fact memory). These are the genuine sandboxed
/// implementations an agent controller calls — not simulations.
/// </summary>
public static class McpTools
{
    public static void MemoryInit() => CceNative.McpMemoryInit();

    /// <summary>Math eval: + - * / ^ %, sqrt/abs/floor/ceil, parentheses (via C math_eval).</summary>
    public static string Calculator(string expr)
    {
        var buf = new byte[512];
        CceNative.McpCalculator(expr, buf, (nuint)buf.Length, out _);
        return Decode(buf);
    }

    public static string FileRead(string path)
    {
        var buf = new byte[2048];
        CceNative.McpFileRead(path, buf, (nuint)buf.Length, out _);
        return Decode(buf);
    }

    /// <summary>DuckDuckGo Instant Answer with Wikipedia fallback; memory-cached.</summary>
    public static string WebSearch(string query)
    {
        var buf = new byte[2048];
        CceNative.McpWebSearch(query ?? "", buf, (nuint)buf.Length, out var fromCache);
        var s = Decode(buf);
        if (string.IsNullOrWhiteSpace(s))
            return "(no web results)";
        return fromCache != 0 ? s + " [cached]" : s;
    }

    /// <summary>Wikipedia page summary; memory-cached.</summary>
    public static string WikiLookup(string query)
    {
        var buf = new byte[2048];
        CceNative.McpWikiLookup(query ?? "", buf, (nuint)buf.Length, out var fromMem);
        var s = Decode(buf);
        if (string.IsNullOrWhiteSpace(s) || s == "LOOKUP_FAILED")
            return "(wiki lookup failed)";
        return fromMem != 0 ? s + " [cached]" : s;
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
