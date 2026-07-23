using CNET.Cce.Llm.Eval;
using Xunit;

namespace CNET.Cce.Llm.Tests;

/// <summary>
/// The evaluation harness's own correctness: scoring is fair and mechanical,
/// the two lanes are compared honestly, and regressions (bare model right,
/// stack wrong) are surfaced, not hidden.
/// </summary>
public sealed class EvalHarnessTests
{
    // ─────────────── scoring ───────────────

    [Theory]
    [InlineData("The answer is 4200.", "4200", true)]
    [InlineData("I calculate 18,888,051 exactly.", "18888051", true)]   // commas ignored
    [InlineData("It is 142008.", "4200", false)]                        // not a whole-number match
    [InlineData("Roughly 4199 or so.", "4200", false)]
    public void Numeric_MatchesWholeNumbersOnly(string answer, string expected, bool pass) =>
        Assert.Equal(pass, EvalHarness.Score(
            new EvalCase("c", "q", CheckKind.Numeric, expected), answer));

    [Theory]
    [InlineData("The password is quartz-owl-42, kept safe.", "quartz-owl-42", true)]
    [InlineData("QUARTZ-OWL-42", "quartz-owl-42", true)]                // case-insensitive
    [InlineData("I don't have that information.", "quartz-owl-42", false)]
    public void Contains_IsCaseInsensitiveSubstring(string answer, string expected, bool pass) =>
        Assert.Equal(pass, EvalHarness.Score(
            new EvalCase("c", "q", CheckKind.Contains, expected), answer));

    [Theory]
    [InlineData("The sky is blue because of Rayleigh scattering of sunlight.", true)]
    [InlineData("blah blah blah blah blah blah blah blah", false)]      // degenerate repetition
    [InlineData("ok", false)]                                           // too short
    [InlineData("", false)]
    public void NonDegenerate_RejectsEmptyShortAndRepetitive(string answer, bool pass) =>
        Assert.Equal(pass, EvalHarness.Score(
            new EvalCase("c", "q", CheckKind.NonDegenerate, ""), answer));

    // ─────────────── the run ───────────────

    [Fact]
    public void Run_ScoresBothLanes_AndTalliesByCategory()
    {
        var cases = new List<EvalCase>
        {
            new("arithmetic", "2+2?", CheckKind.Numeric, "4"),
            new("arithmetic", "10*10?", CheckKind.Numeric, "100"),
            new("memory", "password?", CheckKind.Contains, "quartz"),
        };

        // Model-only: wrong on arithmetic, clueless on memory.
        // Full-stack: exact arithmetic, recalls the fact.
        var harness = new EvalHarness(
            modelOnly: q => q.Contains("password") ? "I don't know." : "about 5, maybe",
            fullStack: q => q.Contains("password") ? "it is quartz-owl-42"
                        : q.StartsWith("2+2") ? "4" : "100");

        EvalReport r = harness.Run(cases);

        Assert.Equal(0, r.ModelOnlyTotal);
        Assert.Equal(3, r.FullStackTotal);
        Assert.Equal((0, 2, 2), r.ByCategory["arithmetic"]);
        Assert.Equal((0, 1, 1), r.ByCategory["memory"]);
    }

    [Fact]
    public void Run_SurfacesRegressions()
    {
        var cases = new List<EvalCase> { new("general", "gold symbol?", CheckKind.Contains, "Au") };
        // Bare model right, stack wrong — the harness must expose this.
        var harness = new EvalHarness(
            modelOnly: _ => "The symbol is Au.",
            fullStack: _ => "I have no memory of that.");

        EvalReport r = harness.Run(cases);
        Assert.Equal(1, r.ModelOnlyTotal);
        Assert.Equal(0, r.FullStackTotal);
        var regression = Assert.Single(r.Results);
        Assert.True(regression.ModelOnlyPass && !regression.FullStackPass);
    }

    [Fact]
    public void Run_ToleratesLaneExceptions()
    {
        var cases = new List<EvalCase> { new("c", "q", CheckKind.Contains, "x") };
        var harness = new EvalHarness(
            modelOnly: _ => throw new InvalidOperationException("boom"),
            fullStack: _ => "the answer is x");

        EvalReport r = harness.Run(cases);   // does not throw
        Assert.False(r.Results[0].ModelOnlyPass);
        Assert.True(r.Results[0].FullStackPass);
    }

    [Fact]
    public void Format_ShowsDeltaPerCategoryAndTotal()
    {
        var report = new EvalHarness(_ => "", _ => "").Run(
        [
            new("arithmetic", "q", CheckKind.Contains, "x"),
        ]);
        string text = EvalHarness.Format(report);
        Assert.Contains("model-only", text);
        Assert.Contains("full-stack", text);
        Assert.Contains("TOTAL", text);
    }

    [Fact]
    public void DefaultSuite_IsWellFormed()
    {
        var all = EvalSuite.All();
        Assert.Contains(all, c => c.Category == "arithmetic");
        Assert.Contains(all, c => c.Category == "general");
        Assert.Contains(all, c => c.Category == "memory");
        // Every memory case's expected answer is a seeded fact.
        foreach (EvalCase c in all.Where(c => c.Category == "memory"))
            Assert.Contains(EvalSuite.MemoryFacts, f => f.Expected == c.Expected);
    }
}
