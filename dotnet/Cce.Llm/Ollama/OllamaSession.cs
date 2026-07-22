using System.Text;
using System.Text.Json;
using CNET.Cce.CnetHarness;

namespace CNET.Cce.Llm.Ollama;

/// <summary>
/// <see cref="ICnetInferenceSession"/> over an Ollama server (`/api/chat`) —
/// local daemon or ollama.com cloud models through it.
/// </summary>
/// <remarks>
/// The third backend behind the shared interface, and the one where "model
/// loading" disappears entirely: a <c>*-cloud</c> model's weights never touch
/// this machine — tokens stream back over HTTP while the model runs remotely.
/// Ghost memory works unchanged on top, which yields the strongest form of the
/// engine-swap property: a fact stored during a 135M local session can be
/// recalled into a frontier-scale cloud model's context, because the memory
/// lives in the store file, not in any engine.
/// <para>
/// Fidelity notes, stated plainly like the managed backend's: no AICIMO
/// (<c>SelectedAdapter</c>/<c>RouteUncertainty</c> are 0, profile resolution is
/// the same static table via <see cref="CnetLlmSamplingProfile"/>); and Ollama
/// exposes no tokenizer API, so <see cref="CountTokens"/> is a character-class
/// estimate (ASCII ~3 chars/token, non-ASCII 1 token/char) that errs toward
/// over-counting; set <see cref="ContextTokens"/> so the server's window
/// matches the budgeter's. Thinking models (e.g. minimax-m3) spend
/// part of <c>num_predict</c> on reasoning that is returned separately; only
/// <c>message.content</c> is surfaced as the result text.
/// </para>
/// <para>
/// Concurrency contract, identical to the other backends (the harness ABI
/// requires per-session serialization): <see cref="Generate"/> holds the
/// session lock for the WHOLE streamed exchange, so a concurrent Generate or
/// Dispose blocks until the in-flight call completes —
/// <see cref="RequestTimeout"/> bounds that wait. Use separate sessions for
/// concurrent conversations.
/// </para>
/// </remarks>
public sealed class OllamaSession : ICnetInferenceSession
{
    private static readonly JsonSerializerOptions JsonOptions = new(JsonSerializerDefaults.Web);

    private readonly HttpClient _http;
    private readonly bool _ownsHttp;
    private readonly string _model;
    private readonly object _gate = new();
    private bool _disposed;

    /// <summary>Model name as known to the server (e.g. <c>minimax-m3:cloud</c>).</summary>
    public string Model => _model;

    /// <summary>
    /// Invoked with each content chunk as it streams in, before
    /// <see cref="Generate"/> returns — lets a UI print tokens live. Runs on
    /// the calling thread; exceptions propagate as generation failure.
    /// </summary>
    public Action<string>? OnToken { get; set; }

    /// <summary>
    /// When set, forwarded as the request's <c>think</c> field (thinking models
    /// only). Null omits the field and leaves the model's default.
    /// </summary>
    public bool? Think { get; set; }

    /// <summary>
    /// Hard bound on one Generate call, INCLUDING reading the streamed body.
    /// HttpClient.Timeout does not cover body reads under
    /// ResponseHeadersRead, so without this a stalled stream would hang
    /// Generate forever while holding the session lock.
    /// </summary>
    public TimeSpan RequestTimeout { get; set; } = TimeSpan.FromMinutes(10);

    /// <summary>
    /// When set, forwarded as <c>options.num_ctx</c>. Without it the server
    /// applies the model's default context and SILENTLY truncates longer
    /// prompts — a memory-packed prompt would lose its oldest blobs with no
    /// error. Hosts that budget against a window must set this to that window.
    /// </summary>
    public int? ContextTokens { get; set; }

    private OllamaSession(HttpClient http, bool ownsHttp, string model)
    {
        _http = http;
        _ownsHttp = ownsHttp;
        _model = model;
    }

