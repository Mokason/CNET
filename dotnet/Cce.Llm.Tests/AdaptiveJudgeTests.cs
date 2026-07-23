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
}
