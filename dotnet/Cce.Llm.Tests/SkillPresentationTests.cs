using CNET.Cce.Llm;
using Xunit;

namespace CNET.Cce.Llm.Tests;

public sealed class SkillPresentationTests
{
    [Fact]
    public void FormatCertified_IncludesUnitAndDecoded()
    {
        string s = SkillPresentation.FormatCertified(
            "acq_research_unity_core_loop",
            picks: new[] { 1, 2 },
            decoded: "core_loop, juice",
            query: "one verb");
        Assert.Contains("·certified", s);
        Assert.Contains("acq_research_unity_core_loop", s);
        Assert.Contains("core_loop, juice", s);
        Assert.Contains("one verb", s);
    }

    [Fact]
    public void DecodePicks_UsesLabelsWhenPresent()
    {
        string d = SkillPresentation.DecodePicks(
            new[] { 0, 2 },
            new[] { "calc", "mem", "web" });
        Assert.Equal("calc, web", d);
    }

    [Fact]
    public void DecodePicks_FallsBackToIndices()
    {
        string d = SkillPresentation.DecodePicks(new[] { 3, 4 }, null);
        Assert.Equal("3, 4", d);
    }

    [Fact]
    public void FormatCertified_PicksOnlyWhenNoDecoded()
    {
        string s = SkillPresentation.FormatCertified("u", new[] { 7 }, null);
        Assert.Contains("picks [7]", s);
    }
}
