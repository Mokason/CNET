using CNET.Cce.Llm.Judgment;
using CNET.Cce.Llm.Memory;
using Xunit;

namespace CNET.Cce.Llm.Tests;

/// <summary>
/// Primitive adaptive common sense: sane seed judgments, online adaptation
/// from consequence labels, durable weights, and the constitutional limits —
/// taste vetoes teaching candidates only, never user imperatives.
/// </summary>
public sealed class AdaptiveJudgeTests : IDisposable
{
    private readonly string _dir;

    public AdaptiveJudgeTests()
    {
        _dir = Path.Combine(Path.GetTempPath(), "cnet-judge", Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(_dir);
    }

    public void Dispose()
    {
        try { Directory.Delete(_dir, recursive: true); } catch (IOException) { }
    }

    private static string Degenerate() =>
        string.Concat(Enumerable.Repeat("\"exactMatch\": false, \"scriptFlag\": false, ", 25));

    private static string Clean() =>
        "The lighthouse keeper logged the beacon frequency every morning and " +
        "compared it against the harbor master's records before opening the channel.";

    // ─────────────── seed behavior ───────────────

    [Fact]
    public void SeedPriors_CatchTheLiveDegenerateLoop()
    {
        var judge = new AdaptiveJudge(_dir);
        Assert.Equal(Verdict.Bad, judge.Judge(Degenerate()).Value);   // the "flags" loop
        Assert.NotEqual(Verdict.Bad, judge.Judge(Clean()).Value);     // real text is not bad
    }

    [Theory]
    [InlineData("")]
    [InlineData("ok")]
    [InlineData("!!!! ???? #### $$$$ %%%% ^^^^ &&&& **** (((( ))))")]
    public void WeirdInput_NeverThrows_LeansAwayFromGood(string text)
    {
        var judgment = new AdaptiveJudge(_dir).Judge(text);
        Assert.NotEqual(Verdict.Good, judgment.Value);
    }

    [Fact]
    public void Judgment_CarriesItsReceipts()
    {
        var j = new AdaptiveJudge(_dir).Judge(Degenerate());
        Assert.NotEmpty(j.TopFeatures);
        Assert.Contains(j.TopFeatures, f => f.StartsWith("trigram_repetition"));
    }

    // ─────────────── adaptation ───────────────

    [Fact]
    public void ConsequenceLabels_RehabilitateAStyle_Durably()
    {
        // Terse numbered notes: the seeds dislike them (short, digit-heavy).
        // Confirmations teach the judge this user's terse answers are GOOD —
        // taste adapting to the person, which is the whole point.
        static string Terse(int i) => $"step {i}: flip breaker {i} then wait {i} minutes";

        var judge = new AdaptiveJudge(_dir);
        Assert.NotEqual(Verdict.Good, judge.Judge(Terse(0)).Value);

        for (int i = 0; i < 15; i++)
            judge.Learn(Terse(i), good: true, source: "test");

        Assert.NotEqual(Verdict.Bad, judge.Judge(Terse(99)).Value);   // adapted

        var reloaded = new AdaptiveJudge(_dir);                        // durable
        Assert.NotEqual(Verdict.Bad, reloaded.Judge(Terse(99)).Value);
        Assert.Equal(15, reloaded.EvidenceCount);                      // journaled
    }

    [Fact]
    public void ConfidentVerdicts_AreNotDisturbed_ByAgreeingEvidence()
    {
        var judge = new AdaptiveJudge(_dir);
        double before = judge.Judge(Degenerate()).Score;
        judge.Learn(Degenerate(), good: false, source: "test");       // already outside margin
        Assert.Equal(before, judge.Judge(Degenerate()).Score, 3);     // no update needed
    }

    // ─────────────── constitutional limits ───────────────

    [Fact]
    public void Consolidator_VetoesBadCandidates_ButNeverUserImperatives()
    {
        string storePath = Path.Combine(_dir, "s.jsonl");
        using var store = BlobStore.Open(storePath);
        // Usage-qualified degenerate blob: taste may veto it.
        long junk = store.Append("s1", 0, "user", Degenerate(), 50).Id;
        store.RecordUsage([junk], "s2");
        store.RecordUsage([junk], "s3");
        // Explicit imperative wrapped around equally degenerate content:
        // the user's request outranks taste.
        store.Append("s1", 1, "user", "remember this: " + Degenerate(), 50);

        var consolidator = new GhostConsolidator(store) { Judge = new AdaptiveJudge(_dir) };
        var items = consolidator.Extract();

        Assert.DoesNotContain(items, i => i.BlobId == junk);          // vetoed
        Assert.Contains(items, i => i.Reason == "explicit remember request");
    }

    [Fact]
    public void Corrections_FeedTheJudge_FromBothSides()
    {
        string storePath = Path.Combine(_dir, "s2.jsonl");
        using var store = BlobStore.Open(storePath);
        store.Append("s1", 0, "user", "what is the port?", 5);
        store.Append("s1", 0, "assistant", "the port is 8080 my friend", 7);
        store.Append("s1", 1, "user", "wrong, the port is 9119", 6);

        var judge = new AdaptiveJudge(_dir);
        var consolidator = new GhostConsolidator(store)
        {
            Judge = judge,
            NoteSkillOverride = (_, _, _) => 0,
        };
        consolidator.EmitCorrections(consolidator.ExtractCorrections(),
            "/t.inbox", Path.Combine(_dir, "records"));

        Assert.Equal(2, judge.EvidenceCount);   // wrong answer bad + correction good
    }

    // ─────────────── reflection: bounded multi-layer thought ───────────────

    private sealed class ScriptedSession(params string[] outputs)
        : CNET.Cce.CnetHarness.ICnetInferenceSession
    {
        private readonly Queue<string> _script = new(outputs);
        public List<CNET.Cce.CnetHarness.CnetHarnessGenerateOptions> Calls { get; } = new();

        public CNET.Cce.CnetHarness.CnetHarnessGenerationResult Generate(
            CNET.Cce.CnetHarness.CnetHarnessGenerateOptions options)
        {
            Calls.Add(options);
            string text = _script.Count > 0 ? _script.Dequeue() : "";
            return new CNET.Cce.CnetHarness.CnetHarnessGenerationResult(text, 10, 5, 1, 1, 0, 0,
                CNET.Cce.CnetHarness.CnetHarnessSamplingMode.Balanced, false, 0, 1, 0, 0);
        }

        public CNET.Cce.CnetHarness.CnetHarnessRouteInfo ProbeRoute(string role,
            CNET.Cce.CnetHarness.CnetHarnessSamplingMode overrideMode =
                CNET.Cce.CnetHarness.CnetHarnessSamplingMode.Auto) =>
            throw new NotSupportedException();

        public void Dispose() { }
    }

    [Fact]
    public void Reflection_RegeneratesAFlaggedDraft_StoresWhatTheUserSaw()
    {
        string storePath = Path.Combine(_dir, "r.jsonl");
        using var store = BlobStore.Open(storePath);
        var session = new ScriptedSession(Degenerate(), Clean());
        var ghost = new MemorySession(session, new ConversationMemory(store, t => t.Length / 4 + 1),
                                      4096, t => t.Length / 4 + 1)
        {
            Judge = new AdaptiveJudge(_dir),
        };

        var r = ghost.Generate(null, "tell me about the lighthouse keeper");

        Assert.Equal(1, r.Reflections);
        Assert.Equal(Clean(), r.Result.Text);                       // clean draft won
        Assert.Equal(2, session.Calls.Count);
        Assert.Contains("output-quality filter", session.Calls[1].System);
        Assert.Contains(store.All(), b => b.Text == Clean());       // stored = shown
        Assert.DoesNotContain(store.All(), b => b.Text == Degenerate());
    }

    [Fact]
    public void Reflection_KeepsTheDraft_WhenTheRetryIsWorse()
    {
        string storePath = Path.Combine(_dir, "r2.jsonl");
        using var store = BlobStore.Open(storePath);
        var session = new ScriptedSession(Degenerate(), Degenerate() + Degenerate());
        var ghost = new MemorySession(session, new ConversationMemory(store, t => t.Length / 4 + 1),
                                      4096, t => t.Length / 4 + 1)
        {
            Judge = new AdaptiveJudge(_dir),
        };

        var r = ghost.Generate(null, "hello there");

        Assert.Equal(1, r.Reflections);                             // one round, then stop
        Assert.Equal(2, session.Calls.Count);                       // never loops
    }

    [Fact]
    public void Reflection_LeavesCleanAnswersUntouched()
    {
        string storePath = Path.Combine(_dir, "r3.jsonl");
        using var store = BlobStore.Open(storePath);
        var session = new ScriptedSession(Clean());
        var ghost = new MemorySession(session, new ConversationMemory(store, t => t.Length / 4 + 1),
                                      4096, t => t.Length / 4 + 1)
        {
            Judge = new AdaptiveJudge(_dir),
        };

        var r = ghost.Generate(null, "tell me about the lighthouse");
        Assert.Equal(0, r.Reflections);
        Assert.Single(session.Calls);
    }
}
