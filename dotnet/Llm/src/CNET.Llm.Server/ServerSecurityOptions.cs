using System.Net;

namespace CNET.Llm.Server;

/// <summary>Owner configuration; never populated from an HTTP request.</summary>
public sealed record ServerSecurityOptions
{
    public string? InferenceApiKey { get; init; }
    public string? AdminApiKey { get; init; }
    public bool LoopbackDevelopment { get; init; }
    public bool TrustedLoopbackProxy { get; init; }
    public string[] AllowedOrigins { get; init; } = [];
    public int MaxBodyBytes { get; init; } = 65536;
    public int MaxPromptCharacters { get; init; } = 32768;
    public int MaxOutputTokens { get; init; } = 2048;
    public int RequestsPerMinute { get; init; } = 120;
    public TimeSpan RequestTimeout { get; init; } = TimeSpan.FromSeconds(30);

    // Records normally print every field, which would disclose bearer secrets.
    public override string ToString() => "ServerSecurityOptions { credentials = [redacted] }";

    public static ServerSecurityOptions FromEnvironment() => new()
    {
        InferenceApiKey = Environment.GetEnvironmentVariable("CNET_SERVER_INFERENCE_KEY"),
        AdminApiKey = Environment.GetEnvironmentVariable("CNET_SERVER_ADMIN_KEY"),
        LoopbackDevelopment = Flag("CNET_SERVER_LOOPBACK_DEVELOPMENT"),
        TrustedLoopbackProxy = Flag("CNET_SERVER_TRUSTED_LOOPBACK_PROXY"),
        AllowedOrigins = (Environment.GetEnvironmentVariable("CNET_SERVER_ALLOWED_ORIGINS") ?? "")
            .Split(',', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries),
        MaxBodyBytes = Number("CNET_SERVER_MAX_BODY_BYTES", 65536),
        MaxPromptCharacters = Number("CNET_SERVER_MAX_PROMPT_CHARACTERS", 32768),
        MaxOutputTokens = Number("CNET_SERVER_MAX_OUTPUT_TOKENS", 2048),
        RequestsPerMinute = Number("CNET_SERVER_REQUESTS_PER_MINUTE", 120),
        RequestTimeout = TimeSpan.FromSeconds(Number("CNET_SERVER_TIMEOUT_SECONDS", 30)),
    };

    public void Validate()
    {
        if (!LoopbackDevelopment && (string.IsNullOrEmpty(InferenceApiKey) || !TrustedLoopbackProxy))
            throw new InvalidOperationException("Configure an inference key and trusted loopback TLS proxy, or explicitly enable loopback development.");
        foreach (var key in new[] { InferenceApiKey, AdminApiKey })
            if (key is not null && (key.Length is < 32 or > 256 || key.Any(c => c < '!' || c > '~')))
                throw new InvalidOperationException("Configured bearer keys must contain 32 to 256 printable non-space ASCII characters.");
        if (AdminApiKey is not null && AdminApiKey == InferenceApiKey)
            throw new InvalidOperationException("Administration requires a distinct credential.");
        if (MaxBodyBytes is < 1024 or > 1048576 || MaxPromptCharacters is < 1 or > 262144 ||
            MaxOutputTokens is < 1 or > 8192 || RequestsPerMinute is < 1 or > 10000 ||
            RequestTimeout < TimeSpan.FromMilliseconds(50) || RequestTimeout > TimeSpan.FromMinutes(5))
            throw new InvalidOperationException("Server resource limits are outside supported bounds.");
        if (AllowedOrigins is null || AllowedOrigins.Length > 16)
            throw new InvalidOperationException("At most 16 explicit origins are supported.");
        foreach (var origin in AllowedOrigins)
            if (!Uri.TryCreate(origin, UriKind.Absolute, out var uri) || !string.IsNullOrEmpty(uri.UserInfo) ||
                uri.GetLeftPart(UriPartial.Authority) != origin ||
                (uri.Scheme != "https" && !(LoopbackDevelopment && uri.Scheme == "http" && IsLoopbackHost(uri.Host))))
                throw new InvalidOperationException("Origins must be explicit HTTPS origins, or loopback HTTP origins in development.");
    }

    internal static bool IsLoopbackHost(string host) =>
        host.Equals("localhost", StringComparison.OrdinalIgnoreCase) ||
        (IPAddress.TryParse(host.Trim('[', ']'), out var address) && IPAddress.IsLoopback(address));

    private static bool Flag(string name) => Environment.GetEnvironmentVariable(name) switch
    {
        null or "" or "0" or "false" => false,
        "1" or "true" => true,
        _ => throw new InvalidOperationException($"Invalid boolean setting: {name}"),
    };

    private static int Number(string name, int fallback)
    {
        var value = Environment.GetEnvironmentVariable(name);
        return value is null ? fallback : int.TryParse(value, out var number) ? number
            : throw new InvalidOperationException($"Invalid integer setting: {name}");
    }
}
