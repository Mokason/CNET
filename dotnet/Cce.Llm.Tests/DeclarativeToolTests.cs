using CNET.Cce.CnetHarness;
using CNET.Cce.Llm.Memory;
using CNET.Cce.Llm.Tools;
using Xunit;

namespace CNET.Cce.Llm.Tests;

/// <summary>
/// T1 declarative tools: the trusted interpreter is total and sandboxed by
/// construction, verification certifies against contract examples, the
/// registry is durable, the forge turns model output into certified tools,
/// and the TOOL: action runs them mid-deliberation.
/// </summary>
public sealed class DeclarativeToolTests : IDisposable
{
    private readonly string _dir;

    public DeclarativeToolTests()
    {
        _dir = Path.Combine(Path.GetTempPath(), "cnet-tools", Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(_dir);
    }

    public void Dispose()
    {
        try { Directory.Delete(_dir, recursive: true); } catch (IOException) { }
    }

    private static ToolSpec Spec(string name, string kind,
        Dictionary<string, string> pars, params (string In, string Out)[] examples) =>
        new(name, kind, pars, examples.Select(e => new ToolExample(e.In, e.Out)).ToList());

    // ─────────────── the interpreter ───────────────

    [Fact]
    public void Regex_ExtractsCaptureGroup()
    {
        var t = Spec("port", "regex", new() { ["pattern"] = @"port (\d+)", ["group"] = "1" },
            ("connect to port 8080 now", "8080"));
        Assert.True(DeclarativeTool.TryApply(t, "the port 9119 is open", out string o));
        Assert.Equal("9119", o);
        Assert.False(DeclarativeTool.TryApply(t, "no digits here", out _));   // declines
    }

    [Fact]
    public void JsonField_NavigatesObjectsAndArrays()
    {
        var t = Spec("ruler", "json_field", new() { ["path"] = "quest.stages.0.name" });
        Assert.True(DeclarativeTool.TryApply(t,
            """{"quest":{"stages":[{"name":"Accepted"},{"name":"Done"}]}}""", out string o));
        Assert.Equal("Accepted", o);
        Assert.False(DeclarativeTool.TryApply(t, """{"quest":{}}""", out _));   // missing path
    }

    [Fact]
    public void Formula_EvaluatesOverVariables_Exactly()
    {
        var t = Spec("area", "formula", new() { ["expression"] = "$width * $height" });
        Assert.True(DeclarativeTool.TryApply(t, "width=12, height=8", out string o));
        Assert.Equal("96", o);
        Assert.False(DeclarativeTool.TryApply(t, "width=12", out _));   // unbound $height
    }

    [Fact]
    public void Formula_LongestVariableNameWins()
    {
        var t = Spec("f", "formula", new() { ["expression"] = "$ab + $a" });
        Assert.True(DeclarativeTool.TryApply(t, "a=2, ab=10", out string o));
        Assert.Equal("12", o);   // $ab -> 10 (not "2b"), $a -> 2
    }

    [Fact]
    public void Regex_CatastrophicPattern_DeclinesWithinBudget()
    {
        var t = Spec("evil", "regex", new() { ["pattern"] = "(a+)+$", ["group"] = "0" });
        // A classic ReDoS trigger: must decline via the match timeout, not hang.
        string evil = new string('a', 40) + "!";
        var sw = System.Diagnostics.Stopwatch.StartNew();
        bool ok = DeclarativeTool.TryApply(t, evil, out _);
        sw.Stop();
        Assert.False(ok);
        Assert.True(sw.ElapsedMilliseconds < 2000, $"took {sw.ElapsedMilliseconds}ms");
    }

    [Fact]
    public void UnknownKind_NeverApplies()
    {
        var t = Spec("x", "shell", new() { ["cmd"] = "rm -rf /" }, ("a", "b"));
        Assert.False(DeclarativeTool.TryApply(t, "anything", out _));
    }

    // ─────────────── verification ───────────────

    [Fact]
    public void Verify_CertifiesOnlyWhenAllExamplesReproduce()
    {
        var good = Spec("port", "regex", new() { ["pattern"] = @"port (\d+)" },
            ("port 80", "80"), ("the port 443 here", "443"));
        Assert.True(ToolVerifier.Verify(good).Certified);

        var wrong = Spec("port", "regex", new() { ["pattern"] = @"port (\d+)" },
            ("port 80", "80"), ("port 443", "999"));         // second example lies
        var v = ToolVerifier.Verify(wrong);
        Assert.False(v.Certified);
        Assert.Equal(1, v.Passed);
    }

    [Fact]
    public void Verify_RequiresTwoExamples()
    {
        var thin = Spec("port", "regex", new() { ["pattern"] = @"(\d+)" }, ("a1", "1"));
        Assert.False(ToolVerifier.Verify(thin).Certified);
    }

    // ─────────────── registry ───────────────

    [Fact]
    public void Registry_RegistersInvokesAndPersists()
    {
        string path = Path.Combine(_dir, "tools.json");
        var reg = new ToolRegistry(path);
        var spec = Spec("area", "formula", new() { ["expression"] = "$w * $h" },
            ("w=2, h=3", "6"), ("w=4, h=5", "20"));

        Assert.True(reg.Register(spec).Certified);
        Assert.True(reg.TryInvoke("area", "w=7, h=6", out string o));
        Assert.Equal("42", o);

        var reloaded = new ToolRegistry(path);            // durable
        Assert.True(reloaded.TryInvoke("area", "w=10, h=10", out string o2));
        Assert.Equal("100", o2);
    }

    [Fact]
    public void Registry_RefusesUncertifiedSpecs()
    {
        var reg = new ToolRegistry(Path.Combine(_dir, "t.json"));
        var bad = Spec("x", "regex", new() { ["pattern"] = "(a)" }, ("b", "z"));  // fails its example
        Assert.False(reg.Register(bad).Certified);
        Assert.False(reg.TryInvoke("x", "a", out _));      // never registered
    }

    [Fact]
    public void Registry_RetiredToolStopsServing()
    {
        var reg = new ToolRegistry(Path.Combine(_dir, "t.json"));
        reg.Register(Spec("f", "formula", new() { ["expression"] = "$a+$a" },
            ("a=1", "2"), ("a=3", "6")));
        Assert.True(reg.Retire("f"));
        Assert.False(reg.TryInvoke("f", "a=5", out _));
    }

    // ─────────────── the forge (generate-and-verify) ───────────────

    [Fact]
    public void Forge_CertifiesAModelProposedSpec()
    {
        var reg = new ToolRegistry(Path.Combine(_dir, "t.json"));
        var forge = new ToolForge(reg);
        string modelOutput = "Here is a tool:\n```json\n" +
            "{\"name\":\"double\",\"kind\":\"formula\",\"params\":{\"expression\":\"$x * 2\"}," +
            "\"examples\":[{\"in\":\"x=5\",\"out\":\"10\"},{\"in\":\"x=21\",\"out\":\"42\"}]}" +
            "\n```\nHope that helps!";

        var (spec, verdict) = forge.TryForge(modelOutput);
        Assert.True(verdict.Certified);
        Assert.Equal("double", spec!.Name);
        Assert.True(reg.TryInvoke("double", "x=50", out string o));
        Assert.Equal("100", o);
    }

    [Fact]
    public void Forge_RejectsAWrongSpec_NothingRegistered()
    {
        var reg = new ToolRegistry(Path.Combine(_dir, "t.json"));
        var forge = new ToolForge(reg);
        // The regex does not actually produce the claimed output.
        string bad = "{\"name\":\"x\",\"kind\":\"regex\",\"params\":{\"pattern\":\"(\\\\d+)\"}," +
                     "\"examples\":[{\"in\":\"abc\",\"out\":\"5\"},{\"in\":\"de\",\"out\":\"6\"}]}";

        var (_, verdict) = forge.TryForge(bad);
        Assert.False(verdict.Certified);
        Assert.Empty(reg.List());
    }

    // ─────────────── the TOOL: action in the loop ───────────────

    private sealed class ScriptedSession(params string[] outputs) : ICnetInferenceSession
    {
        private readonly Queue<string> _script = new(outputs);
        public List<CnetHarnessGenerateOptions> Calls { get; } = new();

        public CnetHarnessGenerationResult Generate(CnetHarnessGenerateOptions options)
        {
            Calls.Add(options);
            string text = _script.Count > 0 ? _script.Dequeue() : "(done)";
            return new CnetHarnessGenerationResult(text, 10, 5, 1, 1, 0, 0,
                CnetHarnessSamplingMode.Deterministic, false, 0, 1, 0, 0);
        }

        public CnetHarnessRouteInfo ProbeRoute(string role,
            CnetHarnessSamplingMode overrideMode = CnetHarnessSamplingMode.Auto) =>
            throw new NotSupportedException();

        public void Dispose() { }
    }

    [Fact]
    public void ToolAction_RunsCertifiedTool_MidDeliberation()
    {
        string storePath = Path.Combine(_dir, "s.jsonl");
        using var store = BlobStore.Open(storePath);
        var reg = new ToolRegistry(Path.Combine(_dir, "t.json"));
        reg.Register(Spec("area", "formula", new() { ["expression"] = "$w * $h" },
            ("w=2, h=3", "6"), ("w=4, h=5", "20")));

        var session = new ScriptedSession(
            "TOOL: area w=13, h=4",
            "The area is 52 square units.");
        var memory = new ConversationMemory(store, t => t.Length / 4 + 1);
        var ghost = new MemorySession(session, memory, 8192, t => t.Length / 4 + 1)
        {
            Tools = reg,
        };

        var r = ghost.Generate(null, "what is the area of a 13 by 4 room?");

        var round = Assert.Single(r.Lookups);
        Assert.Equal("tool", round.Kind);
        Assert.Equal("52", round.Output);
        Assert.Contains("area(w=13, h=4) = 52", session.Calls[1].System);
        Assert.Contains("TOOL: <name>", session.Calls[0].System);   // protocol advertised
    }

    [Fact]
    public void ToolAction_UnknownTool_DeclinesHonestly()
    {
        string storePath = Path.Combine(_dir, "s2.jsonl");
        using var store = BlobStore.Open(storePath);
        var reg = new ToolRegistry(Path.Combine(_dir, "t.json"));   // empty registry
        var session = new ScriptedSession("TOOL: nonexistent foo", "I cannot compute that.");
        var memory = new ConversationMemory(store, t => t.Length / 4 + 1);
        var ghost = new MemorySession(session, memory, 8192, t => t.Length / 4 + 1)
        {
            // No registry attached at all: TOOL: is not even advertised, so
            // the "TOOL:" line is treated as the answer, not an action.
            Tools = reg,
        };

        var r = ghost.Generate(null, "run a tool");
        var round = Assert.Single(r.Lookups);
        Assert.Contains("no such certified tool", session.Calls[1].System);
        Assert.Null(round.Output);
    }
}
