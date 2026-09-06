using System.Net.Http.Headers;
using CNET.Llm.Server;
using Microsoft.AspNetCore.Builder;
using Microsoft.AspNetCore.Hosting.Server;
using Microsoft.AspNetCore.Hosting.Server.Features;
using Microsoft.Extensions.DependencyInjection;

namespace CNET.Llm.Tests.ServerSecurity;

internal sealed class ServerFixture(WebApplication app, ServerState state, HttpClient client) : IAsyncDisposable
{
    internal const string InferenceKey = "fixture-inference-key-not-secret-0001";
    internal const string AdminKey = "fixture-administration-key-not-secret-0002";
    internal static ServerSecurityOptions Policy => new()
    {
        InferenceApiKey = InferenceKey, AdminApiKey = AdminKey, TrustedLoopbackProxy = true,
    };
    internal HttpClient Client => client;
    internal ServerState State => state;

    internal static async Task<ServerFixture> Start(ServerSecurityOptions? security = null, ServerState? state = null,
        Action<WebApplication>? configure = null, string[]? args = null)
    {
        state ??= ServerStartup.CreateBareState(new ServerOptions { Model = "unused-fixture", Host = "127.0.0.1", Port = 0 });
        var app = ServerStartup.BuildApp(state, args ?? [], security: security ?? Policy);
        configure?.Invoke(app);
        await app.StartAsync();
        var address = app.Services.GetRequiredService<IServer>().Features.Get<IServerAddressesFeature>()!.Addresses.Single();
        // Disposal must really disconnect; automatic response draining would let a test's engine finish.
        var client = new HttpClient(new SocketsHttpHandler { MaxResponseDrainSize = 0 })
            { BaseAddress = new Uri(address), Timeout = TimeSpan.FromSeconds(10) };
        return new ServerFixture(app, state, client);
    }

    internal void Authenticate(string key = InferenceKey) => client.DefaultRequestHeaders.Authorization = new AuthenticationHeaderValue("Bearer", key);

    public async ValueTask DisposeAsync()
    {
        client.Dispose();
        await app.StopAsync();
        await app.DisposeAsync();
        state.Dispose();
    }
}
