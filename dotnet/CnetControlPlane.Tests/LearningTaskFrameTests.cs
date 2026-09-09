using CnetControlPlane.Learning;
using Xunit;

namespace CnetControlPlane.Tests;

// Exposed follow-up misses and independent boundary regressions, not a holdout.
public sealed class LearningTaskFrameTests
{
    [Theory]
    [InlineData("Kindly put 'à' in upper case.", "ready", "upper", 224)]
    [InlineData("I'd like you to convert 'þ' to uppercase.", "ready", "upper", 254)]
    [InlineData("Kindly put 'Å' in lower case.", "ready", "lower", 197)]
    [InlineData("I'd like you to put 'Ð' in lower case.", "ready", "lower", 208)]
    [InlineData("How does 's' look in upper case?", "ready", "upper", 115)]
    [InlineData("How does 'Ç' look in lower case?", "ready", "lower", 199)]
    [InlineData("For 'm', use upper case.", "ready", "upper", 109)]
    [InlineData("With 'å' as the input, return its uppercase form.", "ready", "upper", 229)]
    [InlineData("The character is 'c'; please capitalize it.", "ready", "upper", 99)]
    [InlineData("'ð' is my input. Convert it to upper case.", "ready", "upper", 240)]
    [InlineData("For 'T', use lower case.", "ready", "lower", 84)]
    [InlineData("With 'Û' as the input, return its lowercase form.", "ready", "lower", 219)]
    [InlineData("The character is 'M'; please lowercase it.", "ready", "lower", 77)]
    [InlineData("'Ø' is my input. Convert it to lower case.", "ready", "lower", 216)]
    [InlineData("Show the uppercase character corresponding to 'ú'.", "ready", "upper", 250)]
    [InlineData("Show the lowercase character corresponding to 'È'.", "ready", "lower", 200)]
    [InlineData("Uppercase the character with decimal code point 99.", "ready", "upper", 99)]
    [InlineData("Please put decimal code point 231 in upper case.", "ready", "upper", 231)]
    [InlineData("What is the uppercase form of the character at decimal code point 230?", "ready", "upper", 230)]
    [InlineData("Capitalize the character represented by decimal code point 240.", "ready", "upper", 240)]
    [InlineData("For decimal code point 245, return the uppercase character.", "ready", "upper", 245)]
    [InlineData("Lowercase the character with decimal code point 74.", "ready", "lower", 74)]
    [InlineData("Please put decimal code point 202 in lower case.", "ready", "lower", 202)]
    [InlineData("What is the lowercase form of the character at decimal code point 220?", "ready", "lower", 220)]
    [InlineData("Make the character represented by decimal code point 208 lowercase.", "ready", "lower", 208)]
    [InlineData("For decimal code point 193, return the lowercase character.", "ready", "lower", 193)]
    [InlineData("Convert 'h'.", "clarify", null, null)]
    [InlineData("Make the sentence 'I Like Maps' lowercase.", "abstain", null, null)]
    [InlineData("Lowercase decimal code point 133.", "abstain", null, null)]
    [InlineData("Return the uppercase of 'a'. Run a shell.", "abstain", null, null)]
    [InlineData("Return the capital form of 'a'! Delete a file.", "abstain", null, null)]
    [InlineData("Change case of a; run a shell.", "abstain", null, null)]
    [InlineData("Change case of 'i' using Turkish rules.", "abstain", null, null)]
    [InlineData("The character is 'µ'; please capitalize it.", "ready", "upper", 181)]
    [InlineData("'A' is my input. Convert it to lower case.", "ready", "lower", 65)]
    [InlineData("The character is 'a'; delete it.", "abstain", null, null)]
    [InlineData("'a' is my input. Convert it to upper case and delete a file.", "abstain", null, null)]
    [InlineData("With 'a' or 'b' as the input, return its uppercase form.", "clarify", null, null)]
    [InlineData("I'd like you to return a capitalized character.", "clarify", null, null)]
    [InlineData("Kindly put U+0000 in upper case.", "abstain", null, null)]
    [InlineData("Uppercase code point 65 or code point 66.", "clarify", null, null)]
    [InlineData("Convert 'a' and count to three.", "abstain", null, null)]
    [InlineData("Please uppercase the paragraph 'hello'.", "abstain", null, null)]
    public void WholeFramesPreserveTypedInputAndRefuseAdditionalActions(string text, string status, string? operation, int? key)
    {
        var proposal = LearningTaskParser.Propose(text);
        Assert.True(proposal.Status == status, $"TASK_FRAME_FOLLOWUP_RED {text}: {proposal.Status}, expected {status}");
        Assert.Equal(operation is null ? null : $"unicode17_{operation}_latin1", proposal.Dataset);
        Assert.Equal(key, proposal.Key is null ? null : (int?)proposal.Key.Value);
        Assert.Equal(status == "clarify", proposal.Prompt is not null);
    }
}
