using System;
using System.IO;
using System.Text.Json;

namespace CNET.Cce;

/// <summary>
/// Closed-set JSON tool-call bridge (v0).
/// Host owns free-form JSON; CNET owns certified keyword→tool classification.
/// Alphabet: config/json_toolcall_v0.json → gen_json_toolcall_alphabet.py.
/// </summary>
public static partial class JsonToolCall
{
    public const string UnitName = UnitNameGen;
    public const string InputTag = InputTagGen;
    public const string GoalTag = GoalTagGen;
    public const int FeatureCount = FeatureCountGen;
    public const int ToolCount = ToolCountGen;

    /// <summary>Port family values matching native PortFamily (nn.h).</summary>
    public const int PortRaw = 0;
    public const int PortOneHot = 1;

    public static string[] ToolNames => ToolNamesGen;
    public static string[] FeatureNames => FeatureNamesGen;
    public static string[] ExampleJson => ExampleJsonGen;

    /// <summary>Encode JSON text into the closed feature vector (0/1 doubles).</summary>
    public static double[] Encode(string? jsonText)
    {
        var feat = new double[FeatureCount];
        var s = jsonText ?? "";
        for (int i = 0; i < FeatureCount; i++)
            feat[i] = s.Contains(FeatureNames[i], StringComparison.OrdinalIgnoreCase) ? 1.0 : 0.0;
        return feat;
    }

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

    public static bool IsKnownTool(string? tool)
    {
        if (string.IsNullOrEmpty(tool)) return false;
        foreach (var t in ToolNames)
            if (string.Equals(t, tool, StringComparison.OrdinalIgnoreCase))
                return true;
        return tool is "finish" or "answer"; // aliases for final
    }

    public static string NormalizeTool(string? tool)
    {
        if (string.IsNullOrEmpty(tool)) return "final";
        if (tool is "finish" or "answer") return "final";
        foreach (var t in ToolNames)
            if (string.Equals(t, tool, StringComparison.OrdinalIgnoreCase))
                return t;
        return tool;
    }

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

    public static string Classify(SoulHost soul, string jsonText)
    {
        ArgumentNullException.ThrowIfNull(soul);
        var feat = Encode(jsonText);
        var output = soul.RunUnit(UnitName, feat);
        return DecodeTool(output) ?? "final";
    }

    public static string ClassifyByRoute(SoulHost soul, string jsonText)
    {
        ArgumentNullException.ThrowIfNull(soul);
        var feat = Encode(jsonText);
        var output = soul.Route(GoalTag, feat, outputSize: ToolCount);
        return DecodeTool(output) ?? "final";
    }

    public static (bool Served, string Source, string? Tool, bool GapNoted) RequestClassify(
        SoulHost soul, string jsonText)
    {
        ArgumentNullException.ThrowIfNull(soul);
        var feat = Encode(jsonText);
        var (served, gapNoted, residual, source, output) = soul.Request(
            inFamily: PortRaw, inWidth: FeatureCount, inCount: 1, inTag: InputTag,
            goalFamily: PortOneHot, goalWidth: ToolCount, goalCount: 1, goalTag: GoalTag,
            input: feat);
        if (!served || output == null)
            return (false, source, null, gapNoted || residual);
        return (true, source, DecodeTool(output), gapNoted || residual);
    }

    /// <summary>
    /// Append a NO_PLAN line for jtc_feat→json_tool so the gap lane can see demand.
    /// Format matches gap_inbox_note_no_plan (PORT_RAW=0, PORT_ONEHOT=1).
    /// </summary>
    public static bool NoteGap(string? inboxPath)
    {
        if (string.IsNullOrWhiteSpace(inboxPath)) return false;
        try
        {
            Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(inboxPath))!);
            // NO_PLAN fam width count tag fam width count tag
            File.AppendAllText(inboxPath,
                $"NO_PLAN {PortRaw} {FeatureCount} 1 {InputTag} {PortOneHot} {ToolCount} 1 {GoalTag}\n");
            return true;
        }
        catch
        {
            return false;
        }
    }

    /// <summary>
    /// Classify with certified unit when present; on miss/error note gap and return null.
    /// </summary>
    public static (string? Tool, string Source, bool GapNoted) ClassifyOrGap(
        SoulHost? soul, string jsonText, string? inboxPath = null)
    {
        if (soul == null)
        {
            bool noted = NoteGap(inboxPath);
            return (null, "none", noted);
        }
        try
        {
            var (served, source, tool, gapNoted) = RequestClassify(soul, jsonText);
            if (served && tool != null)
                return (tool, source, gapNoted);
            // Fall back to named unit if request path failed but unit exists
            try
            {
                string t = Classify(soul, jsonText);
                return (t, "certified", false);
            }
            catch
            {
                bool noted = NoteGap(inboxPath) || gapNoted;
                return (null, source, noted);
            }
        }
        catch
        {
            bool noted = NoteGap(inboxPath);
            return (null, "error", noted);
        }
    }
}
