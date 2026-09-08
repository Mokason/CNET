using System;
using System.Net;
using System.Net.Http;
using System.Threading;
using System.Text.Json;
using System.Threading.Tasks;
using Xunit;

namespace CnetMcpServer.Tests;

public sealed class SafeWebToolsTests
{
    [Fact]
    public void ReadCapabilityExistsBehindAnExplicitPolicyBoundary()
    {
        Assert.True(typeof(CnetTools).Assembly.GetType("CnetMcpServer.SafeWebTools") is not null,
            "SAFE_WEB_RED: bounded read-only MCP backend has not been implemented");
    }

    [Fact]
    public async Task DisabledReturnsExplicitUntrustedError()
    {
        using var tools = new SafeWebTools(false);
        var result = JsonSerializer.SerializeToElement(await tools.CallAsync("cnet_safe_wiki_search",
            JsonSerializer.SerializeToElement(new { query = "A new subject" })));
        Assert.True(result.GetProperty("isError").GetBoolean());
        Assert.Equal("disabled", result.GetProperty("structuredContent").GetProperty("status").GetString());
        Assert.False(result.GetProperty("structuredContent").GetProperty("certified").GetBoolean());
        Assert.False(result.GetProperty("structuredContent").GetProperty("trusted").GetBoolean());
    }

    [Theory]
    [InlineData("cnet_safe_wiki_search", "{}", "invalid_arguments")]
    [InlineData("cnet_safe_wiki_search", "{\"query\":\"x\",\"extra\":true}", "invalid_arguments")]
    [InlineData("cnet_safe_wiki_search", "{\"query\":\"x\",\"query\":\"y\"}", "invalid_arguments")]
    [InlineData("cnet_safe_wiki_search", "{\"query\":\"\\ud800\"}", "invalid_arguments")]
    [InlineData("cnet_safe_wiki_search", "{\"\\ud800\":\"x\"}", "invalid_arguments")]
    [InlineData("cnet_safe_web_read", "{\"url\":\"http://en.wikipedia.org/\"}", "url_refused")]
    [InlineData("shell", "{}", "tool_refused")]
    public async Task RefusesBeforeAnyNetwork(string tool, string json, string expected)
    {
        using var tools = new SafeWebTools(true);
        using var args = JsonDocument.Parse(json);
        var result = JsonSerializer.SerializeToElement(await tools.CallAsync(tool, args.RootElement));
        Assert.Equal(expected, result.GetProperty("structuredContent").GetProperty("status").GetString());
        Assert.True(result.GetProperty("isError").GetBoolean());
    }

    [Fact]
    public async Task PlainReadProducesUntrustedCitedEvidence()
    {
        using var tools = new SafeWebTools(true, ["example.org"], new ReplyHandler());
        var result = JsonSerializer.SerializeToElement(await tools.CallAsync("cnet_safe_web_read",
            JsonSerializer.SerializeToElement(new { url = "https://example.org/article" })));
        Assert.False(result.GetProperty("isError").GetBoolean());
        var evidence = result.GetProperty("structuredContent");
        Assert.Equal("ok", evidence.GetProperty("status").GetString());
        Assert.Equal("untrusted page", evidence.GetProperty("sources")[0].GetProperty("text").GetString());
    }

    private sealed class ReplyHandler : HttpMessageHandler
    {
        protected override Task<HttpResponseMessage> SendAsync(HttpRequestMessage request, CancellationToken token)
            => Task.FromResult(new HttpResponseMessage(HttpStatusCode.OK) { Content = new StringContent("untrusted page") });
    }
}
