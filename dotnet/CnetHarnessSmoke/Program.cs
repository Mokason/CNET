using System.Diagnostics;
using System.Text.Json;
using System.Text.RegularExpressions;
using CNET.Cce.CnetHarness;
using CNET.CceHost;

const string BatchMode = "--batch-regression";
const string PrefixMode = "--prefix-reuse-regression";
const string SoakMode = "--memory-soak";
const string ContinuousMode = "--continuous-memory";
const string AsyncContextMode = "--async-context";
const string GpuBaselineMode = "--gpu-baseline";
const string GpuOffloadMode = "--gpu-offload";

if (args.Length < 1 || args.Length > 5 || !File.Exists(args[0]))
{
    Console.Error.WriteLine(
        $"usage: CnetHarnessSmoke /absolute/path/to/model.gguf "
        + $"[{BatchMode}|{PrefixMode}|{SoakMode}|{ContinuousMode}|{AsyncContextMode}|"
        + $"{GpuBaselineMode}|{GpuOffloadMode} LAYERS DEVICES [MAX_VRAM_MIB]]");
    return 2;
}

string? mode = args.Length >= 2 ? args[1] : null;
if (mode is not null and not BatchMode and not PrefixMode
    and not SoakMode and not ContinuousMode and not AsyncContextMode
    and not GpuBaselineMode and not GpuOffloadMode)
{
    Console.Error.WriteLine($"unknown mode: {mode}");
    return 2;
}

CnetHarnessGpuOffload? gpuOffload = null;
int[] gpuDevices = Array.Empty<int>();
if (mode == GpuOffloadMode)
{
    if (args.Length is < 4 or > 5
        || !int.TryParse(args[2], out int gpuLayers)
        || gpuLayers <= 0)
    {
        Console.Error.WriteLine(
            $"{GpuOffloadMode} requires positive LAYERS and comma-separated DEVICES");
        return 2;
    }
    try
    {
        gpuDevices = args[3].Split(',', StringSplitOptions.RemoveEmptyEntries)
            .Select(int.Parse).ToArray();
    }
    catch (Exception ex) when (ex is FormatException or OverflowException)
    {
        Console.Error.WriteLine("DEVICES must be comma-separated integer indices");
        return 2;
    }
    if (gpuDevices.Length is < 1 or > 4
        || gpuDevices.Any(d => d is < 0 or > 3)
        || gpuDevices.Distinct().Count() != gpuDevices.Length)
    {
        Console.Error.WriteLine("DEVICES must contain 1-4 distinct indices in [0,3]");
        return 2;
    }
    ulong maxVramMib = 2048;
    if (args.Length == 5
        && (!ulong.TryParse(args[4], out maxVramMib) || maxVramMib == 0))
    {
        Console.Error.WriteLine("MAX_VRAM_MIB must be positive");
        return 2;
    }
    gpuOffload = new CnetHarnessGpuOffload
    {
        LayerCount = gpuLayers,
        DeviceIndices = gpuDevices,
        TensorSplit = Enumerable.Repeat(1.0f, gpuDevices.Length).ToArray(),
        OffloadKqv = true,
        MaxVramBytesPerDevice = checked(maxVramMib * 1024UL * 1024UL),
    };
}
else if (args.Length > 2)
{
    Console.Error.WriteLine("extra arguments are only valid with --gpu-offload");
    return 2;
}

string modelPath = Path.GetFullPath(args[0]);
ulong modelBytes = checked((ulong)new FileInfo(modelPath).Length);
ulong budgetBytes = Math.Max(2UL * 1024UL * 1024UL * 1024UL,
                         checked(modelBytes + 512UL * 1024UL * 1024UL));
CnetHarnessResource resource = gpuOffload is null
    ? CnetHarnessResource.Cpu
    : gpuDevices[0] switch
    {
        0 => CnetHarnessResource.Gpu0,
        1 => CnetHarnessResource.Gpu1,
        2 => CnetHarnessResource.Gpu2,
        3 => CnetHarnessResource.Gpu3,
        _ => throw new UnreachableException(),
    };

