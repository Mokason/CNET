using CNET.Llm.Core.Attention;
using CNET.Llm.Core.Configuration;
using CNET.Llm.Core.Models;
using CNET.Llm.Engine;
using CNET.Llm.Engine.KvCache;
using CNET.Llm.Engine.PromptCache;
using CNET.Llm.Models.Gguf;
using CNET.Llm.Tokenizers;
using CNET.Llm.Tokenizers.ChatTemplates;

namespace CNET.Llm.Server;

/// <summary>
/// Shared server state: loaded model, concurrency control, mutable configuration.
/// Endpoints access live model/generator instances through this object.
/// May start "bare" (no model loaded) when <c>cnet-llm serve</c> is run without a model argument.
/// </summary>
public sealed class ServerState : IDisposable
{
    private readonly SemaphoreSlim _requestGate = new(1, 1);
    private int _disposed;

    /// <summary>Activation succeeded but old-resource cleanup failed; further swaps require restart.</summary>
    public bool RetirementCleanupFailed { get; private set; }

    /// <summary>Server startup options (updated on model swap).</summary>
    public required ServerOptions Options { get; set; }

    /// <summary>Model configuration (null when no model loaded).</summary>
    public ModelConfig? Config { get; set; }

    /// <summary>Tool call parser for the loaded model.</summary>
    public IToolCallParser? ToolCallParser { get; set; }

    /// <summary>KV-cache configuration.</summary>
    public KvCacheConfig KvCacheConfig { get; set; }

    /// <summary>KV-cache factory for the loaded model/device.</summary>
    public Func<ModelConfig, int, IKvCache>? KvCacheFactory { get; set; }

    /// <summary>Paged KV-cache factory (non-null when paged mode is active). Owns the shared block pool.</summary>
    public PagedKvCacheFactory? PagedFactory { get; set; }

    /// <summary>Prefix cache for prompt caching (null when disabled).</summary>
    public PrefixCache? PrefixCache { get; set; }

    /// <summary>Whether a model is loaded and ready to accept requests.</summary>
    public bool IsReady { get; set; }

    // ── Live instances (nullable — null when no model loaded) ──

    /// <summary>Currently loaded model.</summary>
    public IModel? Model { get; set; }

    /// <summary>Tokenizer for the loaded model.</summary>
    public ITokenizer? Tokenizer { get; set; }

    /// <summary>Chat template for the loaded model.</summary>
    public IChatTemplate? ChatTemplate { get; set; }

    /// <summary>Text generator wired to the current model.</summary>
    public TextGenerator? Generator { get; set; }

    /// <summary>Mutable sampling parameter defaults (changeable from the UI).</summary>
    public SamplingDefaults SamplingDefaults { get; set; } = new();

    /// <summary>Path of the currently loaded GGUF file.</summary>
    public string LoadedModelPath { get; set; } = "";

    /// <summary>Open GGUF file handle (disposed on model swap).</summary>
    public GgufFile? CurrentGguf { get; set; }

    /// <summary>Draft model for speculative decoding (null when disabled).</summary>
    public IModel? DraftModel { get; set; }

    /// <summary>Path of the loaded draft model GGUF file.</summary>
    public string DraftModelPath { get; set; } = "";

    /// <summary>Open draft GGUF file handle (disposed on model swap).</summary>
    public GgufFile? DraftGguf { get; set; }

    /// <summary>
    /// Executes a request with sequential access control.
    /// Only one request is processed at a time. Do not dispose this state inside the callback.
    /// </summary>
    public async Task ExecuteAsync(Func<Task> work, CancellationToken ct)
    {
        ObjectDisposedException.ThrowIf(Volatile.Read(ref _disposed) != 0, this);
        await _requestGate.WaitAsync(ct);
        try
        {
            ObjectDisposedException.ThrowIf(Volatile.Read(ref _disposed) != 0, this);
            await work();
        }
        finally { _requestGate.Release(); }
    }

