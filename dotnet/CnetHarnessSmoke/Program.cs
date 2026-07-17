using System.Text.Json;
using CNET.Cce.CnetHarness;

if (args.Length != 1 || !File.Exists(args[0]))
{
    Console.Error.WriteLine("usage: CnetHarnessSmoke /absolute/path/to/model.gguf");
    return 2;
}

string modelPath = Path.GetFullPath(args[0]);
ulong modelBytes = checked((ulong)new FileInfo(modelPath).Length);
ulong budgetBytes = Math.Max(2UL * 1024UL * 1024UL * 1024UL,
                         checked(modelBytes + 512UL * 1024UL * 1024UL));

var config = new CnetHarnessConfig
{
    ModelId = Path.GetFileNameWithoutExtension(modelPath),
    ModelPath = modelPath,
    Resource = CnetHarnessResource.Cpu,
    BudgetBytes = budgetBytes,
    MainGpu = -1,
    ContextTokens = 512,
    BatchTokens = 512,
    Threads = (uint)Math.Clamp(Environment.ProcessorCount, 1, 8),
    AicimoNumOps = 4,
    AicimoBaseDim = 32,
};

using var session = CnetHarnessSession.Open(config);
CnetHarnessRouteInfo route = session.ProbeRoute("analytical");
CnetHarnessGenerationResult result = session.Generate(new CnetHarnessGenerateOptions
{
    System = "Answer concisely.",
    User = "Reply with exactly the word OK.",
    Role = "analytical",
    MaxTokens = 8,
    Seed = 424242,
    Sampling = CnetHarnessSamplingMode.Auto,
});

if (result.GeneratedTokens == 0)
{
    Console.Error.WriteLine("generation returned zero tokens");
    return 1;
}

Console.WriteLine(JsonSerializer.Serialize(new
{
    status = "CNET_HARNESS_REAL_SMOKE_PASS",
    model = modelPath,
    resource = "cpu",
    route.SelectedAdapter,
    route.RouteUncertainty,
    route.EffectiveSampling,
    route.EffectiveTemperature,
    route.EffectiveTopP,
    route.EffectiveTopK,
    route.EffectiveMinP,
    result.PromptTokens,
    result.GeneratedTokens,
    result.Text,
    result.PromptMs,
    result.GenerationMs,
}));
return 0;
