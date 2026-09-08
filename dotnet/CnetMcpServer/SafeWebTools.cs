using System.Globalization;
using System.Net;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;

namespace CnetMcpServer;

/// <summary>Explicitly enabled public read tools. Evidence is never certification.</summary>
internal sealed class SafeWebTools : IDisposable
{
    private readonly bool _enabled;
    private readonly SafeWebPolicy _policy;
    private readonly HttpClient _client;
    private readonly TimeSpan _timeout;
    private readonly SemaphoreSlim _calls = new(2, 2);
    private readonly SemaphoreSlim _wiki = new(1, 1);
    internal const int MaximumBytes = 256 * 1024;
    private static readonly UTF8Encoding StrictUtf8 = new(false, true);

    // Transport injection is internal and only used by deterministic tests, never tool arguments.
    internal SafeWebTools(bool enabled, IEnumerable<string>? hosts = null, HttpMessageHandler? handler = null,
        TimeSpan? timeout = null)
    {
        _enabled = enabled;
        _policy = new SafeWebPolicy(hosts ?? ["en.wikipedia.org"]);
        _client = new HttpClient(handler ?? _policy.CreateHandler()) { Timeout = Timeout.InfiniteTimeSpan };
        _timeout = timeout ?? TimeSpan.FromSeconds(10);
    }

    public static SafeWebTools FromEnvironment() => new(
        Environment.GetEnvironmentVariable("CNET_MCP_READ_ENABLED") == "1",
        (Environment.GetEnvironmentVariable("CNET_MCP_READ_HOSTS") ?? "en.wikipedia.org").Split(',', StringSplitOptions.TrimEntries));

    public async Task<object> CallAsync(string tool, JsonElement arguments)
    {
        var field = tool switch { "cnet_safe_wiki_search" => "query", "cnet_safe_web_read" => "url", _ => null };
        if (field is null) return Result("unknown", "tool_refused");
        if (!_enabled) return Result(tool, "disabled");
        if (!_policy.IsValid) return Result(tool, "invalid_policy");
        if (!SingleStringArgument(arguments, field, field == "query" ? 200 : 2048, out var value))
            return Result(tool, "invalid_arguments");
        bool wiki = field == "query";
        Uri uri;
        try
        {
            uri = _policy.ValidateUrl(wiki
                ? "https://en.wikipedia.org/w/api.php?action=query&format=json&formatversion=2"
                  + "&generator=search&gsrnamespace=0&gsrlimit=3&prop=extracts%7Cinfo&inprop=url"
                  + "&exintro=1&explaintext=1&exchars=1200&exlimit=3&maxlag=5&gsrsearch=" + Uri.EscapeDataString(value)
                : value);
        }
        catch (SafeWebRefusal e) { return Result(tool, e.Code); }
        if (!_calls.Wait(0)) return Result(tool, "busy");
        bool wikiHeld = false;
        try
        {
            if (wiki && !(wikiHeld = _wiki.Wait(0))) return Result(tool, "busy");
            using var deadline = new CancellationTokenSource(_timeout);
            var (body, mime) = await ReadAsync(uri, wiki, deadline.Token).ConfigureAwait(false);
            var sources = wiki ? WikiSources(body) : new[] { WebSource(uri, body, mime) };
            return sources.Length == 0 ? Result(tool, "no_results") : Result(tool, "ok", sources);
        }
        catch (SafeWebRefusal e) { return Result(tool, e.Code); }
        catch (OperationCanceledException) { return Result(tool, "timeout"); }
        catch (JsonException) { return Result(tool, "invalid_response"); }
        catch (InvalidOperationException) { return Result(tool, "invalid_response"); } // malformed escaped JSON property names
        catch (DecoderFallbackException) { return Result(tool, "invalid_encoding"); }
        catch (RegexMatchTimeoutException) { return Result(tool, "invalid_response"); }
        catch (HttpRequestException e)
        {
            for (Exception? inner = e; inner is not null; inner = inner.InnerException)
                if (inner is SafeWebRefusal refusal) return Result(tool, refusal.Code);
            return Result(tool, "network_error");
        }
        catch (IOException) { return Result(tool, "network_error"); }
        finally
        {
            if (wikiHeld) _wiki.Release();
            _calls.Release();
        }
    }

