// ghost-eval — does the full stack answer better than the bare model?
//
// Poses each suite question to two lanes over the SAME model: the raw model
// alone, and the model behind the full memory/exact/tool/record stack. Scores
// both mechanically and prints the delta. The capstone measurement of the
// whole "certified knowledge beats the parrot" thesis — and, because it runs
// every organ together on one path, the integration test the pieces never had.
//
//   ghost-eval [--model minimax-m3:cloud] [--ollama-url http://localhost:11434]

using CNET.Cce.CnetHarness;
using CNET.Cce.Llm.Eval;
using CNET.Cce.Llm.Memory;
using CNET.Cce.Llm.Ollama;

string model = "minimax-m3:cloud";
string ollamaUrl = "http://localhost:11434";
uint maxTokens = 400;
for (int i = 0; i < args.Length; i++)
{
    string? Next() => i + 1 < args.Length ? args[++i] : null;
    switch (args[i])
    {
        case "--model": model = Next() ?? model; break;
        case "--ollama-url": ollamaUrl = Next() ?? ollamaUrl; break;
        case "--max-tokens": maxTokens = uint.Parse(Next() ?? "400"); break;
        default:
            Console.Error.WriteLine("usage: ghost-eval [--model M] [--ollama-url U] [--max-tokens N]");
            return 2;
    }
}

string dir = Path.Combine(Path.GetTempPath(), "ghost-eval", Guid.NewGuid().ToString("N"));
Directory.CreateDirectory(dir);
string storePath = Path.Combine(dir, "eval.jsonl");

Console.WriteLine($"ghost-eval | model={model}");
Console.WriteLine("seeding the full-stack store with facts the bare model cannot know...");

using var session = OllamaSession.Open(model, ollamaUrl);
session.ContextTokens = 16384;
using var store = BlobStore.Open(storePath);

// Seed the memory facts as prior-session user turns, then a summary session so
// they are clearly "past" material recalled by keyword.
int seed = 0;
foreach (MemoryFact f in EvalSuite.MemoryFacts)
    store.Append($"eval-seed-{seed++}", 0, "user", f.Fact, f.Fact.Length / 4 + 1);

var memory = new ConversationMemory(store, session.CountTokens);
var ghost = new MemorySession(session, memory, 16384, session.CountTokens)
{
    ExactLane = true,   // the structural arithmetic advantage
};

// The two lanes over the SAME model. Model-only: a bare generation, no memory,
// no exact lane, no stack. Full-stack: the whole MemorySession path.
string ModelOnly(string q) => session.Generate(new CnetHarnessGenerateOptions
{
    System = "You are a helpful, terse assistant.",
    User = q,
    Role = "eval",
    MaxTokens = maxTokens,
    Sampling = CnetHarnessSamplingMode.Focused,
    Think = false,
}).Text;

string FullStack(string q) => ghost.Generate(
    "You are a helpful, terse assistant with persistent memory.",
    q, maxTokens, CnetHarnessSamplingMode.Focused).Result.Text;

var harness = new EvalHarness(ModelOnly, FullStack);
var cases = EvalSuite.All();
Console.WriteLine($"running {cases.Count} cases × 2 lanes...\n");

int n = 0;
EvalReport report = harness.Run(cases, r =>
{
    n++;
    string mo = r.ModelOnlyPass ? "✓" : "✗";
    string fs = r.FullStackPass ? "✓" : "✗";
    string flag = r.FullStackPass && !r.ModelOnlyPass ? "  ← stack win"
                : r.ModelOnlyPass && !r.FullStackPass ? "  ← REGRESSION" : "";
    Console.WriteLine($"  [{n,2}/{cases.Count}] {r.Case.Category,-11} " +
        $"model {mo}  stack {fs}  {Trunc(r.Case.Question, 42)}{flag}");
});

Console.WriteLine();
Console.WriteLine(EvalHarness.Format(report));

// Honest callout of any regression — a case the bare model got right and the
// stack got wrong is the most important thing this harness can find.
var regressions = report.Results.Where(r => r.ModelOnlyPass && !r.FullStackPass).ToList();
if (regressions.Count > 0)
{
    Console.WriteLine($"⚠ {regressions.Count} REGRESSION(S) — the stack harmed these:");
    foreach (EvalResult r in regressions)
        Console.WriteLine($"    [{r.Case.Category}] {r.Case.Question}\n" +
                          $"      stack answered: {Trunc(r.FullAnswer.Replace("\n", " "), 80)}");
}

try { Directory.Delete(dir, recursive: true); } catch (IOException) { }

// Optional promote delta for CNET_PROMOTE_EVAL_DELTA (adapter/stack gates).
{
    string? deltaPath = Environment.GetEnvironmentVariable("CNET_PROMOTE_EVAL_DELTA");
    if (!string.IsNullOrEmpty(deltaPath) && report.Total > 0)
    {
        double mo = report.ModelOnlyTotal / (double)report.Total;
        double fs = report.FullStackTotal / (double)report.Total;
        string? parent = Path.GetDirectoryName(Path.GetFullPath(deltaPath));
        if (!string.IsNullOrEmpty(parent)) Directory.CreateDirectory(parent);
        File.WriteAllText(deltaPath,
            FormattableString.Invariant($"delta {fs - mo:F6}\nacc_off {mo:F6}\nacc_on {fs:F6}\n"));
        Console.WriteLine($"wrote promote delta → {deltaPath} (Δ={fs - mo:+0.###})");
    }
}
return 0;

static string Trunc(string s, int n) =>
    s.Length <= n ? s : s[..n] + "…";
