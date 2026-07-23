using System.Text;
using System.Text.RegularExpressions;

namespace CNET.Cce.Llm.Eval;

/// <summary>How a case's answer is scored.</summary>
public enum CheckKind
{
    /// <summary>The expected numeric token appears as a whole number.</summary>
    Numeric,
    /// <summary>The expected text appears (case-insensitive).</summary>
    Contains,
    /// <summary>Any substantive, non-degenerate answer passes (parity/harm check).</summary>
    NonDegenerate,
}

/// <summary>One evaluation question with a mechanical check.</summary>
/// <param name="Category">Groups the report (arithmetic, memory, general…).</param>
/// <param name="Question">Posed identically to both lanes.</param>
/// <param name="Check">How <paramref name="Expected"/> is matched.</param>
/// <param name="Expected">The known-good answer (ignored for NonDegenerate).</param>
public sealed record EvalCase(string Category, string Question, CheckKind Check, string Expected);

/// <summary>One case scored on both lanes.</summary>
public sealed record EvalResult(
    EvalCase Case, bool ModelOnlyPass, bool FullStackPass,
    string ModelAnswer, string FullAnswer);

/// <summary>Per-category and overall tallies.</summary>
public sealed record EvalReport(
    IReadOnlyList<EvalResult> Results,
    IReadOnlyDictionary<string, (int ModelOnly, int FullStack, int Total)> ByCategory,
    int ModelOnlyTotal, int FullStackTotal, int Total);

/// <summary>
/// The capstone measurement: does the full stack actually answer better than
/// the bare model? Each case is posed identically to two lanes — the raw model
/// alone, and the model behind the full memory/exact/tool/record stack — and
/// scored by a mechanical check. Categories where the stack has a structural
/// advantage (arithmetic → exact lane; memory facts → recall) should show a
/// wide delta; a neutral category (general knowledge) should show near-parity,
/// proving the stack helps where it can and does no harm where it cannot.
/// </summary>
/// <remarks>
/// Pure orchestration: the two lanes are injected as answer functions, so the
/// harness logic is testable without a model, and the same code drives the
/// live console run and the unit tests.
/// </remarks>
public sealed class EvalHarness
{
    private readonly Func<string, string> _modelOnly;
    private readonly Func<string, string> _fullStack;

    public EvalHarness(Func<string, string> modelOnly, Func<string, string> fullStack)
    {
        _modelOnly = modelOnly ?? throw new ArgumentNullException(nameof(modelOnly));
        _fullStack = fullStack ?? throw new ArgumentNullException(nameof(fullStack));
    }

    /// <summary>Runs every case on both lanes. <paramref name="onCase"/> reports progress.</summary>
    public EvalReport Run(IReadOnlyList<EvalCase> cases, Action<EvalResult>? onCase = null)
    {
        var results = new List<EvalResult>(cases.Count);
        foreach (EvalCase c in cases)
        {
            string mo = Safe(_modelOnly, c.Question);
            string fs = Safe(_fullStack, c.Question);
            var r = new EvalResult(c, Score(c, mo), Score(c, fs), mo, fs);
            results.Add(r);
            onCase?.Invoke(r);
        }

        var byCat = results
            .GroupBy(r => r.Case.Category)
            .ToDictionary(g => g.Key,
                g => (g.Count(r => r.ModelOnlyPass), g.Count(r => r.FullStackPass), g.Count()));

        return new EvalReport(results, byCat,
            results.Count(r => r.ModelOnlyPass),
            results.Count(r => r.FullStackPass),
            results.Count);
    }

    private static string Safe(Func<string, string> lane, string q)
    {
        try { return lane(q) ?? ""; }
        catch (Exception ex) { return "[error] " + ex.Message; }
    }

    /// <summary>Scores one answer against a case. Never throws.</summary>
    public static bool Score(EvalCase c, string answer)
    {
        if (string.IsNullOrWhiteSpace(answer)) return false;
        return c.Check switch
        {
            CheckKind.Numeric => ContainsWholeNumber(answer, c.Expected),
            CheckKind.Contains => answer.Contains(c.Expected, StringComparison.OrdinalIgnoreCase),
            CheckKind.NonDegenerate => IsSubstantive(answer),
            _ => false,
        };
    }

    /// <summary>The expected digits appear bounded by non-digits (so 4200 does
    /// not match inside 142008), commas/spaces in the answer ignored.</summary>
    private static bool ContainsWholeNumber(string answer, string expected)
    {
        string normAnswer = answer.Replace(",", "").Replace(" ", "");
        string normExpected = expected.Replace(",", "").Replace(" ", "");
        return Regex.IsMatch(normAnswer, $@"(?<!\d){Regex.Escape(normExpected)}(?!\d)");
    }

    /// <summary>Non-empty, of some length, and not a degenerate repetition loop.</summary>
    private static bool IsSubstantive(string answer)
    {
        answer = answer.Trim();
        if (answer.Length < 8) return false;
        var words = Regex.Matches(answer.ToLowerInvariant(), @"[a-z]+")
                         .Select(m => m.Value).ToList();
        if (words.Count < 3) return false;
        return words.Distinct().Count() >= words.Count * 0.4;   // not mostly repeated
    }

    /// <summary>Renders the report as a fixed-width table with the deltas.</summary>
    public static string Format(EvalReport report)
    {
        var sb = new StringBuilder();
        sb.AppendLine("┌─ evaluation: model-only vs full stack ─────────────────────────┐");
        sb.AppendLine($"  {"category",-16} {"model-only",12} {"full-stack",12} {"delta",8}");
        sb.AppendLine("  ────────────────────────────────────────────────────────────");
        foreach ((string cat, (int mo, int fs, int total)) in
                 report.ByCategory.OrderBy(kv => kv.Key))
        {
            int delta = fs - mo;
            sb.AppendLine($"  {cat,-16} {$"{mo}/{total}",12} {$"{fs}/{total}",12} " +
                          $"{(delta >= 0 ? "+" : "") + delta,8}");
        }
        sb.AppendLine("  ────────────────────────────────────────────────────────────");
        int totalDelta = report.FullStackTotal - report.ModelOnlyTotal;
        sb.AppendLine($"  {"TOTAL",-16} {$"{report.ModelOnlyTotal}/{report.Total}",12} " +
                      $"{$"{report.FullStackTotal}/{report.Total}",12} " +
                      $"{(totalDelta >= 0 ? "+" : "") + totalDelta,8}");
        sb.AppendLine("└────────────────────────────────────────────────────────────────┘");
        return sb.ToString();
    }
}
