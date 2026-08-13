using System.Diagnostics;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace CnetControlPlane.Ds4;

public static class Ds4EndpointVerifier
{
    public static void AtomicJson(string path, Dictionary<string, object?> value)
    {
        var dir = Path.GetDirectoryName(path);
        if (!string.IsNullOrEmpty(dir))
            Directory.CreateDirectory(dir);
        var temp = path + $".tmp.{Environment.ProcessId}";
        var json = JsonSerializer.Serialize(value, new JsonSerializerOptions
        {
            WriteIndented = false,
        });
        // sort_keys approximation: serialize via sorted JsonObject
        var node = JsonNode.Parse(json)!;
        var sorted = JsonUtil.CompactCanonical(node);
        File.WriteAllText(temp, sorted + "\n");
        File.Move(temp, path, overwrite: true);
    }

    private static int Fail(string verification, string endpoint, string message)
    {
        var record = new Dictionary<string, object?>
        {
            ["ok"] = false,
            ["endpoint"] = endpoint,
            ["error"] = message,
            ["verified_unix_ns"] = Stopwatch.GetTimestamp() > 0
                ? DateTimeOffset.UtcNow.ToUnixTimeMilliseconds() * 1_000_000L
                : 0,
        };
        // Python uses time.time_ns(); approximate with DateTimeOffset ticks.
        record["verified_unix_ns"] = DateTimeOffset.UtcNow.UtcTicks * 100; // 100ns -> ns rough; better:
        record["verified_unix_ns"] = (DateTimeOffset.UtcNow.ToUnixTimeMilliseconds() * 1_000_000L)
            + (DateTimeOffset.UtcNow.Ticks % TimeSpan.TicksPerMillisecond) * 100;
        AtomicJson(verification, record);
        Console.Error.WriteLine($"verify_ds4_endpoint: {message}");
        return 1;
    }

    public static int RunCli(string[] args)
    {
        if (args.Length != 3)
        {
            Console.Error.WriteLine("usage: verify-ds4 ENDPOINT READY_JSON VERIFY_JSON");
            return 2;
        }
        var endpoint = args[0].TrimEnd('/');
        var readyPath = args[1];
        var verificationPath = args[2];
        var payload = JsonSerializer.Serialize(new
        {
            model = "deepseek-v4-flash",
            messages = new[] { new { role = "user", content = "Reply with exactly CNET_READY." } },
            max_tokens = 1,
            temperature = 0,
            stream = false,
        }, new JsonSerializerOptions { WriteIndented = false });

        var started = Stopwatch.GetTimestamp();
        JsonElement body;
        try
        {
            using var client = new HttpClient { Timeout = TimeSpan.FromSeconds(600) };
            using var content = new StringContent(payload, Encoding.UTF8, "application/json");
            using var response = client.PostAsync($"{endpoint}/v1/chat/completions", content)
                .GetAwaiter().GetResult();
            var text = response.Content.ReadAsStringAsync().GetAwaiter().GetResult();
            if (!response.IsSuccessStatusCode)
                return Fail(verificationPath, endpoint, $"HTTP {(int)response.StatusCode}: {text[..Math.Min(text.Length, 4096)]}");
            using var doc = JsonDocument.Parse(text);
            if (doc.RootElement.ValueKind != JsonValueKind.Object)
                return Fail(verificationPath, endpoint, "response is not a JSON object");
            body = doc.RootElement.Clone();
        }
        catch (Exception ex) when (ex is HttpRequestException or TaskCanceledException or IOException or JsonException)
        {
            return Fail(verificationPath, endpoint, ex.Message);
        }

        var elapsedNs = (long)((Stopwatch.GetTimestamp() - started) * (1_000_000_000.0 / Stopwatch.Frequency));
        if (!body.TryGetProperty("choices", out var choices)
            || choices.ValueKind != JsonValueKind.Array
            || choices.GetArrayLength() == 0)
        {
            return Fail(verificationPath, endpoint, "response has no choices");
        }
        var first = choices[0];
        if (first.ValueKind != JsonValueKind.Object
            || !first.TryGetProperty("message", out var msg)
            || msg.ValueKind != JsonValueKind.Object)
        {
            return Fail(verificationPath, endpoint, "response has no assistant message");
        }

        var usage = body.TryGetProperty("usage", out var u) && u.ValueKind == JsonValueKind.Object
            ? u : default;
        var verifiedNs = DateTimeOffset.UtcNow.ToUnixTimeMilliseconds() * 1_000_000L;
        var record = new Dictionary<string, object?>
        {
            ["ok"] = true,
            ["endpoint"] = endpoint,
            ["elapsed_ns"] = elapsedNs,
            ["response_id"] = body.TryGetProperty("id", out var id) ? id.GetString() ?? "" : "",
            ["finish_reason"] = first.TryGetProperty("finish_reason", out var fr) ? fr.GetString() ?? "" : "",
            ["completion_tokens"] = usage.ValueKind == JsonValueKind.Object
                && usage.TryGetProperty("completion_tokens", out var ct)
                ? JsonUtil.ToClr(ct) : null,
            ["verified_unix_ns"] = verifiedNs,
        };
        AtomicJson(verificationPath, record);

        var readiness = new Dictionary<string, object?>();
        if (File.Exists(readyPath))
        {
            try
            {
                using var readyDoc = JsonDocument.Parse(File.ReadAllText(readyPath));
                if (readyDoc.RootElement.ValueKind == JsonValueKind.Object)
                    readiness = JsonUtil.ToDict(readyDoc.RootElement)!;
            }
            catch
            {
                readiness = new Dictionary<string, object?>();
            }
        }
        readiness["ready"] = true;
        readiness["api_ready"] = true;
        readiness["inference_verified"] = true;
        readiness["inference_elapsed_ns"] = elapsedNs;
        readiness["verified_unix_ns"] = verifiedNs;
        AtomicJson(readyPath, readiness);
        Console.WriteLine(JsonUtil.CompactCanonical(JsonSerializer.SerializeToNode(record)!));
        return 0;
    }
}
