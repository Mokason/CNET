using System.Net;
using System.Net.Http.Json;
using System.Text;
using CNET.Llm.Server;
using Xunit;

namespace CNET.Llm.Tests.ServerSecurity;

public sealed class BoundaryTests
{
    [Fact]
    public void StartupRejectsMissingSharedWeakCredentialsAndExternalBinding()
    {
        Assert.Throws<InvalidOperationException>(() => new ServerSecurityOptions().Validate());
        Assert.Throws<InvalidOperationException>(() => (ServerFixture.Policy with { AdminApiKey = ServerFixture.InferenceKey }).Validate());
        Assert.Throws<InvalidOperationException>(() => (ServerFixture.Policy with { InferenceApiKey = "weak" }).Validate());
        Assert.Throws<InvalidOperationException>(() => (ServerFixture.Policy with { AllowedOrigins = ["*"] }).Validate());
        Assert.Throws<InvalidOperationException>(() => (ServerFixture.Policy with { AllowedOrigins = ["https://*.example"] }).Validate());
        using var state = ServerStartup.CreateBareState(new ServerOptions { Model = "unused", Host = "0.0.0.0" });
        Assert.Throws<InvalidOperationException>(() => ServerStartup.BuildApp(state, [], security: ServerFixture.Policy));
        Assert.DoesNotContain(ServerFixture.InferenceKey, ServerFixture.Policy.ToString());
        Assert.DoesNotContain(ServerFixture.AdminKey, ServerFixture.Policy.ToString());
    }

    [Fact]
    public async Task LocalhostBindingIsCaseInsensitive()
    {
        using var state = ServerStartup.CreateBareState(new ServerOptions { Model = "unused", Host = "LOCALHOST", Port = 0 });
        await using var app = ServerStartup.BuildApp(state, [], security: ServerFixture.Policy);
        await app.StartAsync();
        await app.StopAsync();
    }

    [Theory]
    [InlineData("/v1/config")]
    [InlineData("/v1/cache/clear")]
    [InlineData("/v1/models/load")]
    public async Task AnonymousRequestsCannotMutate(string path)
    {
        await using var server = await ServerFixture.Start();
        Assert.Equal(HttpStatusCode.Unauthorized, (await server.Client.PostAsJsonAsync(path, new { })).StatusCode);
        Assert.Equal(0, server.State.SamplingDefaults.Temperature);
    }

    [Theory]
    [InlineData("/v1/config", "{\"temperature\":0.8}")]
    [InlineData("/v1/cache/clear", "{}")]
    [InlineData("/v1/models/load", "{}")]
    public async Task InferenceCredentialCannotMutate(string path, string json)
    {
        await using var server = await ServerFixture.Start();
        server.Authenticate();
        using var response = await server.Client.PostAsync(path, new StringContent(json, Encoding.UTF8, "application/json"));
        Assert.Equal(HttpStatusCode.Forbidden, response.StatusCode);
        Assert.Equal(0, server.State.SamplingDefaults.Temperature);
    }

    [Theory]
    [InlineData("/props")]
    [InlineData("/v1/models/available")]
    [InlineData("/v1/models/inspect?path=unread-fixture.gguf")]
    public async Task FilesystemInspectionRequiresAdministration(string path)
    {
        await using var server = await ServerFixture.Start();
        server.Authenticate();
        Assert.Equal(HttpStatusCode.Forbidden, (await server.Client.GetAsync(path)).StatusCode);
    }

    [Fact]
    public async Task DistinctAdministratorCanChangeValidatedConfiguration()
    {
        await using var server = await ServerFixture.Start();
        server.Authenticate(ServerFixture.AdminKey);
        Assert.Equal(HttpStatusCode.OK, (await server.Client.PostAsJsonAsync("/v1/config", new { temperature = 0.8 })).StatusCode);
        Assert.Equal(0.8f, server.State.SamplingDefaults.Temperature);
    }

    [Fact]
    public async Task DevelopmentIsLoopbackOnlyAndDoesNotGrantAdministration()
    {
        await using var server = await ServerFixture.Start(new ServerSecurityOptions { LoopbackDevelopment = true });
        Assert.Equal(HttpStatusCode.OK, (await server.Client.GetAsync("/health")).StatusCode);
        Assert.Equal(HttpStatusCode.Unauthorized, (await server.Client.PostAsJsonAsync("/v1/cache/clear", new { })).StatusCode);
        server.Client.DefaultRequestHeaders.Host = "rebinding.invalid";
        Assert.Equal(HttpStatusCode.Forbidden, (await server.Client.GetAsync("/health")).StatusCode);
    }

    [Fact]
    public async Task ForwardedHeadersAndHostingOverridesDoNotGrantAuthority()
    {
        await using var server = await ServerFixture.Start(args: ["--urls", "http://0.0.0.0:0", "--Kestrel:Endpoints:Unsafe:Url", "http://0.0.0.0:0"]);
        Assert.Equal("127.0.0.1", server.Client.BaseAddress!.Host);
        server.Client.DefaultRequestHeaders.Add("X-Forwarded-For", "127.0.0.1");
        server.Client.DefaultRequestHeaders.Add("X-Forwarded-Proto", "https");
        Assert.Equal(HttpStatusCode.Unauthorized, (await server.Client.GetAsync("/health")).StatusCode);
    }