    /// <summary>
    /// Opens a session against <paramref name="baseUrl"/> (default local daemon)
    /// and verifies the server is reachable.
    /// </summary>
    /// <param name="model">Model name; cloud models use their <c>:cloud</c> tag.</param>
    /// <param name="baseUrl">Server root, e.g. <c>http://localhost:11434</c>.</param>
    /// <param name="httpClient">Injectable for tests; owned by the caller when supplied.</param>
    public static OllamaSession Open(string model, string baseUrl = "http://localhost:11434",
                                     HttpClient? httpClient = null)
    {
        ArgumentException.ThrowIfNullOrEmpty(model);
        ArgumentException.ThrowIfNullOrEmpty(baseUrl);

        bool owns = httpClient is null;
        HttpClient http = httpClient ?? new HttpClient { Timeout = TimeSpan.FromMinutes(10) };
        // Precedence: a caller-supplied client that already carries a
        // BaseAddress keeps it (test injection, custom routing); otherwise
        // baseUrl is applied. A malformed baseUrl is the caller's error, not a
        // backend failure.
        if (http.BaseAddress is null)
        {
            try
            {
                http.BaseAddress = new Uri(baseUrl.TrimEnd('/') + "/");
            }
            catch (UriFormatException ex)
            {
                if (owns) http.Dispose();
                throw new CnetHarnessException(CnetHarnessStatus.InvalidArgument,
                    $"invalid ollama baseUrl '{baseUrl}': {ex.Message}");
            }
            catch (InvalidOperationException ex)
            {
                // An injected client that has already sent a request cannot
                // accept a BaseAddress.
                if (owns) http.Dispose();
                throw new CnetHarnessException(CnetHarnessStatus.InvalidArgument,
                    $"supplied HttpClient cannot take a BaseAddress: {ex.Message}");
            }
        }

        try
        {
            using HttpResponseMessage probe = http.GetAsync("api/version")
                .GetAwaiter().GetResult();
            if (!probe.IsSuccessStatusCode)
                throw new CnetHarnessException(CnetHarnessStatus.BackendFailure,
                    $"ollama server at {http.BaseAddress} answered {(int)probe.StatusCode} to api/version");
        }
        catch (CnetHarnessException)
        {
            if (owns) http.Dispose();
            throw;
        }
        catch (Exception ex) when (ex is HttpRequestException or TaskCanceledException or IOException)
        {
            if (owns) http.Dispose();
            throw new CnetHarnessException(CnetHarnessStatus.BackendFailure,
                $"ollama server not reachable at {http.BaseAddress}: {ex.Message}");
        }

        return new OllamaSession(http, owns, model);
    }

    /// <inheritdoc />
    public CnetHarnessGenerationResult Generate(CnetHarnessGenerateOptions options)
    {
        ArgumentNullException.ThrowIfNull(options);
        lock (_gate)
        {
            ObjectDisposedException.ThrowIf(_disposed, this);
            try
            {
                CnetHarnessSession.ValidateGenerateOptions(options);
            }
            catch (ArgumentException ex)
            {
                throw new CnetHarnessException(CnetHarnessStatus.InvalidArgument, ex.Message);
            }
            return GenerateCore(options);
        }
    }

    private CnetHarnessGenerationResult GenerateCore(CnetHarnessGenerateOptions options)
    {
        CnetLlmSamplingProfile profile = CnetLlmSamplingProfile.Resolve(options.Sampling);

        var messages = new List<object>(2);
        if (!string.IsNullOrEmpty(options.System))
            messages.Add(new { role = "system", content = options.System });
        messages.Add(new { role = "user", content = options.User });

        var body = new Dictionary<string, object?>
        {
            ["model"] = _model,
            ["messages"] = messages,
            ["stream"] = true,
            ["options"] = new Dictionary<string, object>
            {
                ["temperature"] = profile.Temperature,
                ["top_p"] = profile.TopP,
                ["top_k"] = (int)profile.TopK,
                ["min_p"] = profile.MinP,
                ["seed"] = (long)options.Seed,   // full uint range; masking to
                                                 // int31 collided seeds differing
                                                 // only in the top bit
                ["num_predict"] = (int)Math.Min(options.MaxTokens, int.MaxValue),
            },
        };
        if (ContextTokens is int nctx)
            ((Dictionary<string, object>)body["options"]!)["num_ctx"] = nctx;
        if (Think is not null) body["think"] = Think;

        using var request = new HttpRequestMessage(HttpMethod.Post, "api/chat")
        {
            Content = new StringContent(JsonSerializer.Serialize(body, JsonOptions),
                Encoding.UTF8, "application/json"),
        };

        using var timeout = new CancellationTokenSource(RequestTimeout);
        try
        {
            using HttpResponseMessage response = _http
                .Send(request, HttpCompletionOption.ResponseHeadersRead, timeout.Token);

            if (!response.IsSuccessStatusCode)
            {
                string detail = ReadErrorDetail(response);
                CnetHarnessStatus status = response.StatusCode == System.Net.HttpStatusCode.NotFound
                    ? CnetHarnessStatus.ModelLoadFailed
                    : CnetHarnessStatus.BackendFailure;
                throw new CnetHarnessException(status,
                    $"ollama api/chat for '{_model}' failed with {(int)response.StatusCode}: {detail}");
            }

            return ReadStream(response, profile, timeout.Token);
        }
        catch (HttpRequestException ex)
        {
            throw new CnetHarnessException(CnetHarnessStatus.BackendFailure,
                $"ollama request failed: {ex.Message}");
        }
        catch (NotSupportedException ex)
        {
            // Injected handlers that implement only SendAsync cannot serve the
            // synchronous Send this session uses.
            throw new CnetHarnessException(CnetHarnessStatus.BackendFailure,
                $"supplied HttpClient handler does not support synchronous Send: {ex.Message}");
        }
        catch (IOException ex)
        {
            // A dropped connection mid-stream surfaces as IOException from the
            // response stream, not HttpRequestException from Send.
            throw new CnetHarnessException(CnetHarnessStatus.BackendFailure,
                $"ollama stream broke mid-response: {ex.Message}");
        }
        catch (OperationCanceledException)
        {
            throw new CnetHarnessException(CnetHarnessStatus.BackendFailure,
                $"ollama request exceeded RequestTimeout ({RequestTimeout.TotalSeconds:F0}s) — " +
                "stalled stream or overloaded server");
        }
    }