    /// <summary>
    /// Prepares and adopts a fresh, exclusively owned candidate under the canonical request gate.
    /// The callback must not mutate this state or share its owned resources. Returning a candidate
    /// transfers ownership to this method; failure/cancellation preserves the incumbent.
    /// Retired-resource cleanup failure leaves the candidate active and blocks further swaps.
    /// </summary>
    public async Task SwapModelAsync(Func<CancellationToken, Task<ServerState>> prepare, CancellationToken ct)
    {
        ArgumentNullException.ThrowIfNull(prepare);
        ObjectDisposedException.ThrowIf(Volatile.Read(ref _disposed) != 0, this);
        await _requestGate.WaitAsync(ct);
        ServerState? candidate = null;
        try
        {
            ObjectDisposedException.ThrowIf(Volatile.Read(ref _disposed) != 0, this);
            if (RetirementCleanupFailed)
                throw new InvalidOperationException("A prior retirement failed; restart before another model swap.");
            candidate = await prepare(ct);
            if (ReferenceEquals(candidate, this))
            {
                candidate = null; // Never dispose the incumbent when the callback violates ownership.
                throw new InvalidOperationException("Preparation must return a fresh candidate.");
            }
            ct.ThrowIfCancellationRequested();
            ObjectDisposedException.ThrowIf(Volatile.Read(ref _disposed) != 0, this);
            if (candidate is null || Volatile.Read(ref candidate._disposed) != 0 || !candidate.IsReady ||
                candidate.Model is null || candidate.Config is null || candidate.Tokenizer is null ||
                candidate.Generator is null || candidate.ChatTemplate is null || candidate.RetirementCleanupFailed)
                throw new InvalidOperationException("Preparation returned an incomplete candidate.");

            var retired = OwnedResources();
            Options = candidate.Options;
            Config = candidate.Config;
            ToolCallParser = candidate.ToolCallParser;
            KvCacheConfig = candidate.KvCacheConfig;
            KvCacheFactory = candidate.KvCacheFactory;
            PagedFactory = candidate.PagedFactory;
            PrefixCache = candidate.PrefixCache;
            Model = candidate.Model;
            Tokenizer = candidate.Tokenizer;
            ChatTemplate = candidate.ChatTemplate;
            Generator = candidate.Generator;
            LoadedModelPath = candidate.LoadedModelPath;
            CurrentGguf = candidate.CurrentGguf;
            DraftModel = candidate.DraftModel;
            DraftModelPath = candidate.DraftModelPath;
            DraftGguf = candidate.DraftGguf;
            IsReady = true; // Publication boundary; cancellation after this point cannot roll back.
            candidate.ClearModel();
            RetirementCleanupFailed = !DisposeResources(retired);
        }
        finally
        {
            try { candidate?.Dispose(); }
            finally { _requestGate.Release(); }
        }
    }

    /// <inheritdoc/>
    public void Dispose()
    {
        if (Interlocked.Exchange(ref _disposed, 1) != 0) return;
        _requestGate.Wait();
        try
        {
            var resources = OwnedResources();
            ClearModel();
            RetirementCleanupFailed |= !DisposeResources(resources);
        }
        finally { _requestGate.Release(); }
        // Do not dispose the semaphore while already-waiting direct callers still need to wake and refuse.
    }

    private IDisposable?[] OwnedResources() => [PrefixCache, PagedFactory, DraftModel, DraftGguf, Model, CurrentGguf];

    private void ClearModel()
    {
        IsReady = false;
        PrefixCache = null; PagedFactory = null; DraftModel = null; DraftGguf = null;
        Model = null; CurrentGguf = null; Generator = null; Tokenizer = null; ChatTemplate = null;
        Config = null; ToolCallParser = null; KvCacheFactory = null;
        LoadedModelPath = ""; DraftModelPath = "";
    }

    private static bool DisposeResources(IDisposable?[] resources)
    {
        bool success = true;
        var seen = new HashSet<IDisposable>(ReferenceEqualityComparer.Instance);
        foreach (var resource in resources)
        {
            if (resource is null || !seen.Add(resource)) continue;
            try { resource.Dispose(); }
            catch (Exception) { success = false; }
        }
        if (!success) Console.Error.WriteLine("[cnet-llm] Model resource cleanup failed; owner restart required.");
        return success;
    }
}

/// <summary>
/// Immutable sampling parameter defaults that can be changed from the UI.
/// These serve as defaults when the per-request body does not specify a value.
/// Replaced atomically via <c>with</c> expressions to avoid torn reads.
/// </summary>
public sealed record SamplingDefaults
{
    /// <summary>Sampling temperature. 0 = greedy.</summary>
    public float Temperature { get; init; } = 0.0f;

    /// <summary>Top-P (nucleus) sampling threshold.</summary>
    public float TopP { get; init; } = 1.0f;

    /// <summary>Top-K sampling. 0 = disabled.</summary>
    public int TopK { get; init; }

    /// <summary>Min-P sampling threshold. 0 = disabled.</summary>
    public float MinP { get; init; }

    /// <summary>Repetition penalty factor. 1.0 = disabled.</summary>
    public float RepetitionPenalty { get; init; } = 1.0f;

    /// <summary>Maximum tokens to generate per response.</summary>
    public int MaxTokens { get; init; } = 2048;

    /// <summary>Random seed for reproducibility. Null = non-deterministic.</summary>
    public int? Seed { get; init; }
}
