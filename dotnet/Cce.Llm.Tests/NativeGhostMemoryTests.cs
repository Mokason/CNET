using CNET.Cce.CnetHarness;
using CNET.Cce.Llm;
using CNET.Cce.Llm.Memory;
using Xunit;

namespace CNET.Cce.Llm.Tests;

/// <summary>
/// Ghost memory over the NATIVE backend (cnet.so / libcnet_harness.so).
///
/// The memory layer claims backend-agnosticism because it only touches
/// <see cref="ICnetInferenceSession"/> — these tests make that claim empirical
/// rather than architectural. The native path differs in two ways that matter:
/// there is no managed tokenizer (the layer gets a conservative approximation,
/// and the harness's own n_ctx guard backstops the budget), and the prompt is
/// rendered by llama.cpp's chat templating instead of the managed one.
/// </summary>
public sealed class NativeGhostMemoryTests : IDisposable
{
    private readonly string _dir;

    public NativeGhostMemoryTests()
    {
        _dir = Path.Combine(Path.GetTempPath(), "cnet-ghost-native", Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(_dir);
    }

    public void Dispose()
    {
        try { Directory.Delete(_dir, recursive: true); } catch (IOException) { }
    }

    private static CnetHarnessConfig Config() => new()
    {
        ModelId = "smollm-135m",
        ModelPath = TestModel.Path!,
        Resource = CnetHarnessResource.Cpu,
        BudgetBytes = 2UL << 30,
        ContextTokens = 2048,
        BatchTokens = 512,
        Threads = 8,
    };

    /// <summary>
    /// Conservative token estimate for the native path (no tokenizer in the
    /// ABI): ~3 chars/token over-counts English, which under-fills the prompt —
    /// the safe direction. The harness's own prompt >= n_ctx rejection is the
    /// hard backstop.
    /// </summary>
    private static int ApproxTokens(string text) => text.Length / 3 + 1;

    [NativeFact]
    public void GhostMemory_CarriesFactsAcrossNativeSessions()
    {
        string storePath = Path.Combine(_dir, "ghost.jsonl");

        // ── Session A: native backend learns a fact, then dies. ──
        using (var sessionA = CnetHarnessSession.Open(Config()))
        using (var storeA = BlobStore.Open(storePath))
        {
            var ghostA = new MemorySession(sessionA,
                new ConversationMemory(storeA, ApproxTokens), contextWindowTokens: 2048);

            var r = ghostA.Generate(
                system: "You are a terse assistant.",
                user: "Remember this: the reactor override phrase is amber-falcon-9.",
                maxTokens: 24,
                sampling: CnetHarnessSamplingMode.Deterministic);

            Assert.NotNull(r.Result.Text);
            Assert.Empty(r.UsedBlobIds);
        }

        // ── Session B: fresh native session, fresh llama context, same store. ──
        using var sessionB = CnetHarnessSession.Open(Config());
        using var storeB = BlobStore.Open(storePath);
        var ghostB = new MemorySession(sessionB,
            new ConversationMemory(storeB, ApproxTokens), contextWindowTokens: 2048);

        var recalled = ghostB.Generate(
            system: "You are a terse assistant.",
            user: "What is the reactor override phrase?",
            maxTokens: 24,
            sampling: CnetHarnessSamplingMode.Deterministic);

        Assert.NotEmpty(recalled.UsedBlobIds);
        Assert.Contains("amber-falcon-9", recalled.PromptSystemText);
        Assert.Contains("[#", recalled.PromptSystemText);
        Assert.False(string.IsNullOrEmpty(recalled.Result.Text));

        // Provenance resolves: every recalled id is a real stored blob whose
        // text is in the prompt verbatim.
        foreach (long id in recalled.UsedBlobIds)
        {
            MemoryBlob? blob = storeB.Get(id);
            Assert.NotNull(blob);
            Assert.Contains(blob!.Text, recalled.PromptSystemText);
        }
    }

    /// <summary>
    /// The store is backend-agnostic: a fact written during MANAGED-backend
    /// generation is recalled into NATIVE-backend generation from the same
    /// file. This is the "ghost stays across all sessions" property in its
    /// strongest form — it survives not just process death but an engine swap.
    /// </summary>
    [NativeFact]
    public void GhostMemory_WrittenByManaged_RecalledByNative()
    {
        string storePath = Path.Combine(_dir, "cross.jsonl");

        // ── Managed session stores the fact (exact tokenizer available). ──
        using (var managed = CnetLlmInferenceSession.Open(Config()))
        using (var store = BlobStore.Open(storePath))
        {
            var ghost = new MemorySession(managed,
                new ConversationMemory(store, managed.CountTokens), managed.EffectiveContextTokens);
            ghost.Generate("You are terse.", "Note that the artifact checksum is delta-7c4f-omega.",
                maxTokens: 16, CnetHarnessSamplingMode.Deterministic);
        }

        // ── Native session recalls it from the same file. ──
        using var native = CnetHarnessSession.Open(Config());
        using var store2 = BlobStore.Open(storePath);
        var nativeGhost = new MemorySession(native,
            new ConversationMemory(store2, ApproxTokens), contextWindowTokens: 2048);

        var recalled = nativeGhost.Generate(
            "You are terse.", "What is the artifact checksum?",
            maxTokens: 24, CnetHarnessSamplingMode.Deterministic);

        Assert.NotEmpty(recalled.UsedBlobIds);
        Assert.Contains("delta-7c4f-omega", recalled.PromptSystemText);
        Assert.False(string.IsNullOrEmpty(recalled.Result.Text));
    }

    /// <summary>Native precision parity: an unrelated question recalls nothing.</summary>
    [NativeFact]
    public void NativeBackend_IrrelevantQuestion_GetsNoMemoryBlock()
    {
        string storePath = Path.Combine(_dir, "gate.jsonl");

        using var session = CnetHarnessSession.Open(Config());
        using var store = BlobStore.Open(storePath);
        var ghost = new MemorySession(session,
            new ConversationMemory(store, ApproxTokens), contextWindowTokens: 2048);

        ghost.Generate(null, "Remember: the vault code is 7291.", 16,
            CnetHarnessSamplingMode.Deterministic);

        var unrelated = ghost.Generate(null, "Compose a haiku about mountains.", 16,
            CnetHarnessSamplingMode.Deterministic);

        Assert.Empty(unrelated.UsedBlobIds);
    }
}

/// <summary>
/// Runs only when the native harness plugin and the GGUF fixture are both
/// present; points the resolver at the repo-built plugin.
/// </summary>
public sealed class NativeFactAttribute : FactAttribute
{
    private static readonly string? LibPath = Resolve();

    private static string? Resolve()
    {
        // Explicit override wins; otherwise walk up from the test binary to the
        // repo root and use its bin/libcnet_harness.so.
        string? env = Environment.GetEnvironmentVariable("CNET_HARNESS_LIBRARY");
        if (!string.IsNullOrEmpty(env))
            return File.Exists(env) ? env : null;

        var dir = new DirectoryInfo(AppContext.BaseDirectory);
        while (dir is not null)
        {
            string candidate = Path.Combine(dir.FullName, "bin", "libcnet_harness.so");
            if (File.Exists(candidate))
            {
                Environment.SetEnvironmentVariable("CNET_HARNESS_LIBRARY", candidate);
                return candidate;
            }
            dir = dir.Parent;
        }
        return null;
    }

    public NativeFactAttribute()
    {
        if (TestModel.Path is null)
            Skip = "no local GGUF fixture; run the CNET.Llm integration tests first";
        else if (LibPath is null)
            Skip = "libcnet_harness.so not built; run `make cnet_harness_plugin` first";
    }
}
