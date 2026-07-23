namespace CNET.Cce.Llm.Eval;

/// <summary>One seeded memory fact and the question that should recall it.</summary>
public sealed record MemoryFact(string Fact, string Question, string Expected);

/// <summary>
/// The default evaluation suite. Two structural-advantage categories where
/// the stack should win decisively — arithmetic (the exact lane fires
/// automatically) and memory (recall fires automatically) — plus a neutral
/// general-knowledge category the bare model already handles, included so the
/// harness measures parity and harm honestly, not just a rigged win.
/// </summary>
public static class EvalSuite
{
    /// <summary>Facts to seed into the full-stack store before the run. The
    /// model-only lane never sees these — that is the point.</summary>
    public static readonly IReadOnlyList<MemoryFact> MemoryFacts =
    [
        new("The deployment password for the aurora cluster is quartz-owl-42.",
            "What is the deployment password for the aurora cluster?", "quartz-owl-42"),
        new("The night shift supervisor at the Braxton facility is Dana Whitlock.",
            "Who is the night shift supervisor at the Braxton facility?", "Dana Whitlock"),
        new("The backup generator at the marina runs on diesel batch number D-7719.",
            "Which diesel batch number does the marina backup generator use?", "D-7719"),
        new("Our internal codename for the migration project is Silverfin.",
            "What is the internal codename for the migration project?", "Silverfin"),
        new("The wifi password at the lakehouse is grendel-echo-9.",
            "What is the wifi password at the lakehouse?", "grendel-echo-9"),
    ];

    /// <summary>The arithmetic and general cases (memory cases are added by the
    /// console once the facts are seeded, so their questions match the store).</summary>
    public static IReadOnlyList<EvalCase> StaticCases =>
    [
        // Arithmetic — large enough that the bare model routinely errs, while
        // the exact lane is always right.
        new("arithmetic", "What is 4827 * 3913?", CheckKind.Numeric, "18888051"),
        new("arithmetic", "What is 98765 - 43210?", CheckKind.Numeric, "55555"),
        new("arithmetic", "Compute 7919 * 6841.", CheckKind.Numeric, "54173879"),
        new("arithmetic", "What is 738291 * 466517?", CheckKind.Numeric, "344425302447"),
        new("arithmetic", "What is 89234 * 77219?", CheckKind.Numeric, "6890560246"),
        new("arithmetic", "What is (12345 + 67890) * 37?", CheckKind.Numeric, "2968695"),

        // General knowledge — the bare model already knows these; the stack
        // should match, not break. A regression here means the stack HARMS.
        new("general", "What is the chemical symbol for gold?", CheckKind.Contains, "Au"),
        new("general", "What is the capital of Japan?", CheckKind.Contains, "Tokyo"),
        new("general", "How many continents are there on Earth?", CheckKind.Contains, "seven"),
        new("general", "What planet is known as the Red Planet?", CheckKind.Contains, "Mars"),
        new("general", "Write one sentence about why the sky appears blue.",
            CheckKind.NonDegenerate, ""),
        new("general", "Explain in one sentence what a prime number is.",
            CheckKind.NonDegenerate, ""),
    ];

    /// <summary>Builds the memory cases from the seeded facts.</summary>
    public static IEnumerable<EvalCase> MemoryCases() =>
        MemoryFacts.Select(f => new EvalCase("memory", f.Question, CheckKind.Contains, f.Expected));

    /// <summary>The full default suite.</summary>
    public static IReadOnlyList<EvalCase> All() =>
        [.. StaticCases, .. MemoryCases()];
}
