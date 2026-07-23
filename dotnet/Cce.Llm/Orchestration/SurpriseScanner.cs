using CNET.Cce.Llm.Memory;

namespace CNET.Cce.Llm.Orchestration;

/// <summary>One surprising observation: taught knowledge disagreed with, or
/// was silent about, what conversation actually did.</summary>
/// <param name="Kind">"contradiction" (taught A→B, observed A→C) or
/// "novelty" (recurring transition no record teaches).</param>
/// <param name="FromWord">The window word the transition leaves.</param>
/// <param name="TaughtWord">What the records teach follows (contradictions only).</param>
/// <param name="ObservedWord">What conversation actually did.</param>
/// <param name="Occurrences">How often it was observed (novelty gate).</param>
/// <param name="EvidenceBlobIds">Provenance: where it was seen.</param>
public sealed record Surprise(
    string Kind, string FromWord, string? TaughtWord, string ObservedWord,
    int Occurrences, IReadOnlyList<long> EvidenceBlobIds);

/// <summary>
/// Rung 3: the system chooses what to learn next from where its predictions
/// fail. The taught truth is the record corpus (every record file compiled to
/// its transition table — exactly as the native teacher compiles them); the
/// test is fresh conversation. Where they disagree is a CONTRADICTION — the
/// highest-value teaching signal, because revising beats accumulating. Where
/// conversation keeps doing something no record predicts is NOVELTY, gated by
/// recurrence so one-off phrasing never becomes curriculum.
/// </summary>
/// <remarks>
/// This is prediction-error-driven curriculum, not coverage-driven: the
/// existing curiosity module seeds gaps for what the system has never seen;
/// this seeds records for what the system believes WRONGLY or incompletely,
/// with the observed conversation as the certifying record (skill_obs_*).
/// Same anti-parrot property as every record family: the evidence is what was
/// actually said, not what a model predicts.
/// </remarks>
public sealed class SurpriseScanner
{
    private readonly string[] _words;
    private readonly Dictionary<int, int> _taught = [];      // from -> taught successor

    /// <summary>Novelty gate: a transition must recur this often.</summary>
    public int MinOccurrences { get; init; } = 3;

    /// <summary>Novelty gate: across at least this many distinct blobs.</summary>
    public int MinDistinctBlobs { get; init; } = 2;

    /// <summary>Most surprises reported per scan.</summary>
    public int MaxSurprises { get; init; } = 8;

    public SurpriseScanner(string wordsPath, string recordsDir)
    {
        _words = LoadWords(wordsPath);
        // The taught truth: every record's transitions, first-writer-wins in
        // file-name order (deterministic across runs).
        if (Directory.Exists(recordsDir))
        {
            foreach (string path in Directory.GetFiles(recordsDir, "skill_*.txt").Order())
                foreach ((int from, int to) in Transitions(File.ReadAllText(path)))
                    _taught.TryAdd(from, to);
        }
    }

    /// <summary>Scans blobs (newer than the watermark) for surprises.</summary>
    public List<Surprise> Scan(IMemoryView store, long sinceBlobId)
    {
        // observed: (from, to) -> occurrences + evidence
        var observed = new Dictionary<(int From, int To), (int Count, HashSet<long> Blobs)>();
        foreach (MemoryBlob blob in store.All())
        {
            if (blob.Id <= sinceBlobId) continue;
            foreach ((int from, int to) in Transitions(blob.Text))
            {
                var key = (from, to);
                var cur = observed.GetValueOrDefault(key, (0, new HashSet<long>()));
                cur.Item1++;
                cur.Item2.Add(blob.Id);
                observed[key] = cur;
            }
        }

        var surprises = new List<Surprise>();
        foreach (((int from, int to), (int count, HashSet<long> blobs)) in
                 observed.OrderByDescending(kv => kv.Value.Count))
        {
            if (surprises.Count >= MaxSurprises) break;
            if (_taught.TryGetValue(from, out int taught))
            {
                // Contradiction: taught knowledge says otherwise. One
                // observation suffices — a live disagreement is never noise.
                if (taught != to)
                    surprises.Add(new Surprise("contradiction", _words[from],
                        _words[taught], _words[to], count, blobs.ToList()));
            }
            else if (count >= MinOccurrences && blobs.Count >= MinDistinctBlobs)
            {
                surprises.Add(new Surprise("novelty", _words[from], null,
                    _words[to], count, blobs.ToList()));
            }
        }
        return surprises;
    }

    /// <summary>
    /// Turns surprises into observation records: one skill_obs_* record per
    /// scan, containing the evidence blobs' verbatim text — the observed
    /// conversation IS the certifying record.
    /// </summary>
    public (string Name, string Record)? BuildObservationRecord(
        IMemoryView store, IReadOnlyList<Surprise> surprises)
    {
        if (surprises.Count == 0) return null;
        var evidenceIds = surprises.SelectMany(s => s.EvidenceBlobIds)
                                   .Distinct().Order().ToList();
        var texts = evidenceIds.Select(id => store.All().FirstOrDefault(b => b.Id == id))
                               .Where(b => b is not null)
                               .Select(b => b!.Text);
        string record = "#v2\n" + string.Join("\n", texts) + "\n";
        return ($"obs_{Fnv8(record)}", record);
    }

    // ── same tokenization discipline as the native record teacher ──

    private IEnumerable<(int From, int To)> Transitions(string text)
    {
        int prev = -1;
        foreach (string word in Tokenize(text))
        {
            int i = Array.IndexOf(_words, word);
            if (i < 0) continue;                     // out-of-window: skip over
            if (prev >= 0 && prev != i) yield return (prev, i);
            prev = i;
        }
    }

    private static IEnumerable<string> Tokenize(string text)
    {
        var word = new System.Text.StringBuilder();
        foreach (char c in text)
        {
            if (char.IsAsciiLetter(c)) { word.Append(char.ToLowerInvariant(c)); continue; }
            if (word.Length > 0) { yield return word.ToString(); word.Clear(); }
        }
        if (word.Length > 0) yield return word.ToString();
    }

    private static string[] LoadWords(string wordsPath)
    {
        var words = new List<string>();
        foreach (string line in File.ReadLines(wordsPath))
        {
            int i = 0;
            while (i < line.Length && char.IsAsciiDigit(line[i])) i++;
            string word = line[i..].Trim().ToLowerInvariant();
            if (word.Length == 0) throw new FormatException($"malformed words line: '{line}'");
            words.Add(word);
        }
        return words.ToArray();
    }

    private static string Fnv8(string text)
    {
        ulong h = 14695981039346656037UL;
        foreach (byte c in System.Text.Encoding.UTF8.GetBytes(text))
            h = (h ^ c) * 1099511628211UL;
        return ((uint)h).ToString("x8");
    }
}
