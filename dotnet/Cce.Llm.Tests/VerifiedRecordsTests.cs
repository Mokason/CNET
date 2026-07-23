using CNET.Cce.CnetHarness;
using CNET.Cce.Llm.Verify;
using Xunit;

namespace CNET.Cce.Llm.Tests;

/// <summary>
/// Generate-and-verify distillation: only verifier survivors become records,
/// zero survivors is reported honestly, and the JSON verifier itself refuses
/// every live failure shape we met (unclosed fences, broken syntax, prose).
/// </summary>
public sealed class VerifiedRecordsTests : IDisposable
{
    private readonly string _dir;

    public VerifiedRecordsTests()
    {
        _dir = Path.Combine(Path.GetTempPath(), "cnet-vrf", Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(_dir);
    }

    public void Dispose()
    {
        try { Directory.Delete(_dir, recursive: true); } catch (IOException) { }
    }

    private sealed class ScriptedSession(params string[] outputs) : ICnetInferenceSession
    {
        private readonly Queue<string> _script = new(outputs);
        public List<CnetHarnessGenerateOptions> Calls { get; } = new();

        public CnetHarnessGenerationResult Generate(CnetHarnessGenerateOptions options)
        {
            Calls.Add(options);
            string text = _script.Count > 0 ? _script.Dequeue() : "";
            return new CnetHarnessGenerationResult(text, 10, 5, 1, 1, 0, 0,
                CnetHarnessSamplingMode.Balanced, false, 0, 1, 0, 0);
        }

        public CnetHarnessRouteInfo ProbeRoute(string role,
            CnetHarnessSamplingMode overrideMode = CnetHarnessSamplingMode.Auto) =>
            throw new NotSupportedException();

        public void Dispose() { }
    }

    // ─────────────── the JSON verifier ───────────────

    [Theory]
    [InlineData("""{"a": 1}""")]
    [InlineData("prose before ```json\n{\"a\": [1, 2]}\n``` prose after")]
    [InlineData("  [1, 2, 3]  ")]
    public void JsonVerifier_AcceptsWellFormed(string candidate)
    {
        Assert.True(new JsonVerifier().TryVerify(candidate, out string verified));
        Assert.NotEmpty(verified);
    }

    [Theory]
    [InlineData("""{"type": "choice": "branch"}""")]           // the live minimax typo
    [InlineData("""{"description":"description": "x"}""")]     // the other live typo
    [InlineData("```json\n{\"a\": 1}")]                        // unclosed fence
    [InlineData("here is your quest, brave adventurer")]        // prose
    [InlineData("""{"a": }""")]
    [InlineData("")]
    public void JsonVerifier_RefusesEveryLiveFailureShape(string candidate) =>
        Assert.False(new JsonVerifier().TryVerify(candidate, out _));

    [Fact]
    public void JsonVerifier_Canonicalizes()
    {
        Assert.True(new JsonVerifier().TryVerify("""{"a":1,"b":[2]}""", out string verified));
        using var doc = System.Text.Json.JsonDocument.Parse(verified);   // round-trips
        Assert.Contains("\n", verified);                                  // one normal form: indented
    }

    // ─────────────── distillation ───────────────

    [Fact]
    public void Distill_KeepsOnlyTheSurvivor_AndWritesItsRecord()
    {
        var session = new ScriptedSession(
            "here you go: {broken",                       // refused
            """{"quest": "valid"}""",                     // survives
            "never sampled — distillation stops at the survivor");
        var calls = new List<(string Skill, string Text)>();
        var distiller = new VerifiedRecords(session, new JsonVerifier())
        {
            NoteSkillOverride = (_, skill, text) => { calls.Add((skill, text)); return 0; },
        };

        var receipt = distiller.Distill("make me a quest json", "/t.inbox", _dir, samples: 4);

        Assert.Equal(2, receipt.Candidates);              // stopped at the survivor
        Assert.Equal(1, receipt.Verified);
        Assert.Matches("^vrf_[0-9a-f]{8}$", receipt.SkillName!);
        string record = File.ReadAllText(receipt.RecordPath!);
        Assert.StartsWith("#v2\n", record);
        Assert.Contains("\"quest\": \"valid\"", record);
        Assert.DoesNotContain("broken", record);          // junk never reaches training
        Assert.Equal(record, Assert.Single(calls).Text);

        // Seeds varied per draw — a proposal distribution needs variety.
        Assert.Equal(2, session.Calls.Count);
        Assert.NotEqual(session.Calls[0].Seed, session.Calls[1].Seed);
    }

    [Fact]
    public void Distill_NothingVerifies_ReportsHonestly_WritesNothing()
    {
        var session = new ScriptedSession("junk", "more junk", "{still broken");
        var distiller = new VerifiedRecords(session, new JsonVerifier())
        {
            NoteSkillOverride = (_, _, _) => 0,
        };

        var receipt = distiller.Distill("quest please", "/t.inbox", _dir, samples: 3);

        Assert.Equal(3, receipt.Candidates);
        Assert.Equal(0, receipt.Verified);
        Assert.Null(receipt.SkillName);
        Assert.Empty(Directory.GetFiles(_dir));
    }

    [Fact]
    public void Distill_SameVerifiedContent_MintsTheSameSkillName()
    {
        var distillerA = new VerifiedRecords(
            new ScriptedSession("""{"a": 1}"""), new JsonVerifier())
        { NoteSkillOverride = (_, _, _) => 0 };
        var distillerB = new VerifiedRecords(
            new ScriptedSession("""{ "a" : 1 }"""), new JsonVerifier())   // same canonical form
        { NoteSkillOverride = (_, _, _) => 0 };

        var a = distillerA.Distill("p", "/t.inbox", _dir, samples: 1);
        var b = distillerB.Distill("p", "/t.inbox", _dir, samples: 1);

        Assert.Equal(a.SkillName, b.SkillName);           // canonicalization -> coalescing
    }
}