    [Fact]
    public async Task CorsAllowsOnlyConfiguredOriginAndHeaders()
    {
        await using var server = await ServerFixture.Start(ServerFixture.Policy with { AllowedOrigins = ["https://owner.example"] });
        server.Authenticate();
        server.Client.DefaultRequestHeaders.Add("Origin", "https://attacker.invalid");
        using var denied = await server.Client.GetAsync("/health");
        Assert.Equal(HttpStatusCode.Forbidden, denied.StatusCode);
        Assert.False(denied.Headers.Contains("Access-Control-Allow-Origin"));
        server.Client.DefaultRequestHeaders.Remove("Origin");
        server.Client.DefaultRequestHeaders.Add("Origin", "https://owner.example");
        using var allowed = await server.Client.GetAsync("/health");
        Assert.Equal(HttpStatusCode.OK, allowed.StatusCode);
        Assert.Equal("https://owner.example", allowed.Headers.GetValues("Access-Control-Allow-Origin").Single());
        Assert.False(allowed.Headers.Contains("Access-Control-Allow-Credentials"));
        server.Client.DefaultRequestHeaders.Authorization = null;
        using var preflight = new HttpRequestMessage(HttpMethod.Options, "/v1/completions");
        preflight.Headers.Add("Access-Control-Request-Method", "POST");
        preflight.Headers.Add("Access-Control-Request-Headers", "authorization, content-type");
        Assert.Equal(HttpStatusCode.NoContent, (await server.Client.SendAsync(preflight)).StatusCode);
    }

    [Fact]
    public async Task RateLimitsArePerRoleAndReturnRetryAfter()
    {
        await using var server = await ServerFixture.Start(ServerFixture.Policy with { RequestsPerMinute = 2 });
        server.Authenticate();
        Assert.Equal(HttpStatusCode.OK, (await server.Client.GetAsync("/health")).StatusCode);
        Assert.Equal(HttpStatusCode.OK, (await server.Client.GetAsync("/health")).StatusCode);
        using var limited = await server.Client.GetAsync("/health");
        Assert.Equal(HttpStatusCode.TooManyRequests, limited.StatusCode);
        Assert.NotNull(limited.Headers.RetryAfter);
        server.Authenticate(ServerFixture.AdminKey);
        Assert.Equal(HttpStatusCode.OK, (await server.Client.GetAsync("/health")).StatusCode);
    }

    [Theory]
    [InlineData("/v1/completions", "{\"prompt\":\"ok\",\"max_tokens\":999999}")]
    [InlineData("/v1/chat/completions", "{\"messages\":[null]}")]
    [InlineData("/v1/config", "{\"max_tokens\":999999}")]
    [InlineData("/v1/detokenize", "{\"tokens\":[-1]}")]
    [InlineData("/v1/detokenize", "{\"tokens\":[\"bad\"]}")]
    [InlineData("/V1/COMPLETIONS", "{\"prompt\":\"ok\",\"MAX_TOKENS\":999999}")]
    [InlineData("/v1/config", "{\"max_tokens\":null}")]
    [InlineData("/v1/models/load", "{\"model\":\"unused\",\"device\":123}")]
    [InlineData("/v1/chat/completions", "{\"messages\":[{\"role\":\"assistant\",\"tool_calls\":[null]}]}")]
    [InlineData("/v1/chat/completions", "{\"messages\":[{\"role\":\"user\",\"content\":\"ok\"}],\"tool_choice\":[]}")]
    public async Task InvalidResourcesRefuseBeforeEngineOrConfiguration(string path, string json)
    {
        await using var server = await ServerFixture.Start();
        server.Authenticate(ServerFixture.AdminKey);
        using var response = await server.Client.PostAsync(path, new StringContent(json, Encoding.UTF8, "application/json"));
        Assert.True(response.StatusCode == HttpStatusCode.BadRequest,
            $"MANAGED_SERVER_RED resource_boundary path={path} status={(int)response.StatusCode}");
        Assert.Equal(2048, server.State.SamplingDefaults.MaxTokens);
    }

    [Fact]
    public async Task BodyAndQueryCapsApplyWithoutAModel()
    {
        await using var server = await ServerFixture.Start(ServerFixture.Policy with { MaxBodyBytes = 1024 });
        server.Authenticate();
        Assert.Equal(HttpStatusCode.RequestEntityTooLarge,
            (await server.Client.PostAsJsonAsync("/v1/completions", new { prompt = new string('x', 2048) })).StatusCode);
        Assert.Equal(HttpStatusCode.BadRequest, (await server.Client.GetAsync("/health?q=" + new string('x', 2050))).StatusCode);
        Assert.Equal(HttpStatusCode.BadRequest,
            (await server.Client.PostAsync("/v1/completions", new StringContent("{invalid", Encoding.UTF8, "application/json"))).StatusCode);
    }
}
