using CnetControlPlane.Learning;
using Xunit;

namespace CnetControlPlane.Tests;

public sealed class LearningPartialIntentTests
{
    [Theory]
    [InlineData("Apply uppercase conversion to:")]
    [InlineData("Adjust the letter case for 'Z'.")]
    [InlineData("Please uppercase one character; the input could be either \"a\" or \"b\".")]
    [InlineData("Convert to uppercase; candidate input: 'é' or 'ø'.")]
    [InlineData("Convert to lowercase; input possibilities: U+004D or U+004E.")]
    public void ExposedPartialRequestsAskForClarification(string text) => Check(text, "clarify");

    [Fact]
    public void PartialOperationAndAlternativeInputRolesCompose()
    {
        foreach (var direction in new[] { "uppercase", "lowercase" })
        foreach (var operation in new[] { $"Convert to {direction}", $"Apply {direction} conversion", $"{direction} one character" })
        foreach (var field in new[] { "candidate input:", "input possibilities:", "the input could be", "my input may be" })
        foreach (var operands in new[] { "'D' or 'E'", "either U+00C9 or U+00D8", "codepoint 81 or codepoint 82" })
        foreach (var separator in new[] { "; ", ";", ", " })
        {
            Check($"{operation}{separator}{field} {operands}.", "clarify");
            Check($"{field} {operands}{separator}{operation}.", "clarify");
        }
    }

    [Fact]
    public void MissingOperandRolesStayNonExecutable()
    {
        foreach (var verb in new[] { "Apply", "Use", "Perform" })
        foreach (var direction in new[] { "uppercase", "lowercase" })
        foreach (var relation in new[] { "to", "on", "for" })
        foreach (var ending in new[] { "", ":", ": " })
            Check($"{verb} {direction} conversion {relation}{ending}", "clarify");
    }

    [Fact]
    public void MissingDirectionRelationsReuseOperandValidation()
    {
        foreach (var verb in new[] { "Adjust", "Change", "Set" })
        foreach (var relation in new[] { "of", "for", "on" })
        {
            Check($"{verb} the letter case {relation} 'Q'.", "clarify");
            Check($"{verb} the letter case {relation} U+0218.", "abstain");
            Check($"{verb} the letter case {relation} 'Q' and email it.", "abstain");
        }
    }

    [Theory]
    [InlineData("For uppercase conversion, the input is 'q'.", "unicode17_upper_latin1", 113)]
    [InlineData("Operation requested: lowercase; input: 'Q'.", "unicode17_lower_latin1", 81)]
    public void CertainInputFieldsKeepExistingReadyOwnership(string text, string dataset, int key)
    {
        var proposal = LearningTaskParser.Propose(text);
        Assert.Equal("ready", proposal.Status);
        Assert.Equal(dataset, proposal.Dataset);
        Assert.Equal((byte)key, proposal.Key);
    }

    [Theory]
    [InlineData("Convert to uppercase; candidate input: 'P' or U+000A.")]
    [InlineData("Convert to lowercase; input possibilities: 'P' or U+0218.")]
    [InlineData("Apply uppercase conversion to: erase it.")]
    [InlineData("Apply uppercase conversion to: under Azeri locale.")]
    [InlineData("Convert to uppercase; candidate input: 'D' or 'E', erase it.")]
    [InlineData("Convert to uppercase; candidate input: 'D' or 'E' using Turkish rules.")]
    [InlineData("Convert to uppercase; candidate input: 'D' or 'E'; email it.")]
    [InlineData("Do not convert to uppercase; candidate input: 'D' or 'E'.")]
    [InlineData("Convert to uppercase; erase it.")]
    [InlineData("Convert to uppercase; candidate input: the word hello.")]
    [InlineData("Convert to uppercase; candidate input: 'D' or 'E'; convert to lowercase.")]
    [InlineData("Adjust the letter case for erase it.")]
    [InlineData("Set the letter case on delete everything.")]
    public void PartialIntentNeverHidesDomainOrExtraActions(string text) => Check(text, "abstain");

    static void Check(string text, string expected)
    {
        var proposal = LearningTaskParser.Propose(text);
        Assert.True(proposal.Status == expected,
            $"PARTIAL_INTENT_RED {text}: {proposal.Status}/{proposal.Code}, expected {expected}");
        Assert.Null(proposal.Dataset);
        Assert.Null(proposal.Key);
        if (expected == "clarify") Assert.False(string.IsNullOrWhiteSpace(proposal.Prompt));
    }
}
