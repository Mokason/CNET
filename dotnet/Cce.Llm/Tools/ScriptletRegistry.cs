using System.Text.Json;

namespace CNET.Cce.Llm.Tools;

/// <summary>A sandboxed scriptlet: a C# transform body plus the contract
/// examples that certify it.</summary>
public sealed record Scriptlet(string Name, string Source, List<ToolExample> Examples);

/// <summary>
/// Durable set of certified scriptlet tools. A scriptlet is admitted only when
/// it (1) passes the syntax guard, (2) compiles inside the OS-isolated worker,
/// and (3) reproduces every contract example inside the execution timeout.
/// Source-bound invocation closures are cached, not host-loaded assemblies;
/// on load, every stored scriptlet is re-guarded,
/// re-compiled, and re-verified, so a scriptlet that no longer holds (a
/// changed runtime) is dropped rather than trusted on faith.
/// </summary>
public sealed class ScriptletRegistry : IToolProvider
{
    private sealed class Entry
    {
        public Scriptlet Scriptlet { get; set; } = null!;
        public int Uses { get; set; }
        public bool Retired { get; set; }
    }

    private readonly string _path;
    private readonly Dictionary<string, Entry> _stored;
    private readonly Dictionary<string, Func<string, string>> _compiled = [];

    private static readonly JsonSerializerOptions Json = new()
    {
        Encoder = System.Text.Encodings.Web.JavaScriptEncoder.UnsafeRelaxedJsonEscaping,
    };

    public ScriptletRegistry(string path)
    {
        _path = path;
        _stored = Load(path);
        // Trust but verify on load: recompile and re-certify each scriptlet.
        foreach ((string name, Entry e) in _stored.ToList())
        {
            if (e.Retired) continue;
            if (Certify(e.Scriptlet).Certified &&
                ScriptletCompiler.TryCompile(e.Scriptlet.Source, out var fn, out _))
                _compiled[name] = fn!;
            else
                e.Retired = true;   // no longer holds: stop serving it
        }
    }

    /// <summary>Guards, compiles, and certifies a scriptlet against its examples.</summary>
    public ToolVerdict Certify(Scriptlet s)
    {
        if (string.IsNullOrWhiteSpace(s.Name) || s.Examples.Count < ToolVerifier.MinExamples)
            return new ToolVerdict(false, 0, s.Examples.Count,
                $"invalid (name/>= {ToolVerifier.MinExamples} examples)");

        string? rejected = ScriptletGuard.Reject(s.Source);
        if (rejected is not null)
            return new ToolVerdict(false, 0, s.Examples.Count, "guard: " + rejected);

        if (!ScriptletCompiler.TryCompile(s.Source, out var fn, out string error))
            return new ToolVerdict(false, 0, s.Examples.Count, "compile: " + error);

        int passed = 0;
        foreach (ToolExample ex in s.Examples)
        {
            if (ScriptletSandbox.TryRun(fn!, ex.In, out string got) && got == ex.Out)
                passed++;
            else
                return new ToolVerdict(false, passed, s.Examples.Count,
                    $"example '{Trim(ex.In)}' → expected '{ex.Out}'");
        }
        return new ToolVerdict(true, passed, s.Examples.Count, null);
    }

    /// <summary>Certifies then registers. Registration happens only on success.</summary>
    public ToolVerdict Register(Scriptlet s)
    {
        ToolVerdict verdict = Certify(s);
        if (verdict.Certified &&
            ScriptletCompiler.TryCompile(s.Source, out var fn, out _))
        {
            _stored[s.Name] = new Entry { Scriptlet = s };
            _compiled[s.Name] = fn!;
            Save();
        }
        return verdict;
    }

    public bool TryInvoke(string name, string input, out string output)
    {
        output = "";
        if (!_stored.TryGetValue(name, out Entry? e) || e.Retired) return false;
        if (!_compiled.TryGetValue(name, out var fn)) return false;
        if (!ScriptletSandbox.TryRun(fn, input, out output)) return false;
        e.Uses++;
        Save();
        return true;
    }

    public bool Retire(string name)
    {
        if (!_stored.TryGetValue(name, out Entry? e) || e.Retired) return false;
        e.Retired = true;
        _compiled.Remove(name);
        Save();
        return true;
    }

    public IReadOnlyList<(string Name, string Kind, int Uses, bool Retired)> List() =>
        _stored.Values.Select(e => (e.Scriptlet.Name, "scriptlet", e.Uses, e.Retired))
                      .OrderBy(t => t.Item1).ToList();

    private static string Trim(string s) =>
        s.Replace("\n", " ")[..Math.Min(s.Length, 40)];

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
        File.WriteAllText(tmp, JsonSerializer.Serialize(_stored, Json));
        File.Move(tmp, _path, overwrite: true);
    }
}

/// <summary>
/// The scriptlet forge: the model writes a C# transform body plus the examples
/// that define it; guard → compile → verify-against-examples, register only
/// survivors. The model may write buggy code — a scriptlet that fails its own
/// examples never registers, so a registered one provably matches its contract.
/// </summary>
public sealed class ScriptletForge(ScriptletRegistry registry)
{
    public const string Protocol =
        "Write a C# method BODY for `string Transform(string input)` and its " +
        "contract examples, as one JSON object and nothing else:\n" +
        "{\"name\":\"snake_case\",\"source\":\"...C# body ending in return...\"," +
        "\"examples\":[{\"in\":\"...\",\"out\":\"...\"},{\"in\":\"...\",\"out\":\"...\"}]}\n" +
        "Available: string, Math, StringBuilder, LINQ, List<>, Regex. " +
        "FORBIDDEN: IO, network, process, reflection, threading, unsafe, using-directives, " +
        "Console — pure computation only. Give at least two examples; the tool is " +
        "registered ONLY if the compiled code reproduces every example exactly.";

    public (Scriptlet? Scriptlet, ToolVerdict Verdict) TryForge(string modelOutput)
    {
        string json = ExtractJson(modelOutput);
        if (json.Length == 0)
            return (null, new ToolVerdict(false, 0, 0, "no JSON object in output"));

        Scriptlet? s;
        try
        {
            s = JsonSerializer.Deserialize<Scriptlet>(json,
                new JsonSerializerOptions { PropertyNameCaseInsensitive = true });
        }
        catch (JsonException ex)
        {
            return (null, new ToolVerdict(false, 0, 0, "malformed: " + ex.Message));
        }
        if (s is null || s.Source is null || s.Examples is null)
            return (null, new ToolVerdict(false, 0, 0, "missing required fields"));

        return (s, registry.Register(s));
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