    private static bool SingleStringArgument(JsonElement args, string field, int maximum, out string value)
    {
        value = "";
        if (args.ValueKind != JsonValueKind.Object) return false;
        int count = 0;
        foreach (var item in args.EnumerateObject())
        {
            try
            {
                if (++count != 1 || !item.NameEquals(field) || item.Value.ValueKind != JsonValueKind.String) return false;
                value = item.Value.GetString() ?? "";
            }
            catch (InvalidOperationException) { return false; } // incomplete escaped UTF-16
        }
        return count == 1 && value.Length <= maximum && !string.IsNullOrWhiteSpace(value) && !value.Any(char.IsControl);
    }

    private async Task<(string Body, string Mime)> ReadAsync(Uri uri, bool wiki, CancellationToken token)
    {
        using var request = new HttpRequestMessage(HttpMethod.Get, uri)
        {
            Version = HttpVersion.Version11, VersionPolicy = HttpVersionPolicy.RequestVersionExact
        };
        request.Headers.ConnectionClose = true;
        request.Headers.UserAgent.ParseAdd("CNET-SafeRead/1.0 (+https://github.com/Mokason/CNET)");
        request.Headers.Accept.ParseAdd(wiki ? "application/json" : "text/plain, text/html;q=0.9");
        request.Headers.AcceptEncoding.ParseAdd("identity");
        using var response = await _client.SendAsync(request, HttpCompletionOption.ResponseHeadersRead, token).ConfigureAwait(false);
        if ((int)response.StatusCode is >= 300 and <= 399) throw new SafeWebRefusal("redirect_refused");
        if (!response.IsSuccessStatusCode) throw new SafeWebRefusal("http_error");
        if (response.StatusCode != HttpStatusCode.OK) throw new SafeWebRefusal("invalid_response");
        if (response.Content.Headers.ContentEncoding.Count != 0) throw new SafeWebRefusal("encoding_refused");
        var type = response.Content.Headers.ContentType;
        string mime = type?.MediaType?.ToLowerInvariant() ?? "";
        if (wiki ? mime != "application/json" : mime is not ("text/plain" or "text/html"))
            throw new SafeWebRefusal("mime_refused");
        string charset = type?.CharSet?.Trim('"').ToLowerInvariant() ?? "utf-8";
        if (charset is not ("utf-8" or "utf8" or "us-ascii")) throw new SafeWebRefusal("encoding_refused");
        var expected = response.Content.Headers.ContentLength;
        if (expected > MaximumBytes) throw new SafeWebRefusal("response_too_large");
        using var stream = await response.Content.ReadAsStreamAsync(token).ConfigureAwait(false);
        using var output = new MemoryStream();
        var buffer = new byte[8192];
        int read;
        while ((read = await stream.ReadAsync(buffer.AsMemory(), token).ConfigureAwait(false)) != 0)
        {
            if (output.Length + read > MaximumBytes) throw new SafeWebRefusal("response_too_large");
            output.Write(buffer, 0, read);
        }
        if (expected.HasValue && output.Length != expected.Value) throw new SafeWebRefusal("truncated_response");
        return (StrictUtf8.GetString(output.GetBuffer(), 0, checked((int)output.Length)), mime);
    }

    private sealed record Source(string url, string title, string text, string retrieved_at, string sha256);

    private static Source MakeSource(string url, string title, string text)
    {
        title = BoundedText(title, 128);
        text = BoundedText(text, 6000);
        if (text.Length == 0) throw new SafeWebRefusal("no_results");
        return new Source(url, title, text, DateTimeOffset.UtcNow.ToString("O", CultureInfo.InvariantCulture),
            Convert.ToHexStringLower(SHA256.HashData(Encoding.UTF8.GetBytes(text))));
    }