    private CnetHarnessGenerationResult ReadStream(HttpResponseMessage response,
                                                   CnetLlmSamplingProfile profile,
                                                   CancellationToken cancel)
    {
        var text = new StringBuilder();
        long promptEvalNs = 0, evalNs = 0;
        uint promptTokens = 0, generatedTokens = 0;
        bool sawDone = false;

        using Stream stream = response.Content.ReadAsStream();
        using var reader = new StreamReader(stream, Encoding.UTF8);

        string? line;
        // ReadLineAsync honors the token; the sync ReadLine cannot be
        // interrupted and is exactly how the stall-hang happened.
        while ((line = reader.ReadLineAsync(cancel).AsTask().GetAwaiter().GetResult()) is not null)
        {
            if (string.IsNullOrWhiteSpace(line)) continue;

            using JsonDocument doc = ParseChunk(line);
            JsonElement root = doc.RootElement;

            if (root.TryGetProperty("error", out JsonElement err))
                throw new CnetHarnessException(CnetHarnessStatus.BackendFailure,
                    $"ollama reported: {err.GetString()}");

            if (root.TryGetProperty("message", out JsonElement msg) &&
                msg.TryGetProperty("content", out JsonElement content))
            {
                string? chunk = content.GetString();
                if (!string.IsNullOrEmpty(chunk))
                {
                    text.Append(chunk);
                    OnToken?.Invoke(chunk);
                }
            }

            if (root.TryGetProperty("done", out JsonElement done) && done.GetBoolean())
            {
                sawDone = true;
                if (root.TryGetProperty("prompt_eval_count", out JsonElement pc))
                    promptTokens = (uint)Math.Max(0, pc.GetInt64());
                if (root.TryGetProperty("eval_count", out JsonElement ec))
                    generatedTokens = (uint)Math.Max(0, ec.GetInt64());
                if (root.TryGetProperty("prompt_eval_duration", out JsonElement pd))
                    promptEvalNs = pd.GetInt64();
                if (root.TryGetProperty("eval_duration", out JsonElement ed))
                    evalNs = ed.GetInt64();
            }
        }

        if (!sawDone)
            throw new CnetHarnessException(CnetHarnessStatus.BackendFailure,
                "ollama stream ended without a done frame — response is incomplete");

        return new CnetHarnessGenerationResult(
            text.ToString(),
            promptTokens,
            generatedTokens,
            promptEvalNs / 1_000_000.0,
            evalNs / 1_000_000.0,
            SelectedAdapter: 0u,
            RouteUncertainty: 0.0f,
            profile.Mode,
            AicimoOverride: false,
            profile.Temperature,
            profile.TopP,
            profile.TopK,
            profile.MinP);
    }

    private static JsonDocument ParseChunk(string line)
    {
        try
        {
            return JsonDocument.Parse(line);
        }
        catch (JsonException ex)
        {
            throw new CnetHarnessException(CnetHarnessStatus.BackendFailure,
                $"unparseable ollama stream line: {ex.Message}");
        }
    }

    private static string ReadErrorDetail(HttpResponseMessage response)
    {
        try
        {
            string body = response.Content.ReadAsStringAsync().GetAwaiter().GetResult();
            return body.Length > 300 ? body[..300] : body;
        }
        catch (Exception e) when (e is HttpRequestException or IOException)
        {
            return "(no body)";
        }
    }

    /// <inheritdoc />
    public CnetHarnessRouteInfo ProbeRoute(string role,
        CnetHarnessSamplingMode overrideMode = CnetHarnessSamplingMode.Auto)
    {
        ArgumentException.ThrowIfNullOrEmpty(role);
        ObjectDisposedException.ThrowIf(_disposed, this);
        if (!CnetHarnessSession.SamplingModeValid(overrideMode))
            throw new ArgumentOutOfRangeException(nameof(overrideMode));

        CnetLlmSamplingProfile p = CnetLlmSamplingProfile.Resolve(overrideMode);
        return new CnetHarnessRouteInfo(0u, 0.0f, p.Mode,
            p.Temperature, p.TopP, p.TopK, p.MinP);
    }

    /// <summary>
    /// Token estimate (~4 chars/token): Ollama exposes no tokenizer endpoint.
    /// Over-estimating is the safe direction for budget packing, and the
    /// server's own context handling is the hard backstop.
    /// </summary>
    public int CountTokens(string text)
    {
        ArgumentNullException.ThrowIfNull(text);
        // Length/3 alone UNDER-estimates token-dense scripts (CJK is roughly a
        // token per character), which would let the budgeter overpack. Count
        // non-ASCII characters as one token each; that over-estimates a little
        // for accented European text, which is the safe direction.
        int ascii = 0, other = 0;
        foreach (char c in text)
        {
            if (c < 128) ascii++;
            else other++;
        }
        return ascii / 3 + other + 1;
    }

    public void Dispose()
    {
        lock (_gate)
        {
            if (_disposed) return;
            _disposed = true;
            if (_ownsHttp) _http.Dispose();
        }
    }
}
