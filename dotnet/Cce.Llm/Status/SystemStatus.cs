using System.Text;
using System.Text.Json;
using CNET.Cce.Llm.Memory;

namespace CNET.Cce.Llm.Status;

/// <summary>
/// A single, read-only snapshot of the whole ghost stack, gathered from files
/// on disk — no locks taken, safe while everything is live. The point is to
/// make the compounding visible: memory growing, records certified past the
/// LM, tools forged, taste learned, the orchestrator's recent decisions. If
/// "it gets better" is true, these numbers move over a week; this is how you
/// watch them.
/// </summary>
public static class SystemStatus
{
    public sealed record Snapshot(
        int MemoryBlobs, int DeadLines, long MaxSeenId,
        int Documents, int Summaries, int Sessions,
        int CertifiedCorr, int CertifiedVrf, int CertifiedObs, int CertifiedOther,
        int DeclarativeTools, int Scriptlets,
        int JudgeExemplars, double[] JudgeWeights,
        int InboxLines, IReadOnlyList<string> RecentDecisions);

    /// <summary>Gathers the snapshot. Missing files count as zero — a fresh
    /// system reports honestly, never throws.</summary>
    public static Snapshot Gather(string storePath, string? gapInbox, string stateDir)
    {
        GhostSnapshot store = SafeSnapshot(storePath);
        var blobs = store.All();

        int docs = blobs.Count(b => b.Role == "doc");
        int summaries = blobs.Count(b => b.Role == "summary");
        int sessions = blobs
            .Where(b => !b.SessionId.StartsWith("doc:", StringComparison.Ordinal) &&
                        !b.SessionId.StartsWith("summary:", StringComparison.Ordinal))
            .Select(b => b.SessionId).Distinct().Count();

        // Certified units by family, from the ledger beside the inbox.
        int corr = 0, vrf = 0, obs = 0, other = 0;
        if (gapInbox is not null && gapInbox.EndsWith(".inbox", StringComparison.Ordinal))
        {
            string ledger = gapInbox[..^".inbox".Length] + ".gaps.txt";
            if (File.Exists(ledger))
            {
                foreach (string line in File.ReadLines(ledger))
                {
                    if (!line.Contains("acq_skill_", StringComparison.Ordinal)) continue;
                    if (line.Contains("skill_corr_", StringComparison.Ordinal)) corr++;
                    else if (line.Contains("skill_vrf_", StringComparison.Ordinal)) vrf++;
                    else if (line.Contains("skill_obs_", StringComparison.Ordinal)) obs++;
                    else other++;
                }
            }
        }

        int inboxLines = gapInbox is not null && File.Exists(gapInbox)
            ? File.ReadLines(gapInbox).Count(l => !string.IsNullOrWhiteSpace(l)) : 0;

        int declTools = CountJsonKeys(Path.Combine(stateDir, "tools.json"));
        int scriptlets = CountJsonKeys(Path.Combine(stateDir, "scriptlets.json"));

        (int exemplars, double[] weights) = ReadJudge(stateDir);
        var decisions = RecentJournal(Path.Combine(stateDir, "orchestrator.journal.jsonl"), 5);

        return new Snapshot(
            blobs.Count, store.DeadLines, store.MaxSeenId,
            docs, summaries, sessions,
            corr, vrf, obs, other,
            declTools, scriptlets,
            exemplars, weights,
            inboxLines, decisions);
    }

    /// <summary>Renders the snapshot as a compact dashboard.</summary>
    public static string Format(Snapshot s)
    {
        var sb = new StringBuilder();
        sb.AppendLine("╭─ ghost status ───────────────────────────────────────────────╮");
        sb.AppendLine($"  memory     {s.MemoryBlobs,4} live blobs   " +
                      $"{s.Sessions} sessions · {s.Documents} doc sections · {s.Summaries} summaries" +
                      (s.DeadLines > 0 ? $"   ({s.DeadLines} dead, janitor pending)" : ""));
        int certified = s.CertifiedCorr + s.CertifiedVrf + s.CertifiedObs + s.CertifiedOther;
        sb.AppendLine($"  learned    {certified,4} certified units  " +
                      $"corr {s.CertifiedCorr} · verified {s.CertifiedVrf} · " +
                      $"observed {s.CertifiedObs} · other {s.CertifiedOther}");
        sb.AppendLine($"  tools      {s.DeclarativeTools + s.Scriptlets,4} certified       " +
                      $"declarative {s.DeclarativeTools} · scriptlets {s.Scriptlets}");
        string trend = s.JudgeWeights.Length >= 4
            ? $"repetition-aversion {s.JudgeWeights[3]:F2}" : "seed";
        sb.AppendLine($"  taste      {s.JudgeExemplars,4} exemplars     {trend}");
        sb.AppendLine($"  inbox      {s.InboxLines,4} pending        (teaching queue)");
        if (s.RecentDecisions.Count > 0)
        {
            sb.AppendLine("  ─ recent orchestrator decisions ─");
            foreach (string d in s.RecentDecisions)
                sb.AppendLine($"    {d}");
        }
        sb.AppendLine("╰──────────────────────────────────────────────────────────────╯");
        return sb.ToString();
    }

    private static GhostSnapshot SafeSnapshot(string path)
    {
        try { return BlobStore.Snapshot(path); }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
        {
            // File-locked or absent: an empty view rather than a crash.
            return BlobStore.Snapshot(path + ".nonexistent");
        }
    }

    private static int CountJsonKeys(string path)
    {
        if (!File.Exists(path)) return 0;
        try
        {
            using JsonDocument doc = JsonDocument.Parse(File.ReadAllText(path));
            return doc.RootElement.ValueKind == JsonValueKind.Object
                ? doc.RootElement.EnumerateObject().Count() : 0;
        }
        catch (JsonException) { return 0; }
    }

    private static (int Exemplars, double[] Weights) ReadJudge(string stateDir)
    {
        string evidence = Path.Combine(stateDir, "judgment.evidence.jsonl");
        int count = File.Exists(evidence)
            ? File.ReadLines(evidence).Count(l => !string.IsNullOrWhiteSpace(l)) : 0;
        double[] weights = [];
        string wpath = Path.Combine(stateDir, "judgment.weights.json");
        if (File.Exists(wpath))
        {
            try { weights = JsonSerializer.Deserialize<double[]>(File.ReadAllText(wpath)) ?? []; }
            catch (JsonException) { }
        }
        return (count, weights);
    }

    private static List<string> RecentJournal(string path, int n)
    {
        var lines = new List<string>();
        if (!File.Exists(path)) return lines;
        foreach (string line in File.ReadLines(path).Reverse().Take(n))
        {
            try
            {
                using JsonDocument doc = JsonDocument.Parse(line);
                JsonElement r = doc.RootElement;
                string ts = r.TryGetProperty("TimestampUtc", out var t)
                    ? (t.GetString() ?? "")[..Math.Min(16, (t.GetString() ?? "").Length)] : "";
                string action = r.TryGetProperty("Action", out var a) ? a.GetString() ?? "" : "";
                string outcome = r.TryGetProperty("Outcome", out var o) ? o.GetString() ?? "" : "";
                if (outcome.Length > 60) outcome = outcome[..60] + "…";
                lines.Add($"{ts.Replace("T", " ")}  {action}: {outcome}");
            }
            catch (JsonException) { }
        }
        return lines;
    }
}
