using CNET.Llm.Engine;
using CNET.Llm.Server;

if (args.Length < 1)
{
    Console.Error.WriteLine("Usage: CNET.Llm.Sample.Server <model.gguf> [--port 8080]");
    Console.Error.WriteLine("  model.gguf  Path to a GGUF model file");
    Console.Error.WriteLine("  --port N    Port to listen on (default: 8080)");
    return 1;
}

string modelPath = args[0];
int port = 8080;
for (int i = 1; i < args.Length - 1; i++)
{
    if (args[i] == "--port" && int.TryParse(args[i + 1], out var p))
        port = p;
}

var options = new ServerOptions
{
    Model = modelPath,
    Port = port,
    Warmup = WarmupOptions.Disabled,
};

// Validate owner security before opening any model or listener.
ServerSecurityOptions security;
try
{
    security = ServerSecurityOptions.FromEnvironment();
    security.Validate();
}
catch (InvalidOperationException)
{
    Console.Error.WriteLine("Server security configuration is missing or invalid; see docs/SERVER.md.");
    return 1;
}

Console.WriteLine("Loading configured model");
var resolvedPath = ServerStartup.ResolveModelPath(options.Model, options.Quant)
    ?? modelPath;

using var state = ServerStartup.LoadModel(resolvedPath, options);
await using var app = ServerStartup.BuildApp(state, [], security: security);

var url = $"http://{options.Host}:{options.Port}";
Console.WriteLine($"Model: {state.Config!.Architecture}, {state.Config.NumLayers} layers");
Console.WriteLine($"Server listening on {url}");
Console.WriteLine("Endpoints: /v1/chat/completions, /v1/completions, /v1/models");

await app.RunAsync();
return 0;
