using System;
using System.Text.Json;

namespace CNET.Cce;

/// <summary>
/// Closed-set JSON tool-call bridge (v0).
///
/// Host owns free-form JSON via System.Text.Json.
/// CNET owns a certified classifier: keyword features → tool ONEHOT.
/// Must stay alphabet-aligned with native <c>src/json_toolcall.c</c>.
/// </summary>
public static class JsonToolCall
{
    public const string UnitName = "json_toolcall_v0";
    public const string InputTag = "jtc_feat";
    public const string GoalTag = "json_tool";
    public const int FeatureCount = 16;
    public const int ToolCount = 6;

    /// <summary>Port family values matching native PortFamily (nn.h).</summary>
    public const int PortRaw = 0;     // PORT_RAW
    public const int PortOneHot = 1;  // PORT_ONEHOT

    public static readonly string[] ToolNames =
    {
        "calculator", "memory_store", "memory_recall",
        "file_read", "cnet_recall", "final"
    };

    /// <summary>Keyword alphabet (order = feature index). Keep in sync with C.</summary>
    public static readonly string[] FeatureNames =
    {
        "calculator", "memory_store", "memory_recall", "file_read",
        "cnet_recall", "final", "expr", "key", "value", "query",
        "path", "cond", "current", "answer", "tool", "args"
    };

    public static readonly string[] ExampleJson =
    {
        """{"tool":"calculator","args":{"expr":"23 * 19"}}""",
        """{"tool":"memory_store","args":{"key":"k","value":"v"}}""",
        """{"tool":"memory_recall","args":{"query":"k"}}""",
        """{"tool":"file_read","args":{"path":"readme.txt"}}""",
        """{"tool":"cnet_recall","args":{"cond":0,"current":1}}""",
        """{"final":"done","answer":"ok"}"""
    };

    /// <summary>Encode JSON text into the closed feature vector (0/1 doubles).</summary>
    public static double[] Encode(string? jsonText)
    {
        var feat = new double[FeatureCount];
        var s = jsonText ?? "";
        for (int i = 0; i < FeatureCount; i++)
            feat[i] = s.Contains(FeatureNames[i], StringComparison.OrdinalIgnoreCase) ? 1.0 : 0.0;
        return feat;
    }

    /// <summary>Argmax of tool one-hot → tool name, or null.</summary>
    public static string? DecodeTool(ReadOnlySpan<double> onehot)
    {
        if (onehot.Length < ToolCount) return null;
        int best = 0;
        for (int i = 1; i < ToolCount; i++)
            if (onehot[i] > onehot[best]) best = i;
        return ToolNames[best];
    }

    public static int DecodeToolId(ReadOnlySpan<double> onehot)
    {
        if (onehot.Length < ToolCount) return -1;
        int best = 0;
        for (int i = 1; i < ToolCount; i++)
            if (onehot[i] > onehot[best]) best = i;
        return best;
    }

    /// <summary>
    /// Host-side parse of a tool-call JSON object (Agent protocol).
    /// Does not replace CNET certification — use <see cref="Classify"/> for the sealed skill.
    /// </summary>
    public static bool TryParseAgentJson(string json, out string? tool, out JsonElement args, out string? final)
    {
        tool = null;
        final = null;
        args = default;
        try
        {
            using var doc = JsonDocument.Parse(json);
            var root = doc.RootElement;
            if (root.TryGetProperty("final", out var fin))
            {
                final = fin.ValueKind == JsonValueKind.String ? fin.GetString() : fin.GetRawText();
                tool = "final";
            }
            if (root.TryGetProperty("tool", out var t))
                tool = t.GetString();
            if (root.TryGetProperty("args", out var a))
                args = a.Clone();
            return tool != null || final != null;
        }
        catch (JsonException)
        {
            return false;
        }
    }

    /// <summary>
    /// Run the certified <c>json_toolcall_v0</c> unit on a sealed SoulHost base.
    /// Throws if the unit is absent (mine/seal spine first).
    /// </summary>
    public static string Classify(SoulHost soul, string jsonText)
    {
        ArgumentNullException.ThrowIfNull(soul);
        var feat = Encode(jsonText);
        var output = soul.RunUnit(UnitName, feat);
        return DecodeTool(output) ?? "final";
    }

    /// <summary>
    /// Route by typed ports (jtc_feat → json_tool) without requiring the unit name.
    /// </summary>
    public static string ClassifyByRoute(SoulHost soul, string jsonText)
    {
        ArgumentNullException.ThrowIfNull(soul);
        var feat = Encode(jsonText);
        var output = soul.Route(GoalTag, feat, outputSize: ToolCount);
        return DecodeTool(output) ?? "final";
    }

    /// <summary>
    /// Capability / serve probe via soul_request ABI (certified vs gap).
    /// </summary>
    public static (bool Served, string Source, string? Tool) RequestClassify(
        SoulHost soul, string jsonText)
    {
        ArgumentNullException.ThrowIfNull(soul);
        var feat = Encode(jsonText);
        var (served, _, _, source, output) = soul.Request(
            inFamily: PortRaw, inWidth: FeatureCount, inCount: 1, inTag: InputTag,
            goalFamily: PortOneHot, goalWidth: ToolCount, goalCount: 1, goalTag: GoalTag,
            input: feat);
        if (!served || output == null) return (false, source, null);
        return (true, source, DecodeTool(output));
    }
}
