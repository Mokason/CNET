using System.Diagnostics;
using System.Net;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using CNET.Llm.Server.Models;
using CNET.Llm.Server.Endpoints;
using Microsoft.AspNetCore.Http.Features;

namespace CNET.Llm.Server;

/// <summary>Authentication, bounded admission and safe errors for every mapped route.</summary>
internal sealed class ServerSecurityMiddleware(ServerSecurityOptions options) : IMiddleware, IDisposable
{
    private readonly SemaphoreSlim _admission = new(1, 1);
    // Three fixed buckets: unauthenticated, inference, administration. Never keyed by attacker input.
    private readonly int[] _requests = new int[3];
    private long _window = Stopwatch.GetTimestamp();

    public async Task InvokeAsync(HttpContext context, RequestDelegate next)
    {
        context.Response.Headers.XContentTypeOptions = "nosniff";
        context.Response.Headers.XFrameOptions = "DENY";
        context.Response.Headers["Referrer-Policy"] = "no-referrer";
        context.Response.Headers.ContentSecurityPolicy = "default-src 'self'; object-src 'none'; frame-ancestors 'none'; base-uri 'none'";
        context.Response.Headers.CacheControl = "no-store";

        // Forwarded headers grant no transport or identity authority. Only actual sockets count.
        if (context.Connection.RemoteIpAddress is not { } remote || !IPAddress.IsLoopback(remote) ||
            context.Connection.LocalIpAddress is not { } local || !IPAddress.IsLoopback(local) ||
            (options.LoopbackDevelopment && !ServerSecurityOptions.IsLoopbackHost(context.Request.Host.Host)))
        { await Error(context, 403, "loopback_transport_required"); return; }

        if (context.Request.QueryString.Value?.Length > 2048)
        { await Error(context, 400, "query_too_long"); return; }

        var origin = context.Request.Headers.Origin;
        if (origin.Count > 0)
        {
            bool sameDevelopmentOrigin = options.LoopbackDevelopment && origin.Count == 1 &&
                origin[0] == $"{context.Request.Scheme}://{context.Request.Host}";
            if (origin.Count != 1 || !(sameDevelopmentOrigin || options.AllowedOrigins.Contains(origin[0], StringComparer.Ordinal)))
            { await Error(context, 403, "origin_denied"); return; }
            context.Response.Headers.AccessControlAllowOrigin = origin[0];
            context.Response.Headers.Vary = "Origin";
        }

        int role = Authenticate(context.Request);
        if (!TakeRatePermit(role))
        { context.Response.Headers.RetryAfter = "60"; await Error(context, 429, "rate_limited"); return; }

        if (HttpMethods.IsOptions(context.Request.Method))
        {
            var headers = context.Request.Headers.AccessControlRequestHeaders.ToString().Split(',', StringSplitOptions.TrimEntries | StringSplitOptions.RemoveEmptyEntries);
            if (origin.Count != 1 || context.Request.Headers.AccessControlRequestMethod.ToString() is not ("GET" or "POST") ||
                headers.Any(h => !h.Equals("Authorization", StringComparison.OrdinalIgnoreCase) && !h.Equals("Content-Type", StringComparison.OrdinalIgnoreCase)))
            { await Error(context, 403, "preflight_denied"); return; }
            context.Response.Headers.AccessControlAllowMethods = "GET, POST";
            context.Response.Headers.AccessControlAllowHeaders = "Authorization, Content-Type";
            context.Response.StatusCode = 204;
            return;
        }

        bool uiAsset = HttpMethods.IsGet(context.Request.Method) && context.GetEndpoint()?.Metadata.GetMetadata<PublicUiAsset>() is not null;
        if (role == 0 && !options.LoopbackDevelopment && !uiAsset)
        { context.Response.Headers.WWWAuthenticate = "Bearer"; await Error(context, 401, "authentication_required"); return; }
        if (RequiresAdministration(context.Request) && role != 2)
        { await Error(context, role == 0 ? 401 : 403, "administration_required"); return; }
        if (uiAsset && context.Features.Get<IHttpRequestBodyDetectionFeature>()?.CanHaveBody == true)
        { await Error(context, 400, "ui_asset_body_forbidden"); return; }
        if (!uiAsset && !await _admission.WaitAsync(0, context.RequestAborted))
        { context.Response.Headers.RetryAfter = "1"; await Error(context, 429, "server_busy"); return; }

        var originalToken = context.RequestAborted;
        var originalBody = context.Request.Body;
        using var deadline = CancellationTokenSource.CreateLinkedTokenSource(originalToken);
        deadline.CancelAfter(options.RequestTimeout);
        context.RequestAborted = deadline.Token;
        try
        {
            // Constant, body-free assets cannot touch model state and must load during inference.
            if (uiAsset) { await next(context); return; }
            if (context.Request.ContentLength > options.MaxBodyBytes)
            { await Error(context, 413, "body_too_large"); return; }
            using var body = new MemoryStream();
            byte[] buffer = new byte[8192];
            while (true)
            {
                int read = await originalBody.ReadAsync(buffer, deadline.Token);
                if (read == 0) break;
                if (body.Length + read > options.MaxBodyBytes)
                { await Error(context, 413, "body_too_large"); return; }
                body.Write(buffer, 0, read);
            }
            body.Position = 0;
            context.Request.Body = body;
            if (body.Length > 0)
            {
                using var document = await JsonDocument.ParseAsync(body, new JsonDocumentOptions { MaxDepth = 16 }, deadline.Token);
                if (document.RootElement.ValueKind != JsonValueKind.Object ||
                    !RequestResourceLimits.IsValid(document.RootElement, context.Request.Path.Value ?? "", options))
                { await Error(context, 400, "invalid_request"); return; }
                body.Position = 0;
            }
            await next(context);
        }
        catch (OperationCanceledException) when (deadline.IsCancellationRequested)
        {
            if (originalToken.IsCancellationRequested || context.Response.HasStarted) context.Abort();
            else await Error(context, 504, "request_timeout");
        }
        catch (BadHttpRequestException ex)
        { await Error(context, ex.StatusCode == 413 ? 413 : 400, ex.StatusCode == 413 ? "body_too_large" : "invalid_request"); }
        catch (JsonException) { await Error(context, 400, "invalid_json"); }
        catch (Exception) { await Error(context, 500, "request_failed"); }
        finally
        {
            context.Request.Body = originalBody;
            context.RequestAborted = originalToken;
            // A deadline does not release model ownership while work is still executing.
            if (!uiAsset) _admission.Release();
        }
    }

