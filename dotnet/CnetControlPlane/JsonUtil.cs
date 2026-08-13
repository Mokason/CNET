using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace CnetControlPlane;

internal static class JsonUtil
{
    public static readonly JsonSerializerOptions Compact = new()
    {
        WriteIndented = false,
    };

    public static readonly JsonSerializerOptions Indented = new()
    {
        WriteIndented = true,
    };

    public static string CompactCanonical(JsonNode node)
    {
        // Match Python json.dumps(..., sort_keys=True, separators=(",", ":"))
        return SortKeys(node).ToJsonString(new JsonSerializerOptions
        {
            WriteIndented = false,
        });
    }

    public static JsonNode SortKeys(JsonNode node)
    {
        if (node is JsonObject obj)
        {
            var sorted = new JsonObject();
            foreach (var kv in obj.OrderBy(k => k.Key, StringComparer.Ordinal))
            {
                sorted[kv.Key] = kv.Value is null ? null : SortKeys(kv.Value.DeepClone());
            }
            return sorted;
        }
        if (node is JsonArray arr)
        {
            var copy = new JsonArray();
            foreach (var item in arr)
            {
                copy.Add(item is null ? null : SortKeys(item.DeepClone()));
            }
            return copy;
        }
        return node.DeepClone();
    }

    public static Dictionary<string, object?> ToDict(JsonElement el)
    {
        if (el.ValueKind != JsonValueKind.Object)
            throw new ArgumentException("expected JSON object");
        var dict = new Dictionary<string, object?>();
        foreach (var prop in el.EnumerateObject())
            dict[prop.Name] = ToClr(prop.Value);
        return dict;
    }

    public static object? ToClr(JsonElement el) => el.ValueKind switch
    {
        JsonValueKind.Object => ToDict(el),
        JsonValueKind.Array => el.EnumerateArray().Select(ToClr).ToList(),
        JsonValueKind.String => el.GetString(),
        JsonValueKind.Number => el.TryGetInt64(out var l) ? l : el.GetDouble(),
        JsonValueKind.True => true,
        JsonValueKind.False => false,
        JsonValueKind.Null => null,
        _ => el.GetRawText(),
    };

    public static string Sha256Hex(string path)
    {
        using var stream = File.OpenRead(path);
        var hash = SHA256.HashData(stream);
        return Convert.ToHexString(hash).ToLowerInvariant();
    }

    public static string Sha256Hex(ReadOnlySpan<byte> data)
    {
        var hash = SHA256.HashData(data);
        return Convert.ToHexString(hash).ToLowerInvariant();
    }

    public static string Sha256Utf8(string text)
    {
        return Sha256Hex(Encoding.UTF8.GetBytes(text));
    }

    public static void WriteJsonAtomic(string path, object value, bool indent = true)
    {
        var dir = Path.GetDirectoryName(path);
        if (!string.IsNullOrEmpty(dir))
            Directory.CreateDirectory(dir);
        var tmp = Path.Combine(dir ?? ".", $".{Path.GetFileName(path)}.{Guid.NewGuid():N}.tmp");
        try
        {
            var json = JsonSerializer.Serialize(value, indent ? Indented : Compact);
            if (indent && !json.EndsWith('\n'))
                json += "\n";
            else if (!indent && !json.EndsWith('\n'))
                json += "\n";
            File.WriteAllText(tmp, json);
            File.Move(tmp, path, overwrite: true);
        }
        catch
        {
            try { File.Delete(tmp); } catch { /* ignore */ }
            throw;
        }
    }
}
