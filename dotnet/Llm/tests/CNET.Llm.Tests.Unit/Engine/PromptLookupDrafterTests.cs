using CNET.Llm.Engine;
using Xunit;

namespace CNET.Llm.Tests.Unit.Engine;

/// <summary>
/// Tests for the draft-free n-gram drafter.
///
/// Its whole value is that a miss is cheap and a hit is free, so the cases that
/// matter are: does it find a real continuation, does it prefer the longest and
/// most recent match, and does it decline cleanly rather than proposing noise.
/// </summary>
public sealed class PromptLookupDrafterTests
{
    private static int[] DraftOf(PromptLookupDrafter d, int[] sequence, out int count)
    {
        var buf = new int[d.MaxCandidates];
        count = d.Draft(sequence, buf);
        return buf[..count];
    }

    [Fact]
    public void ProposesTheContinuationThatFollowedTheEarlierMatch()
    {
        var drafter = new PromptLookupDrafter(maxNgram: 3, minNgram: 2, maxCandidates: 4);
        // "1 2 3" appeared before and was followed by 7 8 9; the suffix repeats it.
        int[] seq = [1, 2, 3, 7, 8, 9, 5, 1, 2, 3];

        int[] got = DraftOf(drafter, seq, out int n);

        Assert.Equal(4, n);
        Assert.Equal([7, 8, 9, 5], got);
    }

    [Fact]
    public void NoMatch_ProposesNothing()
    {
        var drafter = new PromptLookupDrafter(maxNgram: 3, minNgram: 2, maxCandidates: 4);
        int[] seq = [1, 2, 3, 4, 5, 6, 7, 8];

        DraftOf(drafter, seq, out int n);

        // Declining is the correct answer — the caller then decodes normally, and
        // the only cost was the scan.
        Assert.Equal(0, n);
    }

    [Fact]
    public void PrefersTheLongerMatch()
    {
        var drafter = new PromptLookupDrafter(maxNgram: 3, minNgram: 1, maxCandidates: 2);
        // Suffix "8 2 3". The 3-gram occurs once (followed by 40); the 2-gram
        // "2 3" also occurs earlier followed by 99. Longest must win.
        int[] seq = [2, 3, 99, 50, 8, 2, 3, 40, 41, 60, 8, 2, 3];

        int[] got = DraftOf(drafter, seq, out int n);

        Assert.Equal(2, n);
        Assert.Equal([40, 41], got);
    }

    [Fact]
    public void PrefersTheMostRecentOccurrence()
    {
        var drafter = new PromptLookupDrafter(maxNgram: 2, minNgram: 2, maxCandidates: 2);
        // "4 5" occurs twice: early (then 11 12) and late (then 21 22). The
        // nearer continuation is the better predictor.
        int[] seq = [4, 5, 11, 12, 90, 4, 5, 21, 22, 91, 4, 5];

        int[] got = DraftOf(drafter, seq, out int n);

        Assert.Equal([21, 22], got);
    }

    [Fact]
    public void ClampsToAvailableTokens()
    {
        var drafter = new PromptLookupDrafter(maxNgram: 2, minNgram: 2, maxCandidates: 8);
        // "1 2" occurred at the start and everything after it is fair game, even
        // though that run happens to include the suffix being matched. Only four
        // tokens exist after the match, so the proposal is clamped to four rather
        // than the configured eight.
        int[] seq = [1, 2, 30, 31, 1, 2];

        int[] got = DraftOf(drafter, seq, out int n);

        Assert.Equal(4, n);
        Assert.Equal([30, 31, 1, 2], got);
    }

    [Theory]
    [InlineData(0)]
    [InlineData(1)]
    [InlineData(2)]
    public void ShortSequences_ProposeNothing(int length)
    {
        var drafter = new PromptLookupDrafter(maxNgram: 3, minNgram: 2, maxCandidates: 4);
        int[] seq = Enumerable.Range(1, length).ToArray();

        var buf = new int[drafter.MaxCandidates];
        Assert.Equal(0, drafter.Draft(seq, buf));
    }

    [Fact]
    public void RejectsUndersizedCandidateBuffer()
    {
        var drafter = new PromptLookupDrafter(maxCandidates: 8);
        var tooSmall = new int[4];
        Assert.Throws<ArgumentException>(() =>
        {
            int[] seq = [1, 2, 3, 1, 2, 3];
            drafter.Draft(seq, tooSmall);
        });
    }

    [Theory]
    [InlineData(0, 2, 4)]     // maxNgram < 1
    [InlineData(3, 0, 4)]     // minNgram < 1
    [InlineData(2, 3, 4)]     // minNgram > maxNgram
    [InlineData(3, 2, 0)]     // maxCandidates < 1
    public void RejectsInvalidConfiguration(int maxNgram, int minNgram, int maxCandidates)
        => Assert.Throws<ArgumentOutOfRangeException>(
            () => new PromptLookupDrafter(maxNgram, minNgram, maxCandidates));

    /// <summary>
    /// The realistic win case: output that quotes structure already in context.
    /// </summary>
    [Fact]
    public void RepeatedStructure_ProposesTheFullRun()
    {
        var drafter = new PromptLookupDrafter(maxNgram: 3, minNgram: 2, maxCandidates: 10);
        // Think: a JSON key sequence emitted once and being emitted again.
        int[] template = [100, 101, 102, 103, 104, 105];
        int[] seq = [.. template, 200, 201, .. template[..2]];

        int[] got = DraftOf(drafter, seq, out int n);

        // The rest of the template comes first, which is the part that will be
        // accepted; the tokens past it are proposed too because over-proposing
        // is free — surplus candidates are simply rejected at verification, and
        // the extra batch positions cost no additional weight streaming.
        Assert.Equal(8, n);
        Assert.Equal([102, 103, 104, 105, 200, 201, 100, 101], got);
    }
}
