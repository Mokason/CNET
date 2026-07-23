using System.Text.Json;
using System.Text.Json.Serialization;
using System.Text.RegularExpressions;

namespace CNET.Cce.Llm.Tools;

/// <summary>
/// A declarative tool: a fixed interpreter KIND plus parameters the model
/// chose. The model never supplies logic — only which of a small trusted set
/// of transforms to run and with what parameters — so a tool is sandboxed by
/// construction: there is no code to execute. Contract examples define the
/// intent; a tool is certified only once it reproduces every example.
/// </summary>
/// <param name="Name">Unique tool name (the TOOL: action key).</param>
/// <param name="Kind">regex | replace | json_field | formula.</param>
/// <param name="Params">Kind-specific parameters (pattern, path, expression…).</param>
/// <param name="Examples">Input→output pairs that define and certify behavior.</param>
public sealed record ToolSpec(
    string Name,
    string Kind,
    Dictionary<string, string> Params,
    List<ToolExample> Examples);

public sealed record ToolExample(
    [property: JsonPropertyName("in")] string In,
    [property: JsonPropertyName("out")] string Out);

/// <summary>
/// The trusted interpreter. Every kind is a total function: it returns false
/// (declines) on malformed input, never throws, never runs caller-supplied
/// code, and bounds its own work (regex timeout, output size). Adding a kind
/// means adding a reviewed transform here — the model can never introduce one.
/// </summary>
public static class DeclarativeTool
{
    public static readonly string[] Kinds = ["regex", "replace", "json_field", "formula"];

    private const int MaxOutput = 8192;
    private const int MaxPattern = 512;
    private static readonly TimeSpan RegexBudget = TimeSpan.FromMilliseconds(100);

    /// <summary>Applies a tool to one input. Never throws.</summary>
    public static bool TryApply(ToolSpec spec, string input, out string output)
    {
        output = "";
        if (input.Length > MaxOutput * 4) return false;
        try
        {
            return spec.Kind switch
            {
                "regex" => TryRegex(spec, input, out output),
                "replace" => TryReplace(spec, input, out output),
                "json_field" => TryJsonField(spec, input, out output),
                "formula" => TryFormula(spec, input, out output),
                _ => false,
            };
        }
        catch (RegexMatchTimeoutException) { return false; }   // ReDoS budget hit: decline
        catch (Exception ex) when (ex is ArgumentException or JsonException
                                       or FormatException or InvalidOperationException)
        {
            return false;
        }
    }

    private static bool TryRegex(ToolSpec spec, string input, out string output)
    {
        output = "";
        if (!spec.Params.TryGetValue("pattern", out string? pattern) ||
            pattern.Length is 0 or > MaxPattern) return false;
        int group = spec.Params.TryGetValue("group", out string? g) &&
                    int.TryParse(g, out int gi) ? gi : 1;

        var re = new Regex(pattern, RegexOptions.CultureInvariant, RegexBudget);
        Match m = re.Match(input);
        if (!m.Success || group >= m.Groups.Count || !m.Groups[group].Success) return false;
        output = m.Groups[group].Value;
        return output.Length <= MaxOutput;
    }

    private static bool TryReplace(ToolSpec spec, string input, out string output)
    {
        output = "";
        if (!spec.Params.TryGetValue("pattern", out string? pattern) ||
            pattern.Length is 0 or > MaxPattern) return false;
        string replacement = spec.Params.GetValueOrDefault("replacement", "");

        var re = new Regex(pattern, RegexOptions.CultureInvariant, RegexBudget);
        output = re.Replace(input, replacement);
        return output.Length <= MaxOutput;
    }

    private static bool TryJsonField(ToolSpec spec, string input, out string output)
    {
        output = "";
        if (!spec.Params.TryGetValue("path", out string? path) || path.Length == 0)
            return false;

        using JsonDocument doc = JsonDocument.Parse(input);
        JsonElement cur = doc.RootElement;
        foreach (string seg in path.Split('.', StringSplitOptions.RemoveEmptyEntries))
        {
            if (cur.ValueKind == JsonValueKind.Object)
            {
                if (!cur.TryGetProperty(seg, out cur)) return false;
            }
            else if (cur.ValueKind == JsonValueKind.Array &&
                     int.TryParse(seg, out int idx) && idx >= 0 && idx < cur.GetArrayLength())
            {
                cur = cur[idx];
            }
            else return false;
        }
        output = cur.ValueKind == JsonValueKind.String
            ? cur.GetString() ?? ""
            : cur.GetRawText();
        return output.Length <= MaxOutput;
    }

