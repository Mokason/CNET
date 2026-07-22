// A/B the two ICnetInferenceSession backends on identical work.
//
// Both are opened from the same CnetHarnessConfig and driven through the same
// interface, so this measures the backends and not two different call paths.
// Model load is timed separately from generation; generation numbers are
// steady-state over N reps with the backends alternating to blunt drift.
//
// The native backend needs libcnet_harness.so (make cnet_harness_plugin) —
// point CNET_HARNESS_LIBRARY at it. Without it, the native half is reported as
// unavailable rather than silently skipped.
//
//   dotnet run -c Release --project dotnet/Cce.Llm.Benchmarks -- \
//       <model.gguf> [reps] [maxTokens] [threads]

using System.Diagnostics;
using CNET.Cce.CnetHarness;
using CNET.Cce.Llm;

string modelPath = args.Length > 0
    ? args[0]
    : Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile),
        ".cnet-llm", "test-cache", "QuantFactory", "SmolLM-135M-GGUF", "SmolLM-135M.Q8_0.gguf");
int reps = args.Length > 1 ? int.Parse(args[1]) : 5;
uint maxTokens = args.Length > 2 ? uint.Parse(args[2]) : 64;
uint threads = args.Length > 3 ? uint.Parse(args[3]) : 8;
// Prompt repeat count. A 5-token prompt makes prefill unmeasurable — the
// timing is dominated by fixed setup. Raise this to characterize prefill at a
// realistic prompt length.
int promptRepeat = args.Length > 4 ? int.Parse(args[4]) : 1;
// Make every rep's prompt unique. Repeating one prompt lets a backend serve
// prefill from a cached KV prefix, which measures its cache rather than its
// prefill throughput. Pass 1 here for a true cold-prefill comparison.
bool uniquePrompts = args.Length > 5 && args[5] == "1";

string Prompt = promptRepeat <= 1
    ? "The capital of France is"
    : string.Join(" ", Enumerable.Repeat(
        "Paris is the capital and most populous city of France, situated on the river Seine.",
        promptRepeat)) + " The capital of France is";

if (!File.Exists(modelPath))
{
    Console.Error.WriteLine($"model not found: {modelPath}");
    return 1;
}

var config = new CnetHarnessConfig
{
    ModelId = "bench",
    ModelPath = modelPath,
    Resource = CnetHarnessResource.Cpu,
    BudgetBytes = 4UL << 30,
    ContextTokens = 1024,
    BatchTokens = 512,
    Threads = threads,
};

Console.WriteLine($"model      : {Path.GetFileName(modelPath)}");
Console.WriteLine($"reps       : {reps}   maxTokens: {maxTokens}   threads: {threads}");
Console.WriteLine($"prompt     : \"{Prompt}\"");
Console.WriteLine();

foreach (CnetHarnessSamplingMode mode in
         new[] { CnetHarnessSamplingMode.Deterministic, CnetHarnessSamplingMode.Balanced })
{
    Console.WriteLine($"=== sampling: {mode} ===");
    var managed = Measure("managed (CNET.Llm)", () => CnetLlmInferenceSession.Open(config), mode);
    var native = Measure("native  (cnet.so) ", () => CnetHarnessSession.Open(config), mode);
    Report(managed, native);
    Console.WriteLine();
}

return 0;

