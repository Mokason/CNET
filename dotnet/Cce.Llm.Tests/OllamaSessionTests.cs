using System.Net;
using System.Text;
using System.Text.Json;
using CNET.Cce.CnetHarness;
using CNET.Cce.Llm;
using CNET.Cce.Llm.Memory;
using CNET.Cce.Llm.Ollama;
using Xunit;

namespace CNET.Cce.Llm.Tests;

/// <summary>
/// OllamaSession over a fake HTTP handler — no server, no network. Pins the
/// NDJSON stream assembly, the option mapping (what actually goes on the
/// wire), the error paths, and the thinking-model shape where reasoning is
/// separate from content.
/// </summary>
public sealed class OllamaSessionTests
{
    private sealed class FakeHandler : HttpMessageHandler
    {
        public Func<HttpRequestMessage, string?, HttpResponseMessage> Respond = null!;
        public string? LastChatBody;

        protected override HttpResponseMessage Send(HttpRequestMessage request,
            CancellationToken cancellationToken)
        {
            string? body = request.Content?.ReadAsStringAsync(cancellationToken)
                .GetAwaiter().GetResult();
            if (request.RequestUri!.AbsolutePath.EndsWith("api/chat", StringComparison.Ordinal))
                LastChatBody = body;
            return Respond(request, body);
        }

        protected override Task<HttpResponseMessage> SendAsync(HttpRequestMessage request,
            CancellationToken cancellationToken)
            => Task.FromResult(Send(request, cancellationToken));
    }

    private static HttpResponseMessage Ok(string content) => new(HttpStatusCode.OK)
    {
        Content = new StringContent(content, Encoding.UTF8, "application/x-ndjson"),
    };

    private static string Version() => "{\"version\":\"0.23.1\"}";

    /// <summary>Canned stream: two content chunks, a thinking chunk, a done frame.</summary>
    private static string CannedStream() =>
        "{\"message\":{\"role\":\"assistant\",\"thinking\":\"pondering...\",\"content\":\"\"},\"done\":false}\n" +
        "{\"message\":{\"role\":\"assistant\",\"content\":\"Hello \"},\"done\":false}\n" +
        "{\"message\":{\"role\":\"assistant\",\"content\":\"ghost\"},\"done\":false}\n" +
        "{\"message\":{\"role\":\"assistant\",\"content\":\"\"},\"done\":true," +
        "\"done_reason\":\"stop\",\"prompt_eval_count\":42,\"eval_count\":7," +
        "\"prompt_eval_duration\":2000000,\"eval_duration\":350000000}\n";

    private static OllamaSession OpenFake(FakeHandler handler)
    {
        var http = new HttpClient(handler) { BaseAddress = new Uri("http://fake:11434/") };
        return OllamaSession.Open("test-model:cloud", httpClient: http);
    }

    private static FakeHandler HandlerWith(string chatStream) => new()
    {
        Respond = (req, _) => req.RequestUri!.AbsolutePath.EndsWith("api/version", StringComparison.Ordinal)
            ? Ok(Version())
            : Ok(chatStream),
    };

    private static CnetHarnessGenerateOptions Options(uint maxTokens = 64) => new()
    {
        System = "sys",
        User = "hello",
        Role = "chat",
        MaxTokens = maxTokens,
        Seed = 424242,
        Sampling = CnetHarnessSamplingMode.Deterministic,
    };

    [Fact]
    public void AssemblesContent_AndIgnoresThinking()
    {
        using var session = OpenFake(HandlerWith(CannedStream()));
        var r = session.Generate(Options());

        Assert.Equal("Hello ghost", r.Text);           // thinking never leaks into text
        Assert.Equal(42u, r.PromptTokens);
        Assert.Equal(7u, r.GeneratedTokens);
        Assert.Equal(2.0, r.PromptMs, 3);              // ns -> ms
        Assert.Equal(350.0, r.GenerationMs, 3);
        Assert.Equal(CnetHarnessSamplingMode.Deterministic, r.EffectiveSampling);
        Assert.False(r.AicimoOverride);
    }

    [Fact]
    public void StreamsChunks_ThroughOnToken_BeforeReturning()
    {
        using var session = OpenFake(HandlerWith(CannedStream()));
        var chunks = new List<string>();
        session.OnToken = chunks.Add;

        session.Generate(Options());

        Assert.Equal(["Hello ", "ghost"], chunks);      // empty chunks skipped
    }

    [Fact]
    public void WireBody_CarriesProfileSeedAndBudget()
    {
        var handler = HandlerWith(CannedStream());
        using var session = OpenFake(handler);
        session.Generate(Options(maxTokens: 128));

        using JsonDocument doc = JsonDocument.Parse(handler.LastChatBody!);
        JsonElement root = doc.RootElement;
        Assert.Equal("test-model:cloud", root.GetProperty("model").GetString());
        Assert.True(root.GetProperty("stream").GetBoolean());

        JsonElement opts = root.GetProperty("options");
        Assert.Equal(0.0f, opts.GetProperty("temperature").GetSingle());   // deterministic
        Assert.Equal(128, opts.GetProperty("num_predict").GetInt32());
        Assert.Equal(424242, opts.GetProperty("seed").GetInt32());

        JsonElement messages = root.GetProperty("messages");
        Assert.Equal(2, messages.GetArrayLength());
        Assert.Equal("system", messages[0].GetProperty("role").GetString());
        Assert.Equal("user", messages[1].GetProperty("role").GetString());
    }