    private static bool TryFormula(ToolSpec spec, string input, out string output)
    {
        output = "";
        if (!spec.Params.TryGetValue("expression", out string? expr) || expr.Length == 0)
            return false;

        // Input is "name=value, name=value". Substitute $name with the value,
        // longest names first so $ab is not shadowed by $a, then hand the
        // whole thing to the exact-arithmetic engine — which validates the
        // result with its own character whitelist and declines anything odd.
        var vars = new Dictionary<string, string>(StringComparer.Ordinal);
        foreach (string pair in input.Split(',', StringSplitOptions.RemoveEmptyEntries))
        {
            string[] kv = pair.Split('=', 2);
            if (kv.Length == 2) vars[kv[0].Trim()] = kv[1].Trim();
        }
        string filled = expr;
        foreach (string name in vars.Keys.OrderByDescending(k => k.Length))
            filled = filled.Replace("$" + name, vars[name], StringComparison.Ordinal);
        if (filled.Contains('$')) return false;                // an unbound variable

        if (!Verify.ExactArithmetic.TryAnswer(filled, out string answer)) return false;
        output = answer;
        return true;
    }
}

/// <summary>Result of certifying a tool spec against its examples.</summary>
/// <param name="Certified">True iff every example reproduced exactly.</param>
/// <param name="Passed">Examples that passed.</param>
/// <param name="Total">Examples checked.</param>
/// <param name="FirstFailure">The first example that failed, for diagnostics.</param>
public sealed record ToolVerdict(bool Certified, int Passed, int Total, string? FirstFailure);

/// <summary>
/// Certifies a declarative tool: valid kind, well-formed parameters, and —
/// the warrant — every contract example reproduced exactly. Minimum two
/// examples, mirroring the evidence floor everywhere else: one example does
/// not pin behavior.
/// </summary>
public static class ToolVerifier
{
    public const int MinExamples = 2;

    public static ToolVerdict Verify(ToolSpec spec)
    {
        if (string.IsNullOrWhiteSpace(spec.Name) ||
            !DeclarativeTool.Kinds.Contains(spec.Kind) ||
            spec.Examples.Count < MinExamples)
            return new ToolVerdict(false, 0, spec.Examples.Count,
                $"invalid spec (name/kind/>= {MinExamples} examples)");

        int passed = 0;
        foreach (ToolExample ex in spec.Examples)
        {
            if (DeclarativeTool.TryApply(spec, ex.In, out string got) && got == ex.Out)
                passed++;
            else
                return new ToolVerdict(false, passed, spec.Examples.Count,
                    $"input {ex.In.Replace("\n", " ")[..Math.Min(ex.In.Length, 40)]} " +
                    $"expected '{ex.Out}'");
        }
        return new ToolVerdict(true, passed, spec.Examples.Count, null);
    }
}

/// <summary>Durable set of certified tools, with usage accounting.</summary>
public sealed class ToolRegistry
{
    private sealed class Entry
    {
        public ToolSpec Spec { get; set; } = null!;
        public int Uses { get; set; }
        public bool Retired { get; set; }
    }

    private readonly string _path;
    private readonly Dictionary<string, Entry> _tools;

    private static readonly JsonSerializerOptions Json = new()
    {
        WriteIndented = false,
        Encoder = System.Text.Encodings.Web.JavaScriptEncoder.UnsafeRelaxedJsonEscaping,
    };

    public ToolRegistry(string path)
    {
        _path = path;
        _tools = Load(path);
    }

    /// <summary>
    /// Certifies then registers a tool. Returns the verdict; registration
    /// happens only when certified. Re-registering a name replaces it only
    /// with another certified spec (never silently downgrades to junk).
    /// </summary>
    public ToolVerdict Register(ToolSpec spec)
    {
        ToolVerdict verdict = ToolVerifier.Verify(spec);
        if (verdict.Certified)
        {
            _tools[spec.Name] = new Entry { Spec = spec };
            Save();
        }
        return verdict;
    }

