using CNET.Cce.CnetHarness;
using CNET.Cce.Llm;
using CNET.Cce.Llm.Memory;
using Xunit;

namespace CNET.Cce.Llm.Tests;

/// <summary>
/// End-to-end ghost-memory tests over a real model, and the window-enforcement
/// parity the memory layer depends on.
/// </summary>
public sealed class MemorySessionIntegrationTests : IDisposable
{
    private readonly string _dir;

    public MemorySessionIntegrationTests()
    {
        _dir = Path.Combine(Path.GetTempPath(), "cnet-ghost-e2e", Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(_dir);
    }

    public void Dispose()
    {
        try { Directory.Delete(_dir, recursive: true); } catch (IOException) { }
    }

    private static CnetHarnessConfig Config(uint contextTokens = 2048) => new()
    {
        ModelId = "smollm-135m",
        ModelPath = TestModel.Path!,
        Resource = CnetHarnessResource.Cpu,
        BudgetBytes = 2UL << 30,
        ContextTokens = contextTokens,
        BatchTokens = 512,
        Threads = 8,
    };

    /// <summary>
    /// The whole point of the feature: a fact stored in one session is recalled
    /// verbatim, with provenance, into a later session's prompt — across a full
    /// dispose/reopen of everything including the model.
    /// </summary>
    [ModelFact]
    public void GhostMemory_CarriesFactsAcrossSessions_Verbatim()
    {
        string storePath = Path.Combine(_dir, "ghost.jsonl");

        // ── Session A: learn a fact, then die. ──
        using (var sessionA = CnetLlmInferenceSession.Open(Config()))
        using (var storeA = BlobStore.Open(storePath))
        {
            var memoryA = new ConversationMemory(storeA, sessionA.CountTokens);
            var ghostA = new MemorySession(sessionA, memoryA, sessionA.EffectiveContextTokens);

            var r = ghostA.Generate(
                system: "You are a terse assistant.",
                user: "Remember this: the deployment password is quartz-owl-42.",
                maxTokens: 24,
                sampling: CnetHarnessSamplingMode.Deterministic);

            Assert.NotNull(r.Result.Text);
            Assert.Empty(r.UsedBlobIds);   // nothing to recall yet
        }

        // ── Session B: fresh everything, same store file. ──
        using var sessionB = CnetLlmInferenceSession.Open(Config());
        using var storeB = BlobStore.Open(storePath);
        var memoryB = new ConversationMemory(storeB, sessionB.CountTokens);
        var ghostB = new MemorySession(sessionB, memoryB, sessionB.EffectiveContextTokens);

        var recalled = ghostB.Generate(
            system: "You are a terse assistant.",
            user: "What is the deployment password?",
            maxTokens: 24,
            sampling: CnetHarnessSamplingMode.Deterministic);

        // The mechanical guarantees — the memory reached the prompt, verbatim,
        // with provenance. (Whether a 135M model then answers correctly is a
        // model-quality question, not a memory-layer one.)
        Assert.NotEmpty(recalled.UsedBlobIds);
        Assert.Contains("quartz-owl-42", recalled.PromptSystemText);
        Assert.Contains("[#", recalled.PromptSystemText);

        // Every recalled id resolves to a real stored blob whose text appears
        // verbatim in the prompt — no memory can be invented by the layer.
        foreach (long id in recalled.UsedBlobIds)
        {
            MemoryBlob? blob = storeB.Get(id);
            Assert.NotNull(blob);
            Assert.Contains(blob!.Text, recalled.PromptSystemText);
        }
    }

    [ModelFact]
    public void IrrelevantQuestion_GetsNoMemoryBlock()
    {
        string storePath = Path.Combine(_dir, "ghost2.jsonl");

        using var session = CnetLlmInferenceSession.Open(Config());
        using var store = BlobStore.Open(storePath);
        var memory = new ConversationMemory(store, session.CountTokens);
        var ghost = new MemorySession(session, memory, session.EffectiveContextTokens);

        ghost.Generate(null, "Remember: the vault code is 7291.", maxTokens: 16,
            sampling: CnetHarnessSamplingMode.Deterministic);

        var unrelated = ghost.Generate(null, "Write a haiku about mountains.", maxTokens: 16,
            sampling: CnetHarnessSamplingMode.Deterministic);

        Assert.Empty(unrelated.UsedBlobIds);
        Assert.DoesNotContain("7291", unrelated.PromptSystemText.Split("### Recent turns")[0]);
    }

    /// <summary>
    /// ContextTokens now behaves like the native n_ctx: an oversized prompt is
    /// InvalidArgument before decode, not an opaque BackendFailure after it.
    /// </summary>
    [ModelFact]
    public void OversizedPrompt_IsInvalidArgument_MatchingNativeSemantics()
    {
        using var session = CnetLlmInferenceSession.Open(Config(contextTokens: 512));

        string longPrompt = string.Join(' ', Enumerable.Repeat("token stuffing paragraph", 400));
        var ex = Assert.Throws<CnetHarnessException>(() => session.Generate(new CnetHarnessGenerateOptions
        {
            User = longPrompt,
            Role = "test",
            MaxTokens = 16,
        }));

        Assert.Equal(CnetHarnessStatus.InvalidArgument, ex.Status);
        Assert.Contains("window", ex.Message);
    }

    /// <summary>Answer budget is clamped to the room the window leaves, like native cap_max_tokens.</summary>
    [ModelFact]
    public void AnswerBudget_IsClampedToWindow()
    {
        using var session = CnetLlmInferenceSession.Open(Config(contextTokens: 512));

        var result = session.Generate(new CnetHarnessGenerateOptions
        {
            User = "The capital of France is",
            Role = "test",
            MaxTokens = 60000,   // absurd; must be clamped, not crash
            Sampling = CnetHarnessSamplingMode.Deterministic,
        });

        Assert.True(result.PromptTokens + result.GeneratedTokens <= 512,
            $"prompt {result.PromptTokens} + generated {result.GeneratedTokens} exceeded the 512-token window");
    }

    /// <summary>CountTokens agrees with the token counts generation reports.</summary>
    [ModelFact]
    public void CountTokens_MatchesGenerationAccounting()
    {
        using var session = CnetLlmInferenceSession.Open(Config());

        // The raw-concat path (no system) feeds the user text through directly,
        // so the reported prompt token count must equal CountTokens of it.
        string prompt = "The capital of France is";
        var result = session.Generate(new CnetHarnessGenerateOptions
        {
            User = prompt,
            Role = "test",
            MaxTokens = 4,
        });

        Assert.Equal(session.CountTokens(prompt), (int)result.PromptTokens);
    }
}
