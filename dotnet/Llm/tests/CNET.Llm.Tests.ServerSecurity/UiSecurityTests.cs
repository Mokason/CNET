using System.Net;
using System.Net.Http.Json;
using Xunit;

namespace CNET.Llm.Tests.ServerSecurity;

public sealed class UiSecurityTests
{
    [Fact]
    public async Task ExplicitUiBootstrapIsOfflineAndDoesNotGrantApiAccess()
    {
        await using var server = await ServerFixture.Start(serveUi: true);
        foreach (string path in new[] { "/", "/app.css", "/app.js" })
        {
            using var response = await server.Client.GetAsync(path);
            Assert.True(response.StatusCode == HttpStatusCode.OK, "MANAGED_UI_RED bootstrap_requires_preexisting_header");
            Assert.True(response.Headers.CacheControl?.NoStore == true);
            string content = await response.Content.ReadAsStringAsync();
            Assert.DoesNotContain("cdn.tailwindcss.com", content);
            Assert.DoesNotContain("innerHTML", content);
            Assert.DoesNotContain("localStorage", content);
            if (path == "/") Assert.DoesNotContain("<script>", content);
        }
        Assert.Equal(HttpStatusCode.Unauthorized, (await server.Client.GetAsync("/health")).StatusCode);
        Assert.Equal(HttpStatusCode.Unauthorized, (await server.Client.PostAsJsonAsync("/v1/config", new { temperature = 0.8 })).StatusCode);
    }

    [Fact]
    public async Task UiIsNotMappedByDefaultAndRejectsBodiesWhenEnabled()
    {
        await using var disabled = await ServerFixture.Start();
        Assert.Equal(HttpStatusCode.Unauthorized, (await disabled.Client.GetAsync("/")).StatusCode);
        disabled.Authenticate();
        Assert.Equal(HttpStatusCode.NotFound, (await disabled.Client.GetAsync("/")).StatusCode);
        await using var enabled = await ServerFixture.Start(serveUi: true);
        using var request = new HttpRequestMessage(HttpMethod.Get, "/app.js") { Content = JsonContent.Create(new { unwanted = true }) };
        Assert.Equal(HttpStatusCode.BadRequest, (await enabled.Client.SendAsync(request)).StatusCode);
    }

    [Fact]
    public async Task ConstantUiAssetsRemainAvailableDuringModelWorkButAreRateBounded()
    {
        using var release = new ManualResetEventSlim();
        var entered = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var engine = new FixtureEngine { OnForward = _ => { entered.TrySetResult(); release.Wait(TimeSpan.FromSeconds(5)); } };
        await using var server = await ServerFixture.Start(ServerFixture.Policy with { RequestsPerMinute = 2 }, engine.CreateState(), serveUi: true);
        server.Authenticate();
        var work = server.Client.PostAsJsonAsync("/v1/completions", new { prompt = "fixture", max_tokens = 1 });
        try
        {
            await entered.Task.WaitAsync(TimeSpan.FromSeconds(3));
            server.Client.DefaultRequestHeaders.Authorization = null;
            Assert.Equal(HttpStatusCode.OK, (await server.Client.GetAsync("/app.js")).StatusCode);
            Assert.Equal(HttpStatusCode.OK, (await server.Client.GetAsync("/app.css")).StatusCode);
            Assert.Equal(HttpStatusCode.TooManyRequests, (await server.Client.GetAsync("/")).StatusCode);
        }
        finally { release.Set(); await work; }
    }
}
