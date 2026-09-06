using System.Text.Json;

namespace CNET.Llm.Server;

/// <summary>Reject expensive or malformed input before DTO binding and engine access.</summary>
internal static class RequestResourceLimits
{
    internal static bool IsValid(JsonElement root, string path, ServerSecurityOptions limits)
    {
        if (HasDuplicateKeys(root) || root.EnumerateObject().Any(p => p.Name != p.Name.ToLowerInvariant())) return false;
        path = path.TrimEnd('/').ToLowerInvariant();
        if (path is "/v1/chat/completions" or "/v1/completions" or "/v1/config")
        {
            if (!Integer(root, "max_tokens", 1, limits.MaxOutputTokens) || !Integer(root, "n", 1, 1) ||
                !Integer(root, "top_k", 0, 512) || !Integer(root, "top_logprobs", 0, 20) ||
                !Number(root, "temperature", 0, 2) || !Number(root, "top_p", 0, 1) ||
                !Number(root, "min_p", 0, 1) || !Number(root, "repetition_penalty", 0.1, 4) ||
                !Number(root, "frequency_penalty", -2, 2) || !Number(root, "presence_penalty", -2, 2)) return false;
            if (root.TryGetProperty("stop", out var stop) &&
                !(ShortString(stop, 256) || (stop.ValueKind == JsonValueKind.Array && stop.GetArrayLength() <= 8 && stop.EnumerateArray().All(s => ShortString(s, 256))))) return false;
            // Structured constraint compilers need their own work budgets before accepting arbitrary schemas/grammars.
            if (root.TryGetProperty("response_format", out var format) && format.ValueKind != JsonValueKind.Null &&
                (format.ValueKind != JsonValueKind.Object || !format.TryGetProperty("type", out var type) ||
                 type.ValueKind != JsonValueKind.String || type.GetString() is not ("text" or "json_object"))) return false;
        }
        if (path == "/v1/completions")
            return root.TryGetProperty("prompt", out var prompt) && ShortString(prompt, limits.MaxPromptCharacters);
        if (path == "/v1/chat/completions")
        {
            if (!root.TryGetProperty("messages", out var messages) || messages.ValueKind != JsonValueKind.Array ||
                messages.GetArrayLength() is < 1 or > 64) return false;
            int total = 0;
            foreach (var message in messages.EnumerateArray())
            {
                if (message.ValueKind != JsonValueKind.Object || !message.TryGetProperty("role", out var role) ||
                    role.ValueKind != JsonValueKind.String || role.GetString() is not ("system" or "user" or "assistant" or "tool")) return false;
                if (message.TryGetProperty("content", out var content) && content.ValueKind != JsonValueKind.Null)
                {
                    if (content.ValueKind != JsonValueKind.String) return false;
                    total += content.GetString()!.Length;
                    if (total > limits.MaxPromptCharacters) return false;
                }
                if (message.TryGetProperty("tool_calls", out var calls) && calls.ValueKind != JsonValueKind.Null)
                {
                    if (calls.ValueKind != JsonValueKind.Array || calls.GetArrayLength() > 16) return false;
                    foreach (var call in calls.EnumerateArray())
                        if (!Function(call) || !call.TryGetProperty("id", out var id) || !ShortString(id, 128) ||
                            !call.GetProperty("function").TryGetProperty("arguments", out var arguments) ||
                            arguments.ValueKind != JsonValueKind.String || arguments.GetString()!.Length > limits.MaxPromptCharacters) return false;
                }
            }
            if (root.TryGetProperty("tools", out var tools) && tools.ValueKind != JsonValueKind.Null)
            {
                if (tools.ValueKind != JsonValueKind.Array || tools.GetArrayLength() > 16) return false;
                foreach (var tool in tools.EnumerateArray())
                    if (!Function(tool)) return false;
            }
            if (root.TryGetProperty("tool_choice", out var choice) && choice.ValueKind != JsonValueKind.Null &&
                !(choice.ValueKind == JsonValueKind.String && choice.GetString() is "auto" or "none" or "required") && !Function(choice)) return false;
        }
        if (path == "/v1/tokenize")
            return root.TryGetProperty("text", out var text) && text.ValueKind == JsonValueKind.String && text.GetString()!.Length <= limits.MaxPromptCharacters;
        if (path == "/v1/detokenize")
            return root.TryGetProperty("tokens", out var tokens) && tokens.ValueKind == JsonValueKind.Array &&
                tokens.GetArrayLength() <= 8192 && tokens.EnumerateArray().All(t => t.ValueKind == JsonValueKind.Number && t.TryGetInt32(out int id) && id >= 0);
        if (path == "/v1/models/load")
            return root.TryGetProperty("model", out var model) && ShortString(model, 512) &&
                Integer(root, "threads", 0, 64) && Integer(root, "decode_threads", 0, 64) &&
                Integer(root, "speculative_k", 1, 8) && Integer(root, "gpu_layers", 0, 0) &&
                (!root.TryGetProperty("device", out var device) || device.ValueKind == JsonValueKind.Null ||
                 (device.ValueKind == JsonValueKind.String && device.GetString() == "cpu"));
        if (path == "/v1/config")
            return root.EnumerateObject().All(p => p.Name == "seed"
                ? p.Value.ValueKind == JsonValueKind.Null || (p.Value.ValueKind == JsonValueKind.Number && p.Value.TryGetInt32(out _))
                : p.Name is "temperature" or "top_p" or "top_k" or "min_p" or "repetition_penalty" or "max_tokens" && p.Value.ValueKind == JsonValueKind.Number);
        return true;
    }

    private static bool Integer(JsonElement root, string name, int min, int max) =>
        !root.TryGetProperty(name, out var value) || value.ValueKind == JsonValueKind.Null ||
        (value.ValueKind == JsonValueKind.Number && value.TryGetInt32(out int number) && number >= min && number <= max);

    private static bool Number(JsonElement root, string name, double min, double max) =>
        !root.TryGetProperty(name, out var value) || value.ValueKind == JsonValueKind.Null ||
        (value.ValueKind == JsonValueKind.Number && value.TryGetDouble(out double number) && double.IsFinite(number) && number >= min && number <= max);

    private static bool ShortString(JsonElement value, int max) =>
        value.ValueKind == JsonValueKind.String && value.GetString()!.Length is > 0 && value.GetString()!.Length <= max;

    private static bool Function(JsonElement value) =>
        value.ValueKind == JsonValueKind.Object && value.TryGetProperty("function", out var function) &&
        function.ValueKind == JsonValueKind.Object && function.TryGetProperty("name", out var name) && ShortString(name, 64);

    private static bool HasDuplicateKeys(JsonElement value)
    {
        if (value.ValueKind == JsonValueKind.Object)
        {
            var names = new HashSet<string>(StringComparer.Ordinal);
            foreach (var property in value.EnumerateObject())
                if (!names.Add(property.Name) || HasDuplicateKeys(property.Value)) return true;
        }
        else if (value.ValueKind == JsonValueKind.Array)
            foreach (var item in value.EnumerateArray())
                if (HasDuplicateKeys(item)) return true;
        return false;
    }
}