    [Fact]
    public void NoSystem_SendsSingleUserMessage()
    {
        var handler = HandlerWith(CannedStream());
        using var session = OpenFake(handler);
        session.Generate(new CnetHarnessGenerateOptions
        {
            User = "hi", Role = "chat", MaxTokens = 8,
        });

        using JsonDocument doc = JsonDocument.Parse(handler.LastChatBody!);
        Assert.Equal(1, doc.RootElement.GetProperty("messages").GetArrayLength());
    }

    [Fact]
    public void ThinkFlag_IsForwardedOnlyWhenSet()
    {
        var handler = HandlerWith(CannedStream());
        using var session = OpenFake(handler);

        session.Generate(Options());
        using (JsonDocument doc = JsonDocument.Parse(handler.LastChatBody!))
            Assert.False(doc.RootElement.TryGetProperty("think", out _));

        session.Think = false;
        session.Generate(Options());
        using (JsonDocument doc = JsonDocument.Parse(handler.LastChatBody!))
            Assert.False(doc.RootElement.GetProperty("think").GetBoolean());
    }

    [Fact]
    public void MissingModel_MapsTo_ModelLoadFailed()
    {
        var handler = new FakeHandler
        {
            Respond = (req, _) => req.RequestUri!.AbsolutePath.EndsWith("api/version", StringComparison.Ordinal)
                ? Ok(Version())
                : new HttpResponseMessage(HttpStatusCode.NotFound)
                {
                    Content = new StringContent("{\"error\":\"model not found\"}"),
                },
        };
        using var session = OpenFake(handler);

        var ex = Assert.Throws<CnetHarnessException>(() => session.Generate(Options()));
        Assert.Equal(CnetHarnessStatus.ModelLoadFailed, ex.Status);
    }

    [Fact]
    public void ErrorFrameInStream_MapsTo_BackendFailure()
    {
        using var session = OpenFake(HandlerWith(
            "{\"message\":{\"content\":\"par\"},\"done\":false}\n" +
            "{\"error\":\"upstream disconnected\"}\n"));

        var ex = Assert.Throws<CnetHarnessException>(() => session.Generate(Options()));
        Assert.Equal(CnetHarnessStatus.BackendFailure, ex.Status);
        Assert.Contains("upstream disconnected", ex.Message);
    }

    [Fact]
    public void TruncatedStream_WithoutDone_IsAnError_NotASilentPartial()
    {
        using var session = OpenFake(HandlerWith(
            "{\"message\":{\"content\":\"half an ans\"},\"done\":false}\n"));

        var ex = Assert.Throws<CnetHarnessException>(() => session.Generate(Options()));
        Assert.Equal(CnetHarnessStatus.BackendFailure, ex.Status);
        Assert.Contains("without a done frame", ex.Message);
    }

    [Fact]
    public void UnreachableServer_FailsAtOpen_WithClearMessage()
    {
        var handler = new FakeHandler
        {
            Respond = (_, _) => throw new HttpRequestException("connection refused"),
        };
        var http = new HttpClient(handler) { BaseAddress = new Uri("http://fake:11434/") };

        var ex = Assert.Throws<CnetHarnessException>(
            () => OllamaSession.Open("m", httpClient: http));
        Assert.Equal(CnetHarnessStatus.BackendFailure, ex.Status);
        Assert.Contains("not reachable", ex.Message);
    }

    [Fact]
    public void GhostMemory_ComposesOverOllamaSession()
    {
        // The whole point: MemorySession neither knows nor cares that the
        // "engine" is a remote server.
        string dir = Path.Combine(Path.GetTempPath(), "ghost-ollama-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(dir);
        try
        {
            using var session = OpenFake(HandlerWith(CannedStream()));
            using var store = BlobStore.Open(Path.Combine(dir, "g.jsonl"));
            var ghost = new MemorySession(session,
                new ConversationMemory(store, session.CountTokens), 8192);

            var r = ghost.Generate("sys", "hello there", 64);

            Assert.Equal("Hello ghost", r.Result.Text);
            Assert.True(store.Count >= 2);   // user turn + assistant turn stored
        }
        finally
        {
            Directory.Delete(dir, recursive: true);
        }
    }

    [Fact]
    public void UsableThroughTheSharedInterface()
    {
        using ICnetInferenceSession session = OpenFake(HandlerWith(CannedStream()));
        Assert.Equal("Hello ghost", session.Generate(Options()).Text);
    }
}
