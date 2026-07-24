using System.Text;

namespace CNET.Cce.Llm;

/// <summary>
/// Present CNET skill / certified serves as human text for Ghost — not raw pick indices.
/// </summary>
public static class SkillPresentation
{
    /// <summary>
    /// Build a short block Ghost can inject when a certified unit or skill answered.
    /// </summary>
    public static string FormatCertified(string unitOrSkill, IReadOnlyList<int>? picks,
                                         string? decoded, string? query = null)
    {
        var sb = new StringBuilder();
        sb.Append("·certified ");
        sb.Append(unitOrSkill);
        if (!string.IsNullOrWhiteSpace(query))
        {
            sb.Append(" ·q=");
            sb.Append(Trunc(query!, 48));
        }
        if (!string.IsNullOrWhiteSpace(decoded))
        {
            sb.AppendLine();
            sb.Append(decoded);
        }
        else if (picks is { Count: > 0 })
        {
            sb.AppendLine();
            sb.Append("picks [");
            sb.Append(string.Join(", ", picks));
            sb.Append(']');
        }
        return sb.ToString();
    }

    /// <summary>
    /// Map pick indices through optional alphabet labels.
    /// </summary>
    public static string DecodePicks(IReadOnlyList<int> picks, IReadOnlyList<string>? labels)
    {
        if (picks.Count == 0) return "";
        var parts = new List<string>(picks.Count);
        foreach (int p in picks)
        {
            if (labels != null && p >= 0 && p < labels.Count && !string.IsNullOrEmpty(labels[p]))
                parts.Add(labels[p]);
            else
                parts.Add(p.ToString());
        }
        return string.Join(", ", parts);
    }

    static string Trunc(string s, int n) => s.Length <= n ? s : s[..n] + "…";
}