var config = new CnetHarnessConfig
{
    ModelId = Path.GetFileNameWithoutExtension(modelPath),
    ModelPath = modelPath,
    Resource = resource,
    BudgetBytes = budgetBytes,
    MainGpu = gpuOffload is null ? -1 : gpuDevices[0],
    ContextTokens = 512,
    BatchTokens = mode is null ? 512u : 64u,
    Threads = 2,
    AicimoNumOps = 4,
    AicimoBaseDim = 32,
    GpuOffload = gpuOffload,
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

if (mode is GpuBaselineMode or GpuOffloadMode)
{
    CnetHarnessOffloadInfo? info = session.OffloadInfo;
    string prompt = string.Join(' ', Enumerable.Repeat("context", 120))
        + " Explain in one sentence why bounded memory matters. /no_think";
    CnetHarnessGenerationResult gpuResult = Generate(prompt, 64);
    bool isGpu = mode == GpuOffloadMode;
    bool partial = !isGpu || info is not null
        && info.RequestedGpuLayers == gpuOffload!.LayerCount
        && info.AppliedGpuLayers == gpuOffload.LayerCount
        && info.AppliedGpuLayers < info.ModelLayerCount;
    bool deviceMatch = !isGpu || info is not null
        && info.DeviceIndices.SequenceEqual(gpuDevices);
    bool residencyBounded = !isGpu || info is not null
        && info.VramBytes.Count == gpuDevices.Length
        && info.VramBytes.All(bytes => bytes > 0
            && bytes <= gpuOffload!.MaxVramBytesPerDevice);
    bool generated = gpuResult.GeneratedTokens > 0 && gpuResult.Text.Length > 0;
    bool pass = partial && deviceMatch && residencyBounded && generated;
    var evidence = new
    {
        status = pass
            ? isGpu ? "CNET_HARNESS_GPU_OFFLOAD_PASS"
                    : "CNET_HARNESS_GPU_BASELINE_PASS"
            : isGpu ? "CNET_HARNESS_GPU_OFFLOAD_FAIL"
                    : "CNET_HARNESS_GPU_BASELINE_FAIL",
        requestedGpuLayers = gpuOffload?.LayerCount ?? 0,
        appliedGpuLayers = info?.AppliedGpuLayers,
        modelLayerCount = info?.ModelLayerCount,
        deviceIndices = info?.DeviceIndices,
        splitMode = info?.SplitMode.ToString(),
        offloadKqv = info?.OffloadKqv,
        vramBytes = info?.VramBytes,
        maxVramBytesPerDevice = gpuOffload?.MaxVramBytesPerDevice ?? 0,
        gpuResult.PromptTokens,
        gpuResult.GeneratedTokens,
        gpuResult.PromptMs,
        gpuResult.GenerationMs,
        gpuResult.Text,
        partial,
        deviceMatch,
        residencyBounded,
    };
    if (!pass)
    {
        Console.Error.WriteLine(JsonSerializer.Serialize(evidence));
        return 1;
    }
    Console.WriteLine(JsonSerializer.Serialize(evidence));
    return 0;
}

if (mode == AsyncContextMode)
{
    string prompt = string.Join(' ', Enumerable.Repeat("memory", 120))
        + " Explain why bounded context improves coherent offline inference. /no_think";
    const uint acceptanceTokens = 64;
    using var chat = new CnetHarnessChatClient(
        session, role: "analytical", maxTokens: acceptanceTokens, ownsSession: false,
        sampling: CnetHarnessSamplingMode.Deterministic, seed: 424242);
    using var conversation = new CnetHarnessConversation(
        chat, "Answer concisely.", maxRetainedTurns: 3,
        maxRetainedCharacters: 4096, ownsClient: false);
    await using var pipeline = new AsyncContextPipeline(
        conversation,
        new FinalDraftContextSource(
            prompt,
            "Earlier discussion: bounded memory preserves relevant complete turns."),
        new FinalDraftContextSource(
            prompt,
            "connections: bounded -> memory, context -> coherence"),
        new AsyncContextPipelineOptions
        {
            TypingDebounce = TimeSpan.Zero,
            MaxHistoryCharacters = 256,
            MaxConnectionCharacters = 128,
            MaxCombinedCharacters = 512,
        });

    var superseded = new List<Task<AsyncContextSnapshot>>();
    int step = prompt.Length / 4;
    for (int length = step; length < prompt.Length; length += step)
        superseded.Add(pipeline.UpdateDraftAsync(prompt[..length]));
    Task<AsyncContextSnapshot> preparedTask = pipeline.UpdateDraftAsync(prompt);
    int cancelledDrafts = 0;
    foreach (Task<AsyncContextSnapshot> stale in superseded)
    {
        try { _ = await stale; }
        catch (OperationCanceledException) { cancelledDrafts++; }
    }
    AsyncContextSnapshot prepared = await preparedTask;

    var warmedTimer = Stopwatch.StartNew();
    string warmed = await pipeline.SendAsync(prompt);
    warmedTimer.Stop();
    int productionPrefillCount = chat.PrefillCount;

    // Keep acceptance resource-honest: release the production context before
    // opening the independent full-prefill baseline context.
    await pipeline.DisposeAsync();
    conversation.Dispose();
    chat.Dispose();
    session.Dispose();

    using var freshSession = CnetHarnessSession.Open(config);
    using var freshChat = new CnetHarnessChatClient(
        freshSession, role: "analytical", maxTokens: acceptanceTokens, ownsSession: false,
        sampling: CnetHarnessSamplingMode.Deterministic, seed: 424242);
    using var freshConversation = new CnetHarnessConversation(
        freshChat, "Answer concisely.", maxRetainedTurns: 3,
        maxRetainedCharacters: 4096, ownsClient: false);
    await ((IPrefillChatClient)freshChat).PrefillAsync(
        new[]
        {
            ("system", "Unrelated graph warmup."),
            ("user", "Return one token. /no_think"),
        },
        CancellationToken.None);
    var freshTimer = Stopwatch.StartNew();
    string fresh = await freshConversation.SendAsync(
        prompt, prepared.CombinedContext, CancellationToken.None);
    freshTimer.Stop();

    double latencyRatio = warmedTimer.Elapsed.TotalMilliseconds
        / freshTimer.Elapsed.TotalMilliseconds;
    bool outputEquivalent = warmed == fresh;
    bool pass = prepared.PrefillCompleted
        && productionPrefillCount == 1
        && cancelledDrafts == superseded.Count
        && outputEquivalent
        && latencyRatio < 0.85;
    var evidence = new
    {
        status = pass
            ? "CNET_HARNESS_ASYNC_CONTEXT_PASS"
            : "CNET_HARNESS_ASYNC_CONTEXT_FAIL",
        cancelledDrafts,
        submittedDrafts = superseded.Count + 1,
        productionPrefillCount,
        maxConcurrentModelSessions = 1,
        contextTokens = config.ContextTokens,
        generatedTokenLimit = acceptanceTokens,
        measurement = "generate-only warm-prefix versus full-prefill; shared hot OS page cache",
        prepared.PrefillCompleted,
        combinedContextCharacters = prepared.CombinedContext.Length,
        warmedMs = warmedTimer.Elapsed.TotalMilliseconds,
        freshMs = freshTimer.Elapsed.TotalMilliseconds,
        latencyRatio,
        outputEquivalent,
        warmed,
        fresh,
    };
    if (!pass)
    {
        Console.Error.WriteLine(JsonSerializer.Serialize(evidence));
        return 1;
    }
    Console.WriteLine(JsonSerializer.Serialize(evidence));
    return 0;
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

file sealed class FinalDraftContextSource(string finalDraft, string value)
    : IAsyncContextSource
{
    public async ValueTask<string> CollectAsync(
        string draft,
        int maxCharacters,
        CancellationToken cancellationToken)
    {
        if (!string.Equals(draft, finalDraft, StringComparison.Ordinal))
            await Task.Delay(Timeout.InfiniteTimeSpan, cancellationToken);
        cancellationToken.ThrowIfCancellationRequested();
        return value.Length <= maxCharacters
            ? value
            : value[..maxCharacters];
    }
}
