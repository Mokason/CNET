using System.Text.Json;
using CNET.Cce.Llm.Memory;

namespace CNET.Cce.Llm.Routing;

/// <summary>A certified serve: which unit answered and from what evidence.</summary>
/// <param name="UnitName">The sealed specialist (ledger-verified CLOSED).</param>
/// <param name="RecordText">The record the unit was certified to reproduce —
/// serving it verbatim is serving exactly what the weights encode, with the
/// certificate as the warrant.</param>
/// <param name="GroundedTerms">The whole-word matches that grounded the route.</param>
public sealed record CertifiedServe(string UnitName, string RecordText,
                                    IReadOnlyList<string> GroundedTerms);

/// <summary>
/// Rung 4: routing across answer sources — certified knowledge before model
/// opinion. A question routes to a record-taught specialist only when
/// AICIMO's model-free guards pass (ported from SkillRouterDriver): the
/// record's distinctive terms must be whole-word present in the question
/// (subject grounding), the unit must be ledger-certified CLOSED, and its
/// serving reliability must be intact. Everything else declines — the
/// AICIMO rule again: a wrong confident answer is worse than escalating.
/// </summary>
/// <remarks>
/// The learned part is the reliability loop: every certified serve is
/// counted; a correction-shaped user turn immediately after one demotes the
/// unit (served-corrected/served drops below <see cref="MinReliability"/> →
/// it stops serving and the model takes over again). Outcomes, not opinions,
/// adjust the routing — the same stance as everything else on this ladder.
/// State is a small JSON sidecar next to the records dir, so the orchestrator
/// and every session share one view of which units have earned trust.
/// </remarks>
public sealed class RecordRouter
{
    private sealed class UnitStats
    {
        public int Served { get; set; }
        public int Corrected { get; set; }
    }

    private readonly string _recordsDir;
    private readonly string _ledgerPath;
    private readonly string _statsPath;
    private readonly Dictionary<string, UnitStats> _stats;
    private string? _lastServedUnit;

    /// <summary>Distinct grounded terms required to route (subject grounding).</summary>
    public int MinGroundedTerms { get; init; } = 3;

    /// <summary>Below this served-vs-corrected ratio a unit stops serving.</summary>
    public double MinReliability { get; init; } = 0.6;

    public RecordRouter(string recordsDir, string ledgerPath)
    {
        _recordsDir = recordsDir;
        _ledgerPath = ledgerPath;
        _statsPath = Path.Combine(recordsDir, "routing.json");
        _stats = Load(_statsPath);
    }

    /// <summary>
    /// Routes a question to a certified record, or declines. Declining is the
    /// default and the common case.
    /// </summary>
    public CertifiedServe? TryRoute(string question)
    {
        if (!Directory.Exists(_recordsDir) || !File.Exists(_ledgerPath)) return null;

        HashSet<string> questionWords = Words(question);
        CertifiedServe? best = null;
        int bestScore = 0;

        foreach (string path in Directory.GetFiles(_recordsDir, "skill_*.txt").Order())
        {
            string tag = Path.GetFileNameWithoutExtension(path);
            string unit = "acq_" + tag;
            if (!LedgerCertifiesClosed(tag, unit)) continue;

            var stats = _stats.GetValueOrDefault(unit);
            if (stats is { Served: > 0 } &&
                (double)(stats.Served - stats.Corrected) / stats.Served < MinReliability)
                continue;   // demoted: outcomes revoked its routing privilege

            // Subject grounding (AICIMO ArgumentGrounded): the record's
            // DISTINCTIVE words (>=4 chars — mirrors the recall gate's stance)
            // must appear whole-word in the question.
            string record = File.ReadAllText(path);
            var grounded = Words(record).Where(w => w.Length >= 4)
                                        .Where(questionWords.Contains)
                                        .Distinct().Order().ToList();
            if (grounded.Count < MinGroundedTerms) continue;
            if (grounded.Count > bestScore)
            {
                bestScore = grounded.Count;
                best = new CertifiedServe(unit, StripMarker(record), grounded);
            }
        }

        if (best is not null)
        {
            var s = _stats.TryGetValue(best.UnitName, out var cur)
                ? cur : _stats[best.UnitName] = new UnitStats();
            s.Served++;
            _lastServedUnit = best.UnitName;
            Save();
        }
        return best;
    }

    /// <summary>
    /// Outcome feedback: call with each user turn. A correction-shaped turn
    /// right after a certified serve demotes that unit — the router learns
    /// from consequences, per-unit, durably.
    /// </summary>
    public void ObserveUserTurn(string userText)
    {
        if (_lastServedUnit is null) return;
        string head = userText.TrimStart().ToLowerInvariant();
        string[] openers = ["wrong", "no,", "no.", "nope", "incorrect",
                            "that's wrong", "thats wrong", "that's not", "thats not",
                            "actually,", "i said", "i meant", "not true"];
        if (openers.Any(head.StartsWith))
        {
            _stats[_lastServedUnit].Corrected++;
            Save();
        }
        _lastServedUnit = null;   // feedback window is exactly one turn
    }

    /// <summary>Serve/corrected counters per unit, for receipts and the journal.</summary>
    public IReadOnlyDictionary<string, (int Served, int Corrected)> Stats =>
        _stats.ToDictionary(kv => kv.Key, kv => (kv.Value.Served, kv.Value.Corrected));

    // ── plumbing ──

    /// <summary>The ledger line must show the tag CLOSED with our unit minted —
    /// certification is the routing warrant, not the record file's existence.</summary>
    private bool LedgerCertifiesClosed(string tag, string unit)
    {
        foreach (string line in File.ReadLines(_ledgerPath))
            if (line.Contains(" " + tag + " ", StringComparison.Ordinal) &&
                line.Contains(unit, StringComparison.Ordinal))
                return true;
        return false;
    }

    private static string StripMarker(string record)
    {
        // Short "#..." format-version markers ("#v2") are not content.
        string[] lines = record.Split('\n');
        return string.Join("\n",
            lines.Where(l => !(l.StartsWith('#') && l.Trim().Length <= 4))).Trim();
    }

    private static HashSet<string> Words(string text)
    {
        var words = new HashSet<string>(StringComparer.Ordinal);
        var word = new System.Text.StringBuilder();
        foreach (char c in text)
        {
            if (char.IsAsciiLetterOrDigit(c)) { word.Append(char.ToLowerInvariant(c)); continue; }
            if (word.Length > 0) { words.Add(word.ToString()); word.Clear(); }
        }
        if (word.Length > 0) words.Add(word.ToString());
        return words;
    }

    private static Dictionary<string, UnitStats> Load(string path)
    {
        if (!File.Exists(path)) return [];
        try
        {
            return JsonSerializer.Deserialize<Dictionary<string, UnitStats>>(
                File.ReadAllText(path)) ?? [];
        }
        catch (JsonException) { return []; }
    }

    private void Save()
    {
        Directory.CreateDirectory(_recordsDir);
        string tmp = _statsPath + ".tmp";
        File.WriteAllText(tmp, JsonSerializer.Serialize(_stats));
        File.Move(tmp, _statsPath, overwrite: true);
    }
}
