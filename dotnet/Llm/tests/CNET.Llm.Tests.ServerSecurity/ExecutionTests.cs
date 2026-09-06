using System.Net;
using System.Net.Http.Json;
using System.Net.Sockets;
using System.Text;
using System.Text.Json;
using Microsoft.AspNetCore.Builder;
using Xunit;

namespace CNET.Llm.Tests.ServerSecurity;

public sealed class ExecutionTests
{
    [Fact]
    public async Task ActualEngineCompletionAndStreamingRespectOutputCap()
    {
        var engine = new FixtureEngine();
        await using var server = await ServerFixture.Start(ServerFixture.Policy with { MaxOutputTokens = 3 }, engine.CreateState());
        server.Authenticate();
        using var response = await server.Client.PostAsJsonAsync("/v1/completions", new { prompt = "hello" });
        Assert.Equal(HttpStatusCode.OK, response.StatusCode);
        using var json = JsonDocument.Parse(await response.Content.ReadAsStringAsync());
        Assert.Equal(3, json.RootElement.GetProperty("usage").GetProperty("completion_tokens").GetInt32());
        Assert.Equal(3, engine.Calls);
        using var streamed = await server.Client.PostAsJsonAsync("/v1/chat/completions", new
        {
            messages = new[] { new { role = "user", content = "hello" } }, stream = true, max_tokens = 2,
        });
        string text = await streamed.Content.ReadAsStringAsync();
        Assert.Equal(HttpStatusCode.OK, streamed.StatusCode);
        Assert.True(streamed.Headers.CacheControl?.NoStore == true, "MANAGED_SERVER_RED stream_must_not_be_stored");
        Assert.Contains("data: [DONE]", text);
        Assert.Equal(5, engine.Calls);
    }

    [Fact]
    public async Task DeadlineStopsNonStreamingGenerationAtTokenBoundary()
    {
        var engine = new FixtureEngine { OnForward = _ => Thread.Sleep(40) };
        await using var server = await ServerFixture.Start(ServerFixture.Policy with { RequestTimeout = TimeSpan.FromMilliseconds(300) }, engine.CreateState());
        server.Authenticate();
        using var response = await server.Client.PostAsJsonAsync("/v1/completions", new { prompt = "hello", max_tokens = 100 });
        Assert.Equal(HttpStatusCode.GatewayTimeout, response.StatusCode);
        Assert.InRange(engine.Calls, 1, 30);
        int calls = engine.Calls;
        Assert.Equal(HttpStatusCode.OK, (await server.Client.GetAsync("/health")).StatusCode);
        Assert.Equal(calls, engine.Calls);
    }

    [Fact]
    public async Task BusyWorkHasNoWaitingQueueAndCancellationRetainsOwnershipUntilExit()
    {
        using var release = new ManualResetEventSlim();
        var entered = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var engine = new FixtureEngine { OnForward = _ => { entered.TrySetResult(); release.Wait(TimeSpan.FromSeconds(5)); } };
        await using var server = await ServerFixture.Start(state: engine.CreateState());
        server.Authenticate();
        using var cancellation = new CancellationTokenSource();
        var active = server.Client.PostAsJsonAsync("/v1/completions", new { prompt = "hello", max_tokens = 2 }, cancellation.Token);
        try
        {
            await entered.Task.WaitAsync(TimeSpan.FromSeconds(3));
            Assert.Equal(HttpStatusCode.TooManyRequests, (await server.Client.GetAsync("/health")).StatusCode);
            server.Authenticate(ServerFixture.AdminKey);
            Assert.Equal(HttpStatusCode.TooManyRequests,
                (await server.Client.PostAsJsonAsync("/v1/config", new { temperature = 0.8 })).StatusCode);
            cancellation.Cancel();
            await Assert.ThrowsAnyAsync<OperationCanceledException>(async () => await active);
            Assert.Equal(HttpStatusCode.TooManyRequests, (await server.Client.GetAsync("/health")).StatusCode);
            Assert.Equal(0, server.State.SamplingDefaults.Temperature);
        }
        finally { release.Set(); }
        for (int i = 0; i < 50; i++)
        {
            if ((await server.Client.GetAsync("/health")).StatusCode == HttpStatusCode.OK) break;
            await Task.Delay(10);
        }
        Assert.Equal(HttpStatusCode.OK, (await server.Client.GetAsync("/health")).StatusCode);
        Assert.Equal(1, engine.Calls);
    }

    [Fact]
    public async Task EngineErrorsNeverExposeSecretsOrExceptionDetails()
    {
        const string privateDetail = "private-error-fixture-key-and-path";
        var engine = new FixtureEngine { OnForward = _ => throw new InvalidOperationException(privateDetail) };
        await using var server = await ServerFixture.Start(state: engine.CreateState());
        server.Authenticate();
        using var response = await server.Client.PostAsJsonAsync("/v1/completions", new { prompt = "hello", max_tokens = 2 });
        Assert.Equal(HttpStatusCode.InternalServerError, response.StatusCode);
        string text = await response.Content.ReadAsStringAsync();
        Assert.Contains("request_failed", text);
        Assert.DoesNotContain(privateDetail, text);
        Assert.DoesNotContain("InvalidOperationException", text);
        Assert.Equal("nosniff", response.Headers.GetValues("X-Content-Type-Options").Single());
        Assert.Equal("DENY", response.Headers.GetValues("X-Frame-Options").Single());
        Assert.False(response.Headers.Contains("Server"));
    }

