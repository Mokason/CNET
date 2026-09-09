using CnetControlPlane.Learning;
using Xunit;

namespace CnetControlPlane.Tests;

// Exposed second Hermes misses plus authored slot-boundary regressions.
// These are development cases, never fresh acceptance or training truth.
public sealed class LearningCaseSlotTests
{
    [Theory]
    [InlineData("Change this to upper case: \"9\"", "upper", 57)]
    [InlineData("Downcase the byte 0x4B.", "lower", 75)]
    [InlineData("Force uppercase on the letter o.", "upper", 111)]
    [InlineData("Would you mind putting G in lowercase?", "lower", 71)]
    [InlineData("Raise decimal codepoint 250 to uppercase.", "upper", 250)]
    [InlineData("Would you mind putting \"P\" into lowercase?", "lower", 80)]
    [InlineData("If you have a moment, capitalize the letter d.", "upper", 100)]
    [InlineData("Uppercase: 'a'", "upper", 97)]
    [InlineData("Make uppercase: 'a'", "upper", 97)]
    [InlineData("Change that to lowercase: U+0041", "lower", 65)]
    [InlineData("Uppercase the letter shown as E.", "upper", 69)]
    [InlineData("Downcase the byte 'E'.", "lower", 69)]
    [InlineData("Downcase the byte codepoint 69.", "lower", 69)]
    [InlineData("Downcase the byte '''", "lower", 39)]
    [InlineData("Downcase the byte \"\"\"", "lower", 34)]
    public void ExplicitSlotsPreserveTheOriginalScalar(string text, string direction, byte key)
    {
        var proposal = LearningTaskParser.Propose(text);
        Assert.True(proposal.Status == "ready", $"TASK_SLOT_RED {text}: {proposal.Status}");
        Assert.Equal("unicode17_" + direction + "_latin1", proposal.Dataset);
        Assert.Equal(key, proposal.Key);
    }

    [Theory]
    [InlineData("Make uppercase: 'Q", "clarify")]
    [InlineData("Apply an uppercase conversion to the letter shown as E or as U.", "clarify")]
    [InlineData("lowercase a or b, whichever you prefer", "clarify")]
    [InlineData("I need a case conversion applied to the letter S.", "clarify")]
    [InlineData("Uppercase either r or s, whichever you prefer.", "clarify")]
    [InlineData("Uppercase a, whichever you prefer.", "clarify")]
    [InlineData("Change this to uppercase: 'a' or 'b'", "clarify")]
    [InlineData("Raise 'a' to lowercase.", "clarify")]
    [InlineData("Downcase the byte 75.", "clarify")]
    [InlineData("Change this to uppercase: U+000A", "abstain")]
    [InlineData("Change A to uppercase: 'b'", "abstain")]
    [InlineData("Change this to uppercase: 'a' and delete it", "abstain")]
    [InlineData("Uppercase the letter shown as E or as U+000A.", "abstain")]
    [InlineData("Uppercase U+0218, whichever you prefer.", "abstain")]
    [InlineData("I need a case conversion applied to the word hello.", "abstain")]
    [InlineData("Downcase the byte 0x0A.", "abstain")]
    [InlineData("Force uppercase on 'a' using Turkish rules.", "abstain")]
    [InlineData("Would you mind putting 'G' in lowercase and sending it?", "abstain")]
    [InlineData("Raise U+0218 to lowercase.", "abstain")]
    [InlineData("If you have a moment, capitalize 'a' and send it.", "abstain")]
    [InlineData("If 'a' is uppercase, lowercase 'b'.", "abstain")]
    [InlineData("Make uppercase: 'ab'", "abstain")]
    [InlineData("Raise this to lowercase: 'a'", "clarify")]
    [InlineData("Raise 'a' so that it is lowercase.", "clarify")]
    [InlineData("Raise this to lowercase: U+000A", "abstain")]
    [InlineData("Uppercase shown as E.", "clarify")]
    [InlineData("Apply uppercase to shown as E.", "clarify")]
    [InlineData("Downcase the byte A.", "clarify")]
    [InlineData("Downcase the byte B.", "clarify")]
    [InlineData("Downcase the byte E.", "clarify")]
    [InlineData("Downcase the byte F.", "clarify")]
    [InlineData("Downcase the byte G.", "clarify")]
    [InlineData("Downcase the byte shown as E.", "clarify")]
    [InlineData("Downcase the byte 0xE.", "abstain")]
    public void UnresolvedSlotsNeverSelectAnAction(string text, string expected)
    {
        var proposal = LearningTaskParser.Propose(text);
        Assert.True(proposal.Status == expected, $"TASK_SLOT_RED {text}: {proposal.Status}, expected {expected}");
        Assert.Null(proposal.Dataset);
        Assert.Null(proposal.Key);
    }
}