    private Source[] WikiSources(string body)
    {
        using var document = JsonDocument.Parse(body, new JsonDocumentOptions { MaxDepth = 16 });
        var root = document.RootElement;
        if (root.ValueKind != JsonValueKind.Object || root.TryGetProperty("error", out _))
            throw new SafeWebRefusal("upstream_error");
        if (!root.TryGetProperty("query", out var query)) return [];
        if (query.ValueKind != JsonValueKind.Object || !query.TryGetProperty("pages", out var pages)
            || pages.ValueKind != JsonValueKind.Array || pages.GetArrayLength() > 3)
            throw new SafeWebRefusal("invalid_response");
        var sources = new List<Source>();
        foreach (var page in pages.EnumerateArray())
        {
            if (page.ValueKind != JsonValueKind.Object || !page.TryGetProperty("pageid", out var id)
                || id.ValueKind != JsonValueKind.Number || !id.TryGetInt64(out long pageId) || pageId <= 0
                || !page.TryGetProperty("title", out var title) || title.ValueKind != JsonValueKind.String
                || !page.TryGetProperty("extract", out var extract) || extract.ValueKind != JsonValueKind.String)
                throw new SafeWebRefusal("invalid_response");
            string text = ResponseString(extract);
            if (string.IsNullOrWhiteSpace(text)) continue;
            string url = "https://en.wikipedia.org/?curid=" + pageId.ToString(CultureInfo.InvariantCulture);
            sources.Add(MakeSource(url, ResponseString(title), text));
        }
        return sources.ToArray();
    }

    private static string ResponseString(JsonElement element)
    {
        try { return element.GetString() ?? ""; }
        catch (InvalidOperationException) { throw new SafeWebRefusal("invalid_response"); }
    }

    private static Source WebSource(Uri uri, string body, string mime)
    {
        string title = uri.Host;
        if (mime == "text/html")
        {
            var match = Regex.Match(body, @"<title\b[^>]*>([\s\S]*?)</title\s*>",
                RegexOptions.IgnoreCase | RegexOptions.CultureInvariant | RegexOptions.NonBacktracking, TimeSpan.FromMilliseconds(100));
            if (match.Success) title = WebUtility.HtmlDecode(match.Groups[1].Value);
            foreach (var tag in new[] { "script", "style" })
                body = Regex.Replace(body, "<" + tag + @"\b[^>]*>[\s\S]*?</" + tag + @"\s*>", " ",
                    RegexOptions.IgnoreCase | RegexOptions.CultureInvariant | RegexOptions.NonBacktracking, TimeSpan.FromMilliseconds(100));
            body = Regex.Replace(body, @"<[^>]*>", " ", RegexOptions.NonBacktracking, TimeSpan.FromMilliseconds(100));
            body = WebUtility.HtmlDecode(body);
        }
        return MakeSource(uri.AbsoluteUri, title, body);
    }

    private static string BoundedText(string text, int limit)
    {
        // Plain data only; controls cannot forge terminal lines. Preserve ordinary whitespace.
        var safe = new string(text.Where(c => !char.IsControl(c) || c is '\n' or '\r' or '\t').ToArray()).Trim();
        if (safe.Length <= limit) return safe;
        if (char.IsHighSurrogate(safe[limit - 1])) limit--;
        return safe[..limit];
    }

    private static object Result(string tool, string status, Source[]? sources = null)
    {
        var evidence = new { schema = "cnet.web-evidence.v1", tool, status,
            certified = false, trusted = false, sources = sources ?? [] };
        return new { content = new[] { new { type = "text", text = JsonSerializer.Serialize(evidence) } },
            structuredContent = evidence, isError = status != "ok" };
    }

    public void Dispose() { _client.Dispose(); _calls.Dispose(); _wiki.Dispose(); }
}
