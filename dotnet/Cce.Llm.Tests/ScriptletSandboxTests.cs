using CNET.Cce.Llm.Tools;
using Xunit;

namespace CNET.Cce.Llm.Tests;

/// <summary>
/// T2 scriptlets: policy guard plus out-of-process OS isolation. Contract
/// examples and later invocations use the same isolated source execution.
/// </summary>
[Collection("Scriptlet isolation")]
public sealed class ScriptletSandboxTests : IDisposable
{
    private readonly string _dir;

    public ScriptletSandboxTests()
    {
        _dir = Path.Combine(Path.GetTempPath(), "cnet-scriptlet", Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(_dir);
    }

    public void Dispose()
    {
        try { Directory.Delete(_dir, recursive: true); } catch (IOException) { }
    }

    // ─────────────── the guard ───────────────

    [Theory]
    [InlineData("return input.ToUpperInvariant();")]                 // pure string op
    [InlineData("return input.Length.ToString();")]
    [InlineData("var n = input.Count(c => c == 'a'); return n.ToString();")]  // LINQ
    [InlineData("return new string(input.Reverse().ToArray());")]
    public void Guard_AllowsPureComputation(string body) =>
        Assert.Null(ScriptletGuard.Reject(body));

    [Theory]
    [InlineData("File.ReadAllText(input); return input;", "File")]
    [InlineData("var p = new System.Diagnostics.Process(); return input;", "System")]
    [InlineData("return typeof(int).ToString();", "typeof")]
    [InlineData("Environment.Exit(0); return input;", "Environment")]
    [InlineData("unsafe { return input; }", "unsafe")]
    [InlineData("while(true){} return input;", "while(true)")]
    [InlineData("var t = GetType(); return input;", "GetType")]
    [InlineData("new HttpClient(); return input;", "HttpClient")]
    public void Guard_RejectsEscapeHatches(string body, string reasonFragment)
    {
        string? reason = ScriptletGuard.Reject(body);
        Assert.NotNull(reason);
        Assert.Contains(reasonFragment, reason);
    }

    // ─────────────── compile + run ───────────────

    [Fact]
    public void Compile_PureBody_Runs()
    {
        Assert.True(ScriptletCompiler.TryCompile("return input.ToUpperInvariant();",
            out var fn, out _));
        Assert.True(ScriptletSandbox.TryRun(fn!, "hello", out string o));
        Assert.Equal("HELLO", o);
    }

    [Fact]
    public void Compile_DangerousApi_IsRefusedAtPublicBoundary()
    {
        // The public compiler cannot bypass policy. The separate isolation
        // suite deliberately bypasses that policy to test OS enforcement.
        bool ok = ScriptletCompiler.TryCompile(
            "return new System.Net.Http.HttpClient().ToString();", out _, out string err);
        Assert.False(ok);
        Assert.NotEmpty(err);
    }

    [Fact]
    public void Sandbox_ExceptionInScriptlet_Declines()
    {
        Assert.True(ScriptletCompiler.TryCompile(
            "return input.Substring(100);", out var fn, out _));   // will throw at runtime
        Assert.False(ScriptletSandbox.TryRun(fn!, "short", out _));
    }

    [Fact]
    public void Sandbox_Timeout_Declines_WithoutHanging()
    {
        // A guard-evading busy loop (not while(true)): the timeout must catch it.
        Assert.True(ScriptletCompiler.TryCompile(
            "long x = 0; for (long i = 0; i < 100000000000; i++) x += i; return x.ToString();",
            out var fn, out _));
        var sw = System.Diagnostics.Stopwatch.StartNew();
        bool ok = ScriptletSandbox.TryRun(fn!, "x", out _, timeoutMs: 150);
        sw.Stop();
        Assert.False(ok);
        // Fresh isolated compilation has its own 5s budget. The isolation suite
        // independently measures the 150ms execution deadline after READY.
        Assert.True(sw.ElapsedMilliseconds < 6000, $"took {sw.ElapsedMilliseconds}ms");
    }

    // ─────────────── the registry (generate-and-verify) ───────────────

    private static Scriptlet SL(string name, string source, params (string, string)[] ex) =>
        new(name, source, ex.Select(e => new ToolExample(e.Item1, e.Item2)).ToList());

    [Fact]
    public void Registry_CertifiesInvokesAndPersists()
    {
        string path = Path.Combine(_dir, "scriptlets.json");
        var reg = new ScriptletRegistry(path);
        var sl = SL("shout", "return input.ToUpperInvariant() + \"!\";",
            ("hi", "HI!"), ("go", "GO!"));

        Assert.True(reg.Register(sl).Certified);
        Assert.True(reg.TryInvoke("shout", "yes", out string o));
        Assert.Equal("YES!", o);

        // Durable: reload recompiles + re-certifies from stored source.
        var reloaded = new ScriptletRegistry(path);
        Assert.True(reloaded.TryInvoke("shout", "ok", out string o2));
        Assert.Equal("OK!", o2);
    }

    [Fact]
    public void Registry_RefusesScriptletThatFailsItsExamples()
    {
        var reg = new ScriptletRegistry(Path.Combine(_dir, "s.json"));
        var wrong = SL("x", "return input;", ("a", "A"), ("b", "B"));   // identity, not upper
        var v = reg.Register(wrong);
        Assert.False(v.Certified);
        Assert.False(reg.TryInvoke("x", "a", out _));
    }

    [Fact]
    public void Registry_RefusesUnsafeSource_AtTheGuard()
    {
        var reg = new ScriptletRegistry(Path.Combine(_dir, "s.json"));
        var evil = SL("evil", "return System.IO.File.ReadAllText(input);",
            ("/etc/passwd", "x"), ("a", "y"));
        var v = reg.Register(evil);
        Assert.False(v.Certified);
        Assert.StartsWith("guard:", v.FirstFailure);   // stopped before compile
    }

    [Fact]
    public void Forge_CertifiesModelWrittenScriptlet()
    {
        var reg = new ScriptletRegistry(Path.Combine(_dir, "s.json"));
        var forge = new ScriptletForge(reg);
        string output = "```json\n{\"name\":\"vowel_count\"," +
            "\"source\":\"return input.Count(c => \\\"aeiou\\\".Contains(char.ToLower(c))).ToString();\"," +
            "\"examples\":[{\"in\":\"hello\",\"out\":\"2\"},{\"in\":\"sky\",\"out\":\"0\"}]}\n```";

        var (sl, verdict) = forge.TryForge(output);
        Assert.True(verdict.Certified, verdict.FirstFailure);
        Assert.Equal("vowel_count", sl!.Name);
        Assert.True(reg.TryInvoke("vowel_count", "education", out string o));
        Assert.Equal("5", o);
    }

    [Fact]
    public void Scriptlets_ComposeWithDeclarativeTools_UnderOneNamespace()
    {
        var decl = new ToolRegistry(Path.Combine(_dir, "d.json"));
        decl.Register(new ToolSpec("area", "formula",
            new() { ["expression"] = "$w * $h" },
            [new ToolExample("w=2, h=3", "6"), new ToolExample("w=4, h=5", "20")]));
        var scr = new ScriptletRegistry(Path.Combine(_dir, "s.json"));
        scr.Register(SL("shout", "return input.ToUpperInvariant();", ("a", "A"), ("b", "B")));

        var composite = new CompositeToolProvider(decl, scr);

        Assert.True(composite.TryInvoke("area", "w=6, h=7", out string a));
        Assert.Equal("42", a);
        Assert.True(composite.TryInvoke("shout", "hi", out string b));
        Assert.Equal("HI", b);
        Assert.Equal(2, composite.List().Count);
    }

    [Fact]
    public void Guard_StripsLeadingUsingDirectives_ButKeepsDenylist()
    {
        // Model habit: leading usings. Harmless ones vanish; a dangerous type
        // used after a stripped using is still caught by the identifier denylist.
        Assert.Null(ScriptletGuard.Reject(
            "using System.Linq;\nreturn input.Split(' ').Length.ToString();"));
        Assert.NotNull(ScriptletGuard.Reject(
            "using System.IO;\nreturn File.ReadAllText(input);"));   // File still blocked
    }

    [Fact]
    public void Compile_WithLeadingUsings_Works()
    {
        Assert.True(ScriptletCompiler.TryCompile(
            "using System;\nusing System.Linq;\nreturn input.Split(' ').Length.ToString();",
            out var fn, out string err), err);
        Assert.True(ScriptletSandbox.TryRun(fn!, "one two three", out string o));
        Assert.Equal("3", o);
    }
}
