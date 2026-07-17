using System.Diagnostics;
using System.Text.Json;
using System.Text.RegularExpressions;
using CNET.Cce.CnetHarness;
using CNET.CceHost;

const string BatchMode = "--batch-regression";
const string PrefixMode = "--prefix-reuse-regression";
const string SoakMode = "--memory-soak";
const string ContinuousMode = "--continuous-memory";

if (args.Length is < 1 or > 2 || !File.Exists(args[0]))
{
    Console.Error.WriteLine(
        $"usage: CnetHarnessSmoke /absolute/path/to/model.gguf [{BatchMode}|{PrefixMode}|{SoakMode}|{ContinuousMode}]");
    return 2;
}

string? mode = args.Length == 2 ? args[1] : null;
if (mode is not null and not BatchMode and not PrefixMode
    and not SoakMode and not ContinuousMode)
{
    Console.Error.WriteLine($"unknown mode: {mode}");
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
    BatchTokens = mode is null ? 512u : 64u,
    Threads = 2,
    AicimoNumOps = 4,
    AicimoBaseDim = 32,
};

using var session = CnetHarnessSession.Open(config);
CnetHarnessRouteInfo route = session.ProbeRoute("analytical");

CnetHarnessGenerationResult Generate(string user, uint maxTokens = 8) =>
    session.Generate(new CnetHarnessGenerateOptions
    {
        System = "Answer concisely.",
        User = user,
        Role = "analytical",
        MaxTokens = maxTokens,
        Seed = 424242,
        Sampling = CnetHarnessSamplingMode.Deterministic,
    });

static string Visible(string text)
{
    string withoutThinking = Regex.Replace(text,
        @"<think>.*?</think>", string.Empty,
        RegexOptions.Singleline | RegexOptions.IgnoreCase);
    return withoutThinking
        .Replace("<think>", string.Empty, StringComparison.OrdinalIgnoreCase)
        .Replace("</think>", string.Empty, StringComparison.OrdinalIgnoreCase)
        .Trim();
}

if (mode == ContinuousMode)
{
    string nonce = "ORBIT-" + Guid.NewGuid().ToString("N")[..12].ToUpperInvariant();
    using var chat = new CnetHarnessChatClient(
        session, role: "memory", maxTokens: 64, ownsSession: false,
        sampling: CnetHarnessSamplingMode.Deterministic, seed: 424242);
    using var conversation = new CnetHarnessConversation(
        chat,
        "This is an exact memory test. Preserve explicitly supplied codes. "
        + "If a requested code is absent from this conversation, answer UNKNOWN.",
        maxRetainedTurns: 3, maxRetainedCharacters: 3500,
        ownsClient: false);

    _ = await conversation.SendAsync(
        $"Remember the exact code {nonce}. Reply ACK only. /no_think");
    _ = await conversation.SendAsync(
        "Unrelated check: reply BLUE only. /no_think");
    string recalled = Visible(await conversation.SendAsync(
        "Return the exact code I asked you to remember, and nothing else. /no_think"));

    using var isolated = new CnetHarnessConversation(
        chat,
        "This is an exact memory test. If a requested code is absent from "
        + "this conversation, answer UNKNOWN.",
        maxRetainedTurns: 1, maxRetainedCharacters: 1024,
        ownsClient: false);
    string control = Visible(await isolated.SendAsync(
        "Return the exact code from the other conversation. /no_think"));

    bool recallPass = recalled.Contains(nonce, StringComparison.Ordinal);
    bool isolationPass = !control.Contains(nonce, StringComparison.Ordinal);
    bool boundsPass = conversation.RetainedTurnCount <= 3
        && conversation.RetainedCharacterCount <= 3500;
    if (!recallPass || !isolationPass || !boundsPass)
    {
        Console.Error.WriteLine(JsonSerializer.Serialize(new
        {
            status = "CNET_HARNESS_CONTINUOUS_MEMORY_FAIL",
            nonce, recalled, control, recallPass, isolationPass, boundsPass,
            conversation.RetainedTurnCount,
            conversation.RetainedCharacterCount,
        }));
        return 1;
    }
    Console.WriteLine(JsonSerializer.Serialize(new
    {
        status = "CNET_HARNESS_CONTINUOUS_MEMORY_PASS",
        nonce, recalled, control, recallPass, isolationPass, boundsPass,
        conversation.RetainedTurnCount,
        conversation.RetainedCharacterCount,
    }));
    return 0;
}

