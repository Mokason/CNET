// Managed inference backend for CNET.Cce, over the CNET.Llm engine.
//
// Implements the same ICnetInferenceSession contract as the native
// CnetHarnessSession, so a host can swap backends without touching call sites.
// Config and option validation is shared with the native session rather than
// re-stated here, so the two backends cannot drift apart on what they accept.
//
// What this backend does NOT do, and will not pretend to:
//   * AICIMO routing. There is no adapter selection and no route uncertainty;
//     SelectedAdapter is always 0 and RouteUncertainty always 0. Sampling mode
//     resolves through the static table in CnetLlmSamplingProfile.
//   * GPU execution. Open() rejects GPU resource masks and GpuOffload outright
//     rather than silently falling back to CPU.

using System.Diagnostics;
using CNET.Cce.CnetHarness;
using CNET.Llm.Core.Configuration;
using CNET.Llm.Core.Models;
using CNET.Llm.Core.Sampling;
using CNET.Llm.Engine;
using CNET.Llm.Engine.Samplers;
using CNET.Llm.Engine.Samplers.StopConditions;
using CNET.Llm.Models.Architectures;
using CNET.Llm.Models.Gguf;
using CNET.Llm.Tokenizers;
using CNET.Llm.Tokenizers.Bpe;

namespace CNET.Cce.Llm;

/// <summary>
/// A fully managed <see cref="ICnetInferenceSession"/> backed by the CNET.Llm
/// engine. Loads a GGUF model in-process; requires no native plugin.
/// </summary>
public sealed class CnetLlmInferenceSession : ICnetInferenceSession
{
    private readonly GgufFile _gguf;
    private readonly TransformerModel _model;
    private readonly ITokenizer _tokenizer;
    private readonly IChatTemplate? _chatTemplate;
    private readonly TextGenerator _generator;
    private readonly ThreadingConfig _threading;
    private readonly int _contextTokens;
    private readonly object _gate = new();
    private bool _disposed;

    /// <summary>Architecture reported by the loaded GGUF, for host diagnostics.</summary>
    public string Architecture { get; }

    /// <summary>True when the GGUF carried a chat template that System/User messages are rendered through.</summary>
    public bool HasChatTemplate => _chatTemplate is not null;

    private CnetLlmInferenceSession(GgufFile gguf, TransformerModel model,
                                    ITokenizer tokenizer, IChatTemplate? chatTemplate,
                                    TextGenerator generator, ThreadingConfig threading,
                                    int contextTokens, string architecture)
    {
        _gguf = gguf;
        _model = model;
        _tokenizer = tokenizer;
        _chatTemplate = chatTemplate;
        _generator = generator;
        _threading = threading;
        _contextTokens = contextTokens;
        Architecture = architecture;
    }