    [Fact]
    public async Task FailedStreamAbortsWithoutDoneSentinelOrPrivateError()
    {
        using var release = new ManualResetEventSlim();
        var entered = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var engine = new FixtureEngine { OnForward = _ =>
        {
            entered.TrySetResult(); release.Wait(TimeSpan.FromSeconds(5));
            throw new InvalidOperationException("private-stream-error-fixture");
        } };
        await using var server = await ServerFixture.Start(state: engine.CreateState());
        server.Authenticate();
        using var request = new HttpRequestMessage(HttpMethod.Post, "/v1/chat/completions")
        {
            Content = JsonContent.Create(new { messages = new[] { new { role = "user", content = "hello" } }, stream = true }),
        };
        using var response = await server.Client.SendAsync(request, HttpCompletionOption.ResponseHeadersRead);
        var received = new StringBuilder();
        using var reader = new StreamReader(await response.Content.ReadAsStreamAsync());
        try
        {
            await entered.Task.WaitAsync(TimeSpan.FromSeconds(3));
            string? first = await reader.ReadLineAsync().WaitAsync(TimeSpan.FromSeconds(3));
            Assert.Contains("assistant", first);
            received.AppendLine(first);
        }
        finally { release.Set(); }
        try
        {
            while (await reader.ReadLineAsync() is { } line) received.AppendLine(line);
        }
        catch (IOException) { }
        Assert.DoesNotContain("[DONE]", received.ToString());
        Assert.DoesNotContain("private-stream-error-fixture", received.ToString());
    }

    [Fact]
    public async Task StreamDisconnectCancelsEngineWithoutBackgroundContinuation()
    {
        using var release = new ManualResetEventSlim();
        var entered = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var disconnected = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var engine = new FixtureEngine { OnForward = _ => { entered.TrySetResult(); release.Wait(TimeSpan.FromSeconds(5)); } };
        await using var server = await ServerFixture.Start(state: engine.CreateState(), configure: app => app.Use(async (context, next) =>
        {
            using var registration = context.RequestAborted.Register(() => disconnected.TrySetResult());
            await next(context);
        }));
        server.Authenticate();
        using var request = new HttpRequestMessage(HttpMethod.Post, "/v1/chat/completions")
        {
            Content = JsonContent.Create(new { messages = new[] { new { role = "user", content = "hello" } }, stream = true, max_tokens = 100 }),
        };
        using var response = await server.Client.SendAsync(request, HttpCompletionOption.ResponseHeadersRead);
        try
        {
            await entered.Task.WaitAsync(TimeSpan.FromSeconds(3));
            using var stream = await response.Content.ReadAsStreamAsync();
            using var reader = new StreamReader(stream);
            Assert.Contains("assistant", await reader.ReadLineAsync().WaitAsync(TimeSpan.FromSeconds(3)));
            response.Dispose();
            await disconnected.Task.WaitAsync(TimeSpan.FromSeconds(3));
            Assert.Equal(HttpStatusCode.TooManyRequests, (await server.Client.GetAsync("/health")).StatusCode);
        }
        finally { release.Set(); }
        for (int i = 0; i < 50; i++)
        {
            if ((await server.Client.GetAsync("/health")).StatusCode == HttpStatusCode.OK) break;
            await Task.Delay(10);
        }
        Assert.Equal(HttpStatusCode.OK, (await server.Client.GetAsync("/health")).StatusCode);
        Assert.Equal(1, engine.Calls);
    }

    [Fact]
    public async Task ChunkedBodiesAreCappedWithoutContentLength()
    {
        await using var server = await ServerFixture.Start(ServerFixture.Policy with { MaxBodyBytes = 1024 });
        server.Authenticate();
        using var body = new StreamContent(new NonSeekableStream(Encoding.UTF8.GetBytes("{\"prompt\":\"" + new string('x', 2048) + "\"}")));
        body.Headers.ContentType = new("application/json");
        Assert.Equal(HttpStatusCode.RequestEntityTooLarge, (await server.Client.PostAsync("/v1/completions", body)).StatusCode);
    }

    [Fact]
    public async Task StalledBodyTimesOutAndReleasesAdmission()
    {
        await using var server = await ServerFixture.Start(ServerFixture.Policy with { RequestTimeout = TimeSpan.FromMilliseconds(100) });
        using var socket = new TcpClient();
        await socket.ConnectAsync("127.0.0.1", server.Client.BaseAddress!.Port);
        var stream = socket.GetStream();
        var wire = $"POST /v1/completions HTTP/1.1\r\nHost: 127.0.0.1\r\nAuthorization: Bearer {ServerFixture.InferenceKey}\r\nContent-Type: application/json\r\nTransfer-Encoding: chunked\r\n\r\n100\r\n{{";
        await stream.WriteAsync(Encoding.ASCII.GetBytes(wire));
        using var reader = new StreamReader(stream);
        string? status = await reader.ReadLineAsync().WaitAsync(TimeSpan.FromSeconds(3));
        Assert.Contains("504", status);
        server.Authenticate();
        Assert.Equal(HttpStatusCode.OK, (await server.Client.GetAsync("/health")).StatusCode);
    }

    private sealed class NonSeekableStream(byte[] data) : MemoryStream(data)
    {
        public override bool CanSeek => false;
    }
}
