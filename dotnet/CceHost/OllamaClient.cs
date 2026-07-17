using System;
using System.Net.Http;
using System.Text;
using System.Text.Json;
using System.Threading.Tasks;

namespace CNET.CceHost;

/// <summary>Minimal client for a local OpenAI-compatible chat endpoint (ollama).</summary>
public sealed class OllamaClient : IChatClient
{
    private readonly HttpClient _http = new() { Timeout = TimeSpan.FromSeconds(120) };
    private readonly string _url;
    private readonly string _model;

    public OllamaClient(string model, string baseUrl = "http://localhost:11434")
    {
        _model = model;
        _url = baseUrl.TrimEnd('/') + "/v1/chat/completions";
    }

    /// <summary>Send messages, return the assistant's text. Low temperature for tool decisions.</summary>
    public async Task<string> ChatAsync((string Role, string Content)[] messages)
    {
        var payload = new
        {
            model = _model,
            stream = false,
            temperature = 0.2,
            messages = Array.ConvertAll(messages, m => new { role = m.Role, content = m.Content })
        };
        var req = new StringContent(JsonSerializer.Serialize(payload), Encoding.UTF8, "application/json");
        var resp = await _http.PostAsync(_url, req);
        resp.EnsureSuccessStatusCode();
        using var doc = JsonDocument.Parse(await resp.Content.ReadAsStringAsync());
        return doc.RootElement.GetProperty("choices")[0].GetProperty("message").GetProperty("content").GetString() ?? "";
    }
}