    /// <summary>
    /// Open a managed session over the GGUF at <see cref="CnetHarnessConfig.ModelPath"/>.
    /// </summary>
    /// <exception cref="CnetHarnessException">
    /// <see cref="CnetHarnessStatus.InvalidArgument"/> when the config asks for
    /// something this backend does not implement (GPU resource or offload);
    /// <see cref="CnetHarnessStatus.ModelLoadFailed"/> when the model cannot be read.
    /// </exception>
    public static CnetLlmInferenceSession Open(CnetHarnessConfig config)
    {
        ArgumentNullException.ThrowIfNull(config);

        // Same validation the native backend applies — one source of truth.
        try
        {
            CnetHarnessSession.ValidateConfig(config);
        }
        catch (ArgumentException ex)
        {
            throw new CnetHarnessException(CnetHarnessStatus.InvalidArgument, ex.Message);
        }

        if (config.ResourceMask != (ulong)CnetHarnessResource.Cpu)
        {
            throw new CnetHarnessException(CnetHarnessStatus.InvalidArgument,
                "the managed CNET.Llm backend executes on CPU only; " +
                "use CnetHarnessResource.Cpu, or open a native CnetHarnessSession for GPU");
        }
        if (config.GpuOffload is not null)
        {
            throw new CnetHarnessException(CnetHarnessStatus.InvalidArgument,
                "the managed CNET.Llm backend does not implement GpuOffload; " +
                "open a native CnetHarnessSession for partial offload");
        }
        if (!File.Exists(config.ModelPath))
        {
            throw new CnetHarnessException(CnetHarnessStatus.ModelLoadFailed,
                $"model file not found: {config.ModelPath}");
        }

        GgufFile? gguf = null;
        TransformerModel? model = null;
        try
        {
            gguf = GgufFile.Open(config.ModelPath);
            ModelConfig modelConfig = GgufModelConfigExtractor.Extract(gguf.Metadata);

            // Threading MUST be passed here. TransformerModel builds its
            // ComputeThreadPool at load time from this argument; the two-arg
            // overload defaults to ThreadingConfig.SingleThreaded, and setting
            // InferenceOptions.Threading afterwards does NOT create a pool. Load
            // without it and every matmul runs on one thread.
            var threading = new ThreadingConfig((int)config.Threads);
            model = TransformerModel.LoadFromGguf(gguf, modelConfig, threading);

            ITokenizer tokenizer = GgufBpeTokenizerFactory.Load(gguf.Metadata);
            IChatTemplate? chatTemplate = GgufChatTemplateFactory.TryCreate(gguf.Metadata, tokenizer);
            var generator = new TextGenerator(model, tokenizer);

            return new CnetLlmInferenceSession(
                gguf, model, tokenizer, chatTemplate, generator, threading,
                (int)config.ContextTokens, modelConfig.Architecture.ToString());
        }
        catch (CnetHarnessException)
        {
            model?.Dispose();
            gguf?.Dispose();
            throw;
        }
        catch (Exception ex)
        {
            model?.Dispose();
            gguf?.Dispose();
            throw new CnetHarnessException(CnetHarnessStatus.ModelLoadFailed,
                $"failed to load {config.ModelPath}: {ex.Message}");
        }
    }

    /// <inheritdoc />
    public CnetHarnessGenerationResult Generate(CnetHarnessGenerateOptions options)
    {
        lock (_gate)
        {
            return GenerateCore(options);
        }
    }

    private CnetHarnessGenerationResult GenerateCore(CnetHarnessGenerateOptions options)
    {
        ArgumentNullException.ThrowIfNull(options);
        ObjectDisposedException.ThrowIf(_disposed, this);
        try
        {
            CnetHarnessSession.ValidateGenerateOptions(options);
        }
        catch (ArgumentException ex)
        {
            throw new CnetHarnessException(CnetHarnessStatus.InvalidArgument, ex.Message);
        }

        CnetLlmSamplingProfile profile = CnetLlmSamplingProfile.Resolve(options.Sampling);
        string prompt = BuildPrompt(options);

        // Greedy has to go through InferenceOptions' auto-build path:
        // SamplerPipeline only sets its greedy flag when SamplerSteps is null.
        // An empty step array is NOT greedy — it samples categorically from the
        // raw distribution, which is the opposite of what Deterministic means.
        bool greedy = profile.Temperature <= 0.0f;

        var inferenceOptions = new InferenceOptions
        {
            SamplerSteps = greedy ? null : BuildSamplerSteps(profile),
            Temperature = profile.Temperature,
            StopConditions =
            [
                new EosStopCondition(_tokenizer.EosTokenId),
                new MaxTokensStopCondition((int)options.MaxTokens),
            ],
            // CnetHarness seeds are uint; InferenceOptions takes a signed seed.
            // Mask rather than cast so a high seed cannot land negative.
            Seed = (int)(options.Seed & 0x7FFFFFFFu),
            MaxTokens = (int)options.MaxTokens,
            Threading = _threading,
        };

        InferenceResponse response;
        var wall = Stopwatch.StartNew();
        try
        {
            response = _generator.Generate(prompt, inferenceOptions);
        }
        catch (Exception ex)
        {
            throw new CnetHarnessException(CnetHarnessStatus.BackendFailure,
                $"CNET.Llm generation failed: {ex.Message}");
        }
        wall.Stop();

        InferenceTimings timings = response.Timings;
        // GenerationMs is decode + sampling: the wall time attributable to
        // producing tokens, matching what the native backend reports. Prefill is
        // reported separately as PromptMs.
        double generationMs = timings.DecodeTimeMs + timings.SamplingTimeMs;
        if (generationMs <= 0.0) generationMs = wall.Elapsed.TotalMilliseconds;

        return new CnetHarnessGenerationResult(
            response.Text,
            (uint)Math.Max(0, response.PromptTokenCount),
            (uint)Math.Max(0, response.GeneratedTokenCount),
            timings.PrefillTimeMs,
            generationMs,
            SelectedAdapter: 0u,        // no AICIMO adapter set in the managed path
            RouteUncertainty: 0.0f,     // no route decision was made
            profile.Mode,
            AicimoOverride: false,
            profile.Temperature,
            profile.TopP,
            profile.TopK,
            profile.MinP);
    }

