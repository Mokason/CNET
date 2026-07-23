using System.Text.Json;

namespace CNET.Cce.Llm.Judgment;

/// <summary>Three-way verdict. Unsure is a first-class answer: primitive
/// common sense must know when it has none.</summary>
public enum Verdict { Good, Unsure, Bad }

/// <summary>One judged text with the evidence behind the verdict.</summary>
/// <param name="Value">The verdict.</param>
/// <param name="Score">Signed margin score — positive leans good.</param>
/// <param name="TopFeatures">The features that decided it, for receipts.</param>
public sealed record Judgment(Verdict Value, double Score,
                              IReadOnlyList<string> TopFeatures);

/// <summary>
/// The most primitive adaptive common sense: a good/bad/unsure filter over
/// text, learned ONLINE from evidence the stack already produces — tombstoned
/// memories and corrected answers are declared-bad; verified records,
/// usage-earning blobs, and confirmed answers are declared-good. Nobody
/// labels anything by hand; consequences are the labels.
/// </summary>
/// <remarks>
/// Constitutional position, non-negotiable: this is TASTE, never TRUTH.
/// The judge may veto teaching candidates and deprioritize; it may never
/// certify anything, never overrule a mechanical verifier, and never
/// overrule an explicit user imperative. Certification stays with records,
/// verifiers, and the evidence gates — opinion filters the queue, proof
/// guards the gate.
/// <para>
/// Mechanism: eight cheap text features, a margin perceptron updated per
/// labeled exemplar, durable weights + an append-only evidence JSONL (the
/// same receipts culture as everything else — and the future exemplar table
/// for a natively certified judge, JTC-style). Seed weights encode the
/// obvious priors (degenerate repetition is bad) so day one is sane;
/// adaptation refines from there.
/// </para>
/// </remarks>
public sealed class AdaptiveJudge
{
    private static readonly string[] FeatureNames =
    [
        "bias", "length", "distinct_ratio", "trigram_repetition",
        "symbol_ratio", "digit_ratio", "avg_word_len", "single_word",
    ];

    private readonly string _weightsPath;
    private readonly string _evidencePath;
    private double[] _weights;

    /// <summary>|score| below this is Unsure — the judge abstains.</summary>
    public double Margin { get; init; } = 0.5;

    public AdaptiveJudge(string stateDir)
    {
        Directory.CreateDirectory(stateDir);
        _weightsPath = Path.Combine(stateDir, "judgment.weights.json");
        _evidencePath = Path.Combine(stateDir, "judgment.evidence.jsonl");
        _weights = Load();
    }

    /// <summary>Judges a text. Never throws on any input.</summary>
    public Judgment Judge(string text)
    {
        double[] f = Featurize(text);
        double score = 0;
        for (int i = 0; i < f.Length; i++) score += _weights[i] * f[i];

        var contributions = FeatureNames
            .Select((name, i) => (name, value: _weights[i] * f[i]))
            .Where(x => Math.Abs(x.value) > 0.05)
            .OrderByDescending(x => Math.Abs(x.value))
            .Take(3)
            .Select(x => $"{x.name}{(x.value >= 0 ? "+" : "")}{x.value:F2}")
            .ToList();

        Verdict verdict = score > Margin ? Verdict.Good
                        : score < -Margin ? Verdict.Bad
                        : Verdict.Unsure;
        return new Judgment(verdict, score, contributions);
    }

    /// <summary>
    /// Learns from one consequence-labeled exemplar (perceptron with margin:
    /// update only when the current verdict is wrong or weak). The exemplar
    /// is journaled durably — the judge's provenance, and the exemplar table
    /// a future natively-certified judge would be taught from.
    /// </summary>
    public void Learn(string text, bool good, string source)
    {
        double[] f = Featurize(text);
        double score = 0;
        for (int i = 0; i < f.Length; i++) score += _weights[i] * f[i];

        double target = good ? 1.0 : -1.0;
        if (target * score < Margin)              // wrong or inside the margin
        {
            const double LearningRate = 0.15;
            for (int i = 0; i < f.Length; i++)
                _weights[i] += LearningRate * target * f[i];
            Save();
        }

        File.AppendAllText(_evidencePath, JsonSerializer.Serialize(new
        {
            ts = DateTime.UtcNow.ToString("o"),
            good,
            source,
            text = text.Length > 400 ? text[..400] : text,
        }) + "\n");
    }

    /// <summary>Labeled exemplars journaled so far (adaptation evidence).</summary>
    public int EvidenceCount =>
        File.Exists(_evidencePath) ? File.ReadLines(_evidencePath).Count() : 0;

    // ── features: cheap, transparent, language-light ──

    internal static double[] Featurize(string text)
    {
        text = text.Trim();
        if (text.Length == 0) return [1, -1, 0, 0, 0, 0, 0, 1];

        var words = new List<string>();
        var word = new System.Text.StringBuilder();
        int symbols = 0, digits = 0, letters = 0;
        foreach (char c in text)
        {
            if (char.IsAsciiLetter(c)) { word.Append(char.ToLowerInvariant(c)); letters++; }
            else
            {
                if (word.Length > 0) { words.Add(word.ToString()); word.Clear(); }
                if (char.IsAsciiDigit(c)) digits++;
                else if (!char.IsWhiteSpace(c)) symbols++;
            }
        }
        if (word.Length > 0) words.Add(word.ToString());

        double distinctRatio = words.Count > 0 ? (double)words.Distinct().Count() / words.Count : 0;

        // Degeneracy detector: the most-repeated word trigram. The live
        // repetition loop ('"flags": false' forever) lights this up.
        double trigramRepetition = 0;
        if (words.Count >= 6)
        {
            var trigrams = new Dictionary<string, int>();
            for (int i = 0; i + 2 < words.Count; i++)
            {
                string tg = $"{words[i]} {words[i + 1]} {words[i + 2]}";
                trigrams[tg] = trigrams.GetValueOrDefault(tg) + 1;
            }
            trigramRepetition = (double)trigrams.Values.Max() * 3 / words.Count;
        }

        double total = Math.Max(1, text.Length);
        return
        [
            1.0,                                                   // bias
            Math.Min(1.0, text.Length / 400.0) * 2 - 1,            // length, [-1,1]
            distinctRatio * 2 - 1,                                 // vocabulary variety
            Math.Min(1.0, trigramRepetition),                      // degeneracy [0,1]
            Math.Min(1.0, symbols / total * 4),                    // symbol soup
            Math.Min(1.0, digits / total * 4),
            words.Count > 0 ? Math.Min(1.0, words.Average(w => w.Length) / 12.0) : 0,
            words.Count <= 2 ? 1 : 0,                              // near-empty content
        ];
    }

    private static double[] SeedWeights() =>
        // Priors: substantial varied text leans good; degenerate repetition,
        // symbol soup, and near-empty content lean bad. Adaptation refines.
        [0.1, 0.3, 0.5, -1.6, -0.8, -0.2, 0.1, -0.6];

    private double[] Load()
    {
        if (!File.Exists(_weightsPath)) return SeedWeights();
        try
        {
            double[]? w = JsonSerializer.Deserialize<double[]>(File.ReadAllText(_weightsPath));
            return w is { Length: 8 } ? w : SeedWeights();
        }
        catch (JsonException) { return SeedWeights(); }
    }

    private void Save()
    {
        string tmp = _weightsPath + ".tmp";
        File.WriteAllText(tmp, JsonSerializer.Serialize(_weights));
        File.Move(tmp, _weightsPath, overwrite: true);
    }
}