if (mode == PrefixMode)
{
    string prefix = string.Join(' ', Enumerable.Repeat("memory", 120));
    CnetHarnessGenerationResult first = Generate(
        prefix + " alpha. Reply OK. /no_think", 1);
    string secondPrompt = prefix + " beta. Reply OK. /no_think";
    CnetHarnessGenerationResult second = Generate(secondPrompt, 1);
    using var freshSession = CnetHarnessSession.Open(config);
    CnetHarnessGenerationResult fresh = freshSession.Generate(
        new CnetHarnessGenerateOptions
        {
            System = "Answer concisely.",
            User = secondPrompt,
            Role = "analytical",
            MaxTokens = 1,
            Seed = 424242,
            Sampling = CnetHarnessSamplingMode.Deterministic,
        });
    double ratio = second.PromptMs / first.PromptMs;
    bool outputEquivalent = second.Text == fresh.Text
        && second.GeneratedTokens == fresh.GeneratedTokens;
    if (ratio >= 0.85 || !outputEquivalent)
    {
        Console.Error.WriteLine(JsonSerializer.Serialize(new
        {
            status = "CNET_HARNESS_PREFIX_REUSE_FAIL",
            first.PromptTokens,
            firstPromptMs = first.PromptMs,
            secondPromptMs = second.PromptMs,
            ratio,
            outputEquivalent,
            cachedText = second.Text,
            freshText = fresh.Text,
        }));
        return 1;
    }
    Console.WriteLine(JsonSerializer.Serialize(new
    {
        status = "CNET_HARNESS_PREFIX_REUSE_PASS",
        firstPromptTokens = first.PromptTokens,
        secondPromptTokens = second.PromptTokens,
        firstPromptMs = first.PromptMs,
        secondPromptMs = second.PromptMs,
        ratio,
        outputEquivalent,
    }));
    return 0;
}

if (mode == SoakMode)
{
    var process = Process.GetCurrentProcess();
    var samples = new List<object>();
    long warmPrivate = 0;
    long warmWorking = 0;
    long peakPrivate = 0;
    long peakWorking = 0;
    for (int i = 1; i <= 50; ++i)
    {
        _ = Generate("Reply with OK. /no_think", 4);
        if (i == 5)
        {
            GC.Collect();
            GC.WaitForPendingFinalizers();
            GC.Collect();
        }
        process.Refresh();
        long privateBytes = process.PrivateMemorySize64;
        long workingBytes = process.WorkingSet64;
        long managedBytes = GC.GetTotalMemory(forceFullCollection: false);
        if (i == 5)
        {
            warmPrivate = privateBytes;
            warmWorking = workingBytes;
        }
        if (i >= 5)
        {
            peakPrivate = Math.Max(peakPrivate, privateBytes);
            peakWorking = Math.Max(peakWorking, workingBytes);
        }
        samples.Add(new { iteration = i, privateBytes, workingBytes, managedBytes });
    }
    GC.Collect();
    GC.WaitForPendingFinalizers();
    GC.Collect();
    process.Refresh();
    long finalPrivate = process.PrivateMemorySize64;
    long finalWorking = process.WorkingSet64;
    long privateGrowth = peakPrivate - warmPrivate;
    long workingGrowth = peakWorking - warmWorking;
    long finalPrivateGrowth = finalPrivate - warmPrivate;
    long finalWorkingGrowth = finalWorking - warmWorking;
    const long PrivateLimit = 16L * 1024L * 1024L;
    const long WorkingLimit = 32L * 1024L * 1024L;
    if (privateGrowth > PrivateLimit || workingGrowth > WorkingLimit)
    {
        Console.Error.WriteLine(JsonSerializer.Serialize(new
        {
            status = "CNET_HARNESS_MEMORY_SOAK_FAIL",
            privateGrowth,
            workingGrowth,
            finalPrivateGrowth,
            finalWorkingGrowth,
            samples,
        }));
        return 1;
    }
    Console.WriteLine(JsonSerializer.Serialize(new
    {
        status = "CNET_HARNESS_MEMORY_SOAK_PASS",
        privateGrowth,
        workingGrowth,
        finalPrivateGrowth,
        finalWorkingGrowth,
        samples,
    }));
    return 0;
}

string user = mode == BatchMode
    ? string.Join(' ', Enumerable.Repeat("memory", 120))
      + "\nReply with exactly OK. /no_think"
    : "Reply with exactly the word OK.";
CnetHarnessGenerationResult result = Generate(user);
if (result.GeneratedTokens == 0)
{
    Console.Error.WriteLine("generation returned zero tokens");
    return 1;
}

string status = mode == BatchMode
    ? "CNET_HARNESS_BATCH_REGRESSION_PASS"
    : "CNET_HARNESS_REAL_SMOKE_PASS";
Console.WriteLine(JsonSerializer.Serialize(new
{
    status,
    model = modelPath,
    resource = "cpu",
    contextTokens = config.ContextTokens,
    batchTokens = config.BatchTokens,
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
