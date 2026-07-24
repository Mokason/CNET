using System;
using System.Globalization;
using System.IO;
using System.Text;

namespace CNET.Cce;

/// <summary>
/// Thin writer for the native unified fault bus (CNET_FAULT_LOG JSONL).
/// Labeled vector lines are primarily produced by native
/// <c>cnet_fault_mirror_labeled</c> on <c>registry_supply_label</c>.
/// This host path records Ghost/MCP/JTC *miss breadcrumbs* (and optional
/// feature vectors) so ops can see demand even before a teacher label exists.
/// </summary>
public static class FaultBus
{
    private static readonly object Lock = new();

    /// <summary>
    /// Append one JSONL record. No-op unless CNET_FAULT_LOG is set.
    /// When <paramref name="input"/> and <paramref name="target"/> are both
    /// non-null and dims match, emits labeled vectors consumable by
    /// registry_lora_ingest_fault_bus.
    /// </summary>
    public static void Append(
        string unit,
        string source = "ghost",
        string? skill = null,
        string? session = null,
        string? note = null,
        double[]? input = null,
        double[]? target = null)
    {
        var path = Environment.GetEnvironmentVariable("CNET_FAULT_LOG");
        if (string.IsNullOrEmpty(path) || string.IsNullOrEmpty(unit)) return;
        try
        {
            var ts = DateTimeOffset.UtcNow.ToUnixTimeSeconds();
            var sb = new StringBuilder(256);
            sb.Append('{');
            sb.Append(CultureInfo.InvariantCulture, $"\"ts\":{ts}");
            sb.Append(CultureInfo.InvariantCulture, $",\"source\":{JsonStr(source)}");
            sb.Append(CultureInfo.InvariantCulture, $",\"unit\":{JsonStr(unit)}");
            sb.Append(CultureInfo.InvariantCulture, $",\"skill\":{JsonStr(skill ?? "")}");
            sb.Append(CultureInfo.InvariantCulture, $",\"session\":{JsonStr(session ?? "")}");
            int inDim = input?.Length ?? 0;
            int outDim = target?.Length ?? 0;
            sb.Append(CultureInfo.InvariantCulture, $",\"in_dim\":{inDim}");
            sb.Append(CultureInfo.InvariantCulture, $",\"out_dim\":{outDim}");
            sb.Append(",\"label_kind\":\"argmax\"");
            sb.Append(CultureInfo.InvariantCulture, $",\"note\":{JsonStr(note ?? "host")}");
            if (input is { Length: > 0 } && target is { Length: > 0 })
            {
                AppendVec(sb, "in", input);
                AppendVec(sb, "tgt", target);
            }
            sb.Append('}');
            sb.Append('\n');
            lock (Lock)
            {
                var dir = Path.GetDirectoryName(path);
                if (!string.IsNullOrEmpty(dir)) Directory.CreateDirectory(dir);
                File.AppendAllText(path, sb.ToString());
            }
        }
        catch
        {
            /* never affect serving */
        }
    }

    private static string JsonStr(string s)
    {
        return "\"" + s.Replace("\\", "\\\\", StringComparison.Ordinal)
                       .Replace("\"", "\\\"", StringComparison.Ordinal) + "\"";
    }

    private static void AppendVec(StringBuilder sb, string key, double[] v)
    {
        sb.Append(CultureInfo.InvariantCulture, $",\"{key}\":[");
        for (int i = 0; i < v.Length; i++)
        {
            if (i > 0) sb.Append(',');
            sb.Append(v[i].ToString("G9", CultureInfo.InvariantCulture));
        }
        sb.Append(']');
    }
}