    /// <summary>Runs a certified, non-retired tool. Records the use. Declines otherwise.</summary>
    public bool TryInvoke(string name, string input, out string output)
    {
        output = "";
        if (!_tools.TryGetValue(name, out Entry? e) || e.Retired) return false;
        if (!DeclarativeTool.TryApply(e.Spec, input, out output)) return false;
        e.Uses++;
        Save();
        return true;
    }

    /// <summary>Retires a tool from serving; its record stays for provenance.</summary>
    public bool Retire(string name)
    {
        if (!_tools.TryGetValue(name, out Entry? e) || e.Retired) return false;
        e.Retired = true;
        Save();
        return true;
    }

    public IReadOnlyList<(string Name, string Kind, int Uses, bool Retired)> List() =>
        _tools.Values.Select(e => (e.Spec.Name, e.Spec.Kind, e.Uses, e.Retired))
                     .OrderBy(t => t.Name).ToList();

    public ToolSpec? Get(string name) =>
        _tools.TryGetValue(name, out Entry? e) && !e.Retired ? e.Spec : null;

    private static Dictionary<string, Entry> Load(string path)
    {
        if (!File.Exists(path)) return [];
        try
        {
            return JsonSerializer.Deserialize<Dictionary<string, Entry>>(
                File.ReadAllText(path), Json) ?? [];
        }
        catch (JsonException) { return []; }
    }

    private void Save()
    {
        string? dir = System.IO.Path.GetDirectoryName(System.IO.Path.GetFullPath(_path));
        if (!string.IsNullOrEmpty(dir)) Directory.CreateDirectory(dir);
        string tmp = _path + ".tmp";
        File.WriteAllText(tmp, JsonSerializer.Serialize(_tools, Json));
        File.Move(tmp, _path, overwrite: true);
    }
}

/// <summary>
/// The forge: a model proposes a tool as declarative JSON (spec + examples),
/// the verifier certifies it against its own examples, and only survivors
/// register. Generate-and-verify for capabilities — the model may be wrong
/// about its regex, but a wrong tool never certifies, so a registered tool
/// provably does what its examples say.
/// </summary>
public sealed class ToolForge(ToolRegistry registry)
{
    /// <summary>The protocol handed to the model.</summary>
    public const string Protocol =
        "Output ONLY one JSON object defining a tool — no prose, no explanation:\n" +
        "{\"name\":\"snake_case\",\"kind\":\"regex|replace|json_field|formula\"," +
        "\"params\":{...},\"examples\":[{\"in\":\"...\",\"out\":\"...\"}]}\n" +
        "kinds: regex{pattern,group} extracts a capture group (group \"0\" = whole match); " +
        "replace{pattern,replacement}; json_field{path} reads a dotted path from JSON input; " +
        "formula{expression} evaluates arithmetic over $variables given input 'a=1, b=2'. " +
        "Give at least two examples that DEFINE the behavior — the tool is registered " +
        "ONLY if it reproduces every example exactly, so make them correct.";

    /// <summary>Extracts a spec from model output, verifies, and registers on success.</summary>
    public (ToolSpec? Spec, ToolVerdict Verdict) TryForge(string modelOutput)
    {
        string json = ExtractJson(modelOutput);
        if (json.Length == 0)
            return (null, new ToolVerdict(false, 0, 0, "no JSON object in output"));

        ToolSpec? spec;
        try
        {
            spec = JsonSerializer.Deserialize<ToolSpec>(json,
                new JsonSerializerOptions { PropertyNameCaseInsensitive = true });
        }
        catch (JsonException ex)
        {
            return (null, new ToolVerdict(false, 0, 0, "malformed spec: " + ex.Message));
        }
        if (spec is null || spec.Params is null || spec.Examples is null)
            return (null, new ToolVerdict(false, 0, 0, "spec missing required fields"));

        return (spec, registry.Register(spec));
    }

    private static string ExtractJson(string text)
    {
        int fence = text.IndexOf("```json", StringComparison.Ordinal);
        if (fence >= 0)
        {
            int start = fence + "```json".Length;
            int end = text.IndexOf("```", start, StringComparison.Ordinal);
            if (end > start) text = text[start..end];
        }
        int open = text.IndexOf('{');
        int close = text.LastIndexOf('}');
        return open >= 0 && close > open ? text[open..(close + 1)] : "";
    }
}