    private int Authenticate(HttpRequest request)
    {
        var authorization = request.Headers.Authorization;
        if (authorization.Count != 1 || authorization[0] is not { } value ||
            value.Length > 263 || !value.StartsWith("Bearer ", StringComparison.OrdinalIgnoreCase)) return 0;
        var presented = Encoding.UTF8.GetBytes(value[7..]);
        bool admin = Matches(options.AdminApiKey, presented);
        bool inference = Matches(options.InferenceApiKey, presented);
        return admin ? 2 : inference ? 1 : 0;
    }

    private static bool Matches(string? configured, byte[] presented) => configured is not null &&
        CryptographicOperations.FixedTimeEquals(Encoding.UTF8.GetBytes(configured), presented);

    private bool TakeRatePermit(int role)
    {
        lock (_requests)
        {
            if (Stopwatch.GetElapsedTime(_window) >= TimeSpan.FromMinutes(1))
            { Array.Clear(_requests); _window = Stopwatch.GetTimestamp(); }
            if (_requests[role] >= options.RequestsPerMinute) return false;
            _requests[role]++;
            return true;
        }
    }

    private static bool RequiresAdministration(HttpRequest request)
    {
        string path = request.Path.Value?.TrimEnd('/').ToLowerInvariant() ?? "";
        if (HttpMethods.IsGet(request.Method))
            return path is not ("" or "/app.js" or "/app.css" or "/health" or "/ready" or "/v1/models" or "/v1/config");
        return !HttpMethods.IsPost(request.Method) || path is not ("/v1/chat/completions" or "/v1/completions" or "/v1/tokenize" or "/v1/detokenize");
    }

    private static async Task Error(HttpContext context, int status, string code)
    {
        if (context.Response.HasStarted) { context.Abort(); return; }
        context.Response.StatusCode = status;
        await context.Response.WriteAsJsonAsync(new ErrorResponse { Error = code }, ServerJsonContext.Default.ErrorResponse, cancellationToken: CancellationToken.None);
    }

    public void Dispose() => _admission.Dispose();
}