    /// <inheritdoc />
    /// <remarks>
    /// Reports the static profile this backend would apply. There is no AICIMO
    /// here, so <c>SelectedAdapter</c> and <c>RouteUncertainty</c> are always 0 —
    /// they are not a measured route decision. <paramref name="role"/> is
    /// validated for parity with the native backend but does not influence the
    /// result.
    /// </remarks>
    public CnetHarnessRouteInfo ProbeRoute(string role,
        CnetHarnessSamplingMode overrideMode = CnetHarnessSamplingMode.Auto)
    {
        ArgumentException.ThrowIfNullOrEmpty(role);
        ObjectDisposedException.ThrowIf(_disposed, this);
        if (!CnetHarnessSession.SamplingModeValid(overrideMode))
            throw new ArgumentOutOfRangeException(nameof(overrideMode));

        CnetLlmSamplingProfile profile = CnetLlmSamplingProfile.Resolve(overrideMode);
        return new CnetHarnessRouteInfo(
            SelectedAdapter: 0u,
            RouteUncertainty: 0.0f,
            profile.Mode,
            profile.Temperature,
            profile.TopP,
            profile.TopK,
            profile.MinP);
    }

    /// <summary>
    /// Render System/User into the model's prompt format. Uses the GGUF chat
    /// template when the model carries one; otherwise falls back to a plain
    /// concatenation, which is the honest thing to do for a base model that has
    /// no turn structure to honour.
    /// </summary>
    private string BuildPrompt(CnetHarnessGenerateOptions options)
    {
        if (_chatTemplate is null)
        {
            return string.IsNullOrEmpty(options.System)
                ? options.User
                : $"{options.System}\n\n{options.User}";
        }

        var messages = new List<ChatMessage>(2);
        if (!string.IsNullOrEmpty(options.System))
            messages.Add(new ChatMessage { Role = "system", Content = options.System });
        messages.Add(new ChatMessage { Role = "user", Content = options.User });

        return _chatTemplate.Apply(messages,
            new ChatTemplateOptions { AddGenerationPrompt = true });
    }

    /// <summary>
    /// Sampler steps for the stochastic profiles. Never called for the
    /// deterministic profile — that one passes <c>SamplerSteps = null</c> so the
    /// pipeline takes its greedy path; see the note in <see cref="GenerateCore"/>.
    /// </summary>
    private static ISamplerStep[] BuildSamplerSteps(CnetLlmSamplingProfile p)
    {
        var steps = new List<ISamplerStep>(4) { new TemperatureSampler(p.Temperature) };
        if (p.TopK > 0) steps.Add(new TopKSampler((int)p.TopK));
        if (p.TopP is > 0.0f and < 1.0f) steps.Add(new TopPSampler(p.TopP));
        if (p.MinP > 0.0f) steps.Add(new MinPSampler(p.MinP));
        return [.. steps];
    }

    /// <inheritdoc />
    public void Dispose()
    {
        lock (_gate)
        {
            if (_disposed) return;
            _disposed = true;
            _model.Dispose();
            _gguf.Dispose();
        }
    }
}
