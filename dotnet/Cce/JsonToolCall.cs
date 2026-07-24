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

    private static readonly object TrafficLogLock = new();

    /// <summary>Append a tool-call request to the recorded-traffic log. No-op
    /// unless CNET_JTC_TRAFFIC_LOG is set (read per call so it stays runtime-
    /// configurable). One single-line JSON record per line, so the personal-AI
    /// lane can replay real host traffic for fault-mining and adapter
    /// certification. Failures never affect serving.</summary>
    public static void LogTraffic(string? jsonText)
    {
        var path = Environment.GetEnvironmentVariable("CNET_JTC_TRAFFIC_LOG");
        if (string.IsNullOrEmpty(path) || string.IsNullOrEmpty(jsonText)) return;
        try
        {
            string line = jsonText.Replace('\r', ' ').Replace('\n', ' ').Trim();
            if (line.Length == 0) return;
            lock (TrafficLogLock) File.AppendAllText(path, line + "\n");
        }
        catch { /* recording must never break serving */ }
    }

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

    /// <summary>Argmax + margin confidence in [0,1] (max - second_max, floored by max).</summary>
    public static (string? Tool, double Confidence, double MaxAct) DecodeToolScored(ReadOnlySpan<double> onehot)
    {
        if (onehot.Length < ToolCount) return (null, 0, 0);
        int best = 0, second = 0;
        for (int i = 1; i < ToolCount; i++)
        {
            if (onehot[i] > onehot[best]) { second = best; best = i; }
            else if (onehot[i] > onehot[second] || second == best) second = i;
        }
        double max = onehot[best];
        double sec = best == second ? 0 : onehot[second];
        double conf = max <= 0 ? 0 : Math.Min(1.0, Math.Max(0.0, max - sec + max * 0.25));
        if (max < 1e-9) conf = 0;
        return (ToolNames[best], conf, max);
    }

    /// <summary>CNET_JTC_MIN_CONF (default 0.20). Below → refuse as unknown.</summary>
    public static double MinConfidence
    {
        get
        {
            var e = Environment.GetEnvironmentVariable("CNET_JTC_MIN_CONF");
            if (string.IsNullOrEmpty(e)) return 0.20;
            return double.TryParse(e, System.Globalization.NumberStyles.Float,
                System.Globalization.CultureInfo.InvariantCulture, out var v)
                ? Math.Clamp(v, 0, 1) : 0.20;
        }
    }

    /// <summary>
    /// If JSON names an explicit tool not in the closed set → unknown.
    /// Empty feature vector → unknown. Low confidence → unknown.
    /// </summary>
    public static bool TryExplicitUnknown(string? jsonText, out string? named)
    {
        named = null;
        if (string.IsNullOrWhiteSpace(jsonText)) return false;
        if (!TryParseAgentJson(jsonText, out var tool, out _, out _)) return false;
        if (string.IsNullOrEmpty(tool)) return false;
        named = tool;
        return !IsKnownTool(tool);
    }

    public static bool FeaturesEmpty(ReadOnlySpan<double> feat)
    {
        for (int i = 0; i < feat.Length; i++)
            if (feat[i] > 0.5) return false;
        return true;
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

    /// <summary>
    /// Append a NO_PLAN line for jtc_feat→json_tool so the gap lane can see demand.
    /// </summary>
    public static bool NoteGap(string? inboxPath)
    {
        if (string.IsNullOrWhiteSpace(inboxPath)) return false;
        try
        {
            var dir = Path.GetDirectoryName(Path.GetFullPath(inboxPath));
            if (!string.IsNullOrEmpty(dir)) Directory.CreateDirectory(dir);
            File.AppendAllText(inboxPath,
                $"NO_PLAN {PortRaw} {FeatureCount} 1 {InputTag} {PortOneHot} {ToolCount} 1 {GoalTag}\n");
            return true;
        }
        catch
        {
            return false;
        }
    }

    public static (bool Served, string Source, string? Tool, bool GapNoted) RequestClassify(
        SoulHost soul, string jsonText)
    {
        ArgumentNullException.ThrowIfNull(soul);
        if (TryExplicitUnknown(jsonText, out var named))
        {
            bool noted = NoteGap(null); // caller path notes with inbox
            return (false, "unknown_tool", named, noted);
        }
        var feat = Encode(jsonText);
        if (FeaturesEmpty(feat))
            return (false, "unknown_empty", null, false);
        var (served, gapNoted, residual, source, output) = soul.Request(
            inFamily: PortRaw, inWidth: FeatureCount, inCount: 1, inTag: InputTag,
            goalFamily: PortOneHot, goalWidth: ToolCount, goalCount: 1, goalTag: GoalTag,
            input: feat);
        if (!served || output == null)
            return (false, source, null, gapNoted || residual);
        var (tool, conf, maxAct) = DecodeToolScored(output);
        if (tool == null || conf < MinConfidence || maxAct < MinConfidence * 0.5)
            return (false, "unknown_low_conf", tool, gapNoted || residual);
        return (true, source, tool, gapNoted || residual);
    }

    /// <summary>
    /// Classify with certified unit when present; on miss/error note gap and return null.
    /// Unknown tools / empty / low-confidence → tool null, source starts with unknown_.
    /// </summary>
    public static (string? Tool, string Source, bool GapNoted) ClassifyOrGap(
        SoulHost? soul, string jsonText, string? inboxPath = null)
    {
        LogTraffic(jsonText);   // capture the production request stream (env-gated)
        if (soul == null)
        {
            bool noted = NoteGap(inboxPath);
            return (null, "none", noted);
        }
        try
        {
            if (TryExplicitUnknown(jsonText, out _))
            {
                bool noted = NoteGap(inboxPath);
                return (null, "unknown_tool", noted);
            }
            var feat = Encode(jsonText);
            if (FeaturesEmpty(feat))
            {
                bool noted = NoteGap(inboxPath);
                return (null, "unknown_empty", noted);
            }
            var (served, source, tool, gapNoted) = RequestClassify(soul, jsonText);
            if (source.StartsWith("unknown", StringComparison.Ordinal))
            {
                bool noted = NoteGap(inboxPath) || gapNoted;
                try { FaultBus.Append(UnitName, source: "jtc", note: source, input: feat); }
                catch { /* ignore */ }
                return (null, source, noted);
            }
            if (served && tool != null)
                return (tool, source, gapNoted);
            // Fall back to named unit if request path failed but unit exists
            try
            {
                string t = Classify(soul, jsonText);
                var outVec = soul.RunUnit(UnitName, feat);
                var (_, conf, maxAct) = DecodeToolScored(outVec);
                if (conf < MinConfidence || maxAct < MinConfidence * 0.5)
                {
                    bool noted = NoteGap(inboxPath);
                    try { FaultBus.Append(UnitName, source: "jtc", note: "unknown_low_conf", input: feat); }
                    catch { /* ignore */ }
                    return (null, "unknown_low_conf", noted);
                }
                return (t, "certified", false);
            }
            catch
            {
                bool noted = NoteGap(inboxPath) || gapNoted;
                try { FaultBus.Append(UnitName, source: "jtc", note: "error_fallback", input: feat); }
                catch { /* ignore */ }
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
