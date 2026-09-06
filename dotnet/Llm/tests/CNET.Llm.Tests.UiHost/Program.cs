using System.Net;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;
using System.Text.Json;
using CNET.Llm.Server;
using CNET.Llm.Tests.ServerSecurity;
using Microsoft.AspNetCore.Hosting.Server;
using Microsoft.AspNetCore.Hosting.Server.Features;

// Private browser-test fixture only: no model files, registry, upstream service or persistent certificate.
using var rsa = RSA.Create(2048);
var request = new CertificateRequest("CN=localhost", rsa, HashAlgorithmName.SHA256, RSASignaturePadding.Pkcs1);
var names = new SubjectAlternativeNameBuilder();
names.AddIpAddress(IPAddress.Loopback);
request.CertificateExtensions.Add(names.Build());
using var certificate = request.CreateSelfSigned(DateTimeOffset.UtcNow.AddMinutes(-5), DateTimeOffset.UtcNow.AddHours(1));
var builder = WebApplication.CreateSlimBuilder();
builder.Logging.ClearProviders();
builder.WebHost.ConfigureKestrel(k => k.Listen(IPAddress.Loopback, 0, endpoint => endpoint.UseHttps(certificate)));
await using var proxy = builder.Build();
using var forwarding = new HttpClient(new SocketsHttpHandler { AllowAutoRedirect = false });
Uri? backendAddress = null;
proxy.Run(async context =>
{
    if (backendAddress is null) { context.Response.StatusCode = 503; return; }
    using var forwarded = new HttpRequestMessage(new HttpMethod(context.Request.Method), new Uri(backendAddress, context.Request.Path + context.Request.QueryString));
    if (context.Request.ContentLength > 0 || context.Request.Headers.TransferEncoding.Count > 0)
        forwarded.Content = new StreamContent(context.Request.Body);
    foreach (var header in context.Request.Headers)
    {
        if (header.Key.Equals("Host", StringComparison.OrdinalIgnoreCase) || header.Key.Equals("Transfer-Encoding", StringComparison.OrdinalIgnoreCase)) continue;
        if (!forwarded.Headers.TryAddWithoutValidation(header.Key, header.Value.ToArray()))
            forwarded.Content?.Headers.TryAddWithoutValidation(header.Key, header.Value.ToArray());
    }
    using var response = await forwarding.SendAsync(forwarded, HttpCompletionOption.ResponseHeadersRead, context.RequestAborted);
    context.Response.StatusCode = (int)response.StatusCode;
    foreach (var header in response.Headers.Concat(response.Content.Headers))
        context.Response.Headers[header.Key] = header.Value.ToArray();
    context.Response.Headers.Remove("transfer-encoding");
    await context.Response.StartAsync(context.RequestAborted);
    await response.Content.CopyToAsync(context.Response.Body, context.RequestAborted);
});
await proxy.StartAsync();
string address = proxy.Services.GetRequiredService<IServer>().Features.Get<IServerAddressesFeature>()!.Addresses.Single();
using var state = new FixtureEngine
{
    OnForward = _ => Thread.Sleep(30),
    TokenText = "<img src=x onerror='window.fixtureXss=1'>[link](javascript:alert(1))",
}.CreateState();
var policy = new ServerSecurityOptions
{
    InferenceApiKey = "fixture-inference-key-not-secret-0001",
    AdminApiKey = "fixture-administration-key-not-secret-0002",
    TrustedLoopbackProxy = true, AllowedOrigins = [address], MaxOutputTokens = 8,
};
await using var backend = ServerStartup.BuildApp(state, [], serveUi: true, security: policy);
await backend.StartAsync();
backendAddress = new Uri(backend.Services.GetRequiredService<IServer>().Features.Get<IServerAddressesFeature>()!.Addresses.Single());
Console.WriteLine("UI_FIXTURE_READY " + JsonSerializer.Serialize(new { address }));
await Console.In.ReadLineAsync();
await backend.StopAsync();
await proxy.StopAsync();
