using System.Net;
using System.Net.Http.Json;
using CNET.Llm.Server;
using Microsoft.AspNetCore.Hosting.Server;
using Microsoft.AspNetCore.Hosting.Server.Features;
using Microsoft.Extensions.DependencyInjection;
using Xunit;

namespace CNET.Llm.Tests.ServerSecurity;

public sealed class ServerSecurityTests
{
    [Fact]
    public async Task AnonymousConfigurationMutationIsDenied()
    {
        using var state = ServerStartup.CreateBareState(new ServerOptions { Model = "unused-fixture", Port = 0 });
        await using var app = ServerStartup.BuildApp(state, [], security: new ServerSecurityOptions
        {
            InferenceApiKey = new string('i', 32),
            AdminApiKey = new string('a', 32),
            TrustedLoopbackProxy = true,
        });
        await app.StartAsync();
        try
        {
            var address = app.Services.GetRequiredService<IServer>()
                .Features.Get<IServerAddressesFeature>()!.Addresses.Single();
            using var client = new HttpClient { BaseAddress = new Uri(address) };
            using var response = await client.PostAsJsonAsync("/v1/config", new { temperature = 0.8 });
            Assert.True(response.StatusCode == HttpStatusCode.Unauthorized,
                $"MANAGED_SERVER_RED anonymous_mutation status={(int)response.StatusCode}");
            Assert.Equal(0, state.SamplingDefaults.Temperature);
        }
        finally { await app.StopAsync(); }
    }
}