Result Measure(string label, Func<ICnetInferenceSession> open, CnetHarnessSamplingMode mode)
{
    var result = new Result(label);
    ICnetInferenceSession session;
    var loadWatch = Stopwatch.StartNew();
    try
    {
        session = open();
    }
    catch (Exception ex)
    {
        result.Unavailable = ex.Message;
        return result;
    }
    loadWatch.Stop();
    result.LoadMs = loadWatch.Elapsed.TotalMilliseconds;

    using (session)
    {
        var options = new CnetHarnessGenerateOptions
        {
            User = Prompt,
            Role = "bench",
            MaxTokens = maxTokens,
            Seed = 424242,
            Sampling = mode,
        };

        // One untimed warm-up: first call pays JIT and first-touch page costs
        // that would otherwise be charged to rep 1.
        try { session.Generate(options); }
        catch (Exception ex) { result.Unavailable = ex.Message; return result; }

        for (int i = 0; i < reps; i++)
        {
            CnetHarnessGenerateOptions repOptions = uniquePrompts
                ? new CnetHarnessGenerateOptions
                {
                    // Variation goes at the FRONT. A suffix-only change still
                    // shares a long common prefix, which prefix caching serves
                    // happily — that would measure the cache, not prefill.
                    User = $"Note {i} of {reps}, concerning {i * 7919} matters. {Prompt}",
                    Role = options.Role,
                    MaxTokens = options.MaxTokens,
                    Seed = options.Seed,
                    Sampling = options.Sampling,
                }
                : options;

            var wall = Stopwatch.StartNew();
            CnetHarnessGenerationResult r = session.Generate(repOptions);
            wall.Stop();

            result.WallMs.Add(wall.Elapsed.TotalMilliseconds);
            result.PrefillMs.Add(r.PromptMs);
            result.DecodeMs.Add(r.GenerationMs);
            result.PromptTokens = r.PromptTokens;
            result.GeneratedTokens = r.GeneratedTokens;
            result.Text ??= r.Text;
            result.EffectiveSampling = r.EffectiveSampling;
            result.EffectiveTemperature = r.EffectiveTemperature;
        }
    }
    return result;
}

static void Report(Result a, Result b)
{
    foreach (Result r in new[] { a, b })
    {
        if (r.Unavailable is not null)
        {
            Console.WriteLine($"{r.Label} : UNAVAILABLE — {r.Unavailable}");
            continue;
        }
        double decodeToks = r.GeneratedTokens / (Median(r.DecodeMs) / 1000.0);
        double prefillToks = Median(r.PrefillMs) > 0
            ? r.PromptTokens / (Median(r.PrefillMs) / 1000.0)
            : double.NaN;
        Console.WriteLine(
            $"{r.Label} : load {r.LoadMs,8:F1} ms | prefill {prefillToks,8:F1} tok/s | " +
            $"decode {decodeToks,7:F2} tok/s | wall {Median(r.WallMs),8:F1} ms " +
            $"(min {Min(r.WallMs):F1}) | {r.PromptTokens}p/{r.GeneratedTokens}g | " +
            $"applied {r.EffectiveSampling} T={r.EffectiveTemperature:F2}");
    }

    if (a.Unavailable is null && b.Unavailable is null)
    {
        double da = a.GeneratedTokens / (Median(a.DecodeMs) / 1000.0);
        double db = b.GeneratedTokens / (Median(b.DecodeMs) / 1000.0);
        Console.WriteLine($"  decode ratio (first/second): {da / db:F2}x");
        Console.WriteLine($"  text identical: {(a.Text == b.Text ? "yes" : "no")}");
        if (a.Text != b.Text)
        {
            Console.WriteLine($"    {a.Label.Trim()}: {Trim(a.Text)}");
            Console.WriteLine($"    {b.Label.Trim()}: {Trim(b.Text)}");
        }
    }
}

static string Trim(string? s)
{
    if (s is null) return "(null)";
    s = s.Replace('\n', ' ').Replace('\r', ' ').Trim();
    return s.Length <= 90 ? s : s[..90] + "…";
}

static double Median(List<double> xs)
{
    if (xs.Count == 0) return 0;
    var s = xs.ToArray();
    Array.Sort(s);
    return s.Length % 2 == 1 ? s[s.Length / 2] : (s[s.Length / 2 - 1] + s[s.Length / 2]) / 2.0;
}

static double Min(List<double> xs) => xs.Count == 0 ? 0 : xs.Min();

sealed class Result(string label)
{
    public string Label { get; } = label;
    public string? Unavailable { get; set; }
    public double LoadMs { get; set; }
    public List<double> WallMs { get; } = [];
    public List<double> PrefillMs { get; } = [];
    public List<double> DecodeMs { get; } = [];
    public uint PromptTokens { get; set; }
    public uint GeneratedTokens { get; set; }
    public string? Text { get; set; }
    public CnetHarnessSamplingMode EffectiveSampling { get; set; }
    public float EffectiveTemperature { get; set; }
}
