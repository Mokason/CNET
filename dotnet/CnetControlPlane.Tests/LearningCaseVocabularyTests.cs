using CnetControlPlane.Learning;
using Xunit;

namespace CnetControlPlane.Tests;

// The first Hermes confirmation is exposed development evidence now. These
// regressions retain its misses; they are never fresh acceptance or training truth.
public sealed class LearningCaseVocabularyTests
{
    [Theory]
    [InlineData("Convert the hyphen '-' to uppercase.", "upper", 45)]
    [InlineData("Take decimal codepoint 250 and put it in uppercase.", "upper", 250)]
    [InlineData("Map ' ' to uppercase.", "upper", 32)]
    [InlineData("Downcase F please.", "lower", 70)]
    [InlineData("Apply uppercase mapping to ','.", "upper", 44)]
    [InlineData("Take 'S' down to lowercase, please.", "lower", 83)]
    [InlineData("Fold U+00C9 to lower case.", "lower", 201)]
    [InlineData("Map the punctuation \".\" to lowercase.", "lower", 46)]
    [InlineData("Apply uppercase mapping to the quoted digit '7'", "upper", 55)]
    [InlineData("Rewrite the letter k so that it is uppercase.", "upper", 107)]
    [InlineData("Would you mind lowercasing Q?", "lower", 81)]
    [InlineData("Downcase the character 'H'", "lower", 72)]
    [InlineData("Apply lowercase to the quoted punctuation '!'", "lower", 33)]
    [InlineData("The asterisk '*' should be uppercased.", "upper", 42)]
    [InlineData("Would you recast the letter H so it appears in lowercase?", "lower", 72)]
    [InlineData("Please downcase the digit '0'.", "lower", 48)]
    [InlineData("Give me a lowercase plus sign '+'.", "lower", 43)]
    [InlineData("Convert the space ' ' to lowercase.", "lower", 32)]
    [InlineData("Downcase the semicolon ';'.", "lower", 59)]
    public void SharedCaseVocabularyAndExplicitDescriptionsPreserveOriginalInputs(string text, string operation, byte key)
    {
        var proposal = LearningTaskParser.Propose(text);
        Assert.True(proposal.Status == "ready", $"TASK_VOCABULARY_RED {text}: {proposal.Status}");
        Assert.Equal("unicode17_" + operation + "_latin1", proposal.Dataset);
        Assert.Equal(key, proposal.Key);
    }

    [Theory]
    [InlineData("Go ahead and lowercase it.", "clarify")]
    [InlineData("Convert to uppercase whichever single letter is intended, c or d.", "clarify")]
    [InlineData("Uppercase the letters f and g.", "clarify")]
    [InlineData("Lowercase the letters a and b", "clarify")]
    [InlineData("The letter c should undergo case conversion.", "clarify")]
    [InlineData("I am requesting a case change for the letter p.", "clarify")]
    [InlineData("I still need uppercase applied, yet no letter has been given.", "clarify")]
    [InlineData("Make uppercase the digit 8'", "clarify")]
    [InlineData("Please uppercase the character specified as U+0218.", "abstain")]
    [InlineData("Downcase U+000A.", "abstain")]
    [InlineData("Upcase the word hello.", "abstain")]
    [InlineData("Downcase 'A' and send it.", "abstain")]
    [InlineData("Map 'A' to small.", "abstain")]
    [InlineData("Rewrite 'A' so that it is uppercase and delete it.", "abstain")]
    [InlineData("Convert the hyphen '+' to uppercase.", "clarify")]
    [InlineData("Convert the hyphen to uppercase.", "clarify")]
    [InlineData("Upcase the punctuation 'a'.", "clarify")]
    [InlineData("Take 'a' up to lowercase.", "clarify")]
    [InlineData("Convert to uppercase whichever single letter is intended, c.", "clarify")]
    [InlineData("Convert to lowercase whichever character is intended, U+0218.", "abstain")]
    [InlineData("Map the punctuation U+000A to uppercase.", "abstain")]
    [InlineData("Downcase the hyphen U+0218.", "abstain")]
    [InlineData("Take U+0218 up to lowercase.", "abstain")]
    [InlineData("Go ahead and downcase 'A' and send it.", "abstain")]
    [InlineData("Would you mind lowercasing the word hello?", "abstain")]
    [InlineData("Apply uppercase mapping to 'a' using Turkish rules.", "abstain")]
    [InlineData("Rewrite 'A' so that it is uppercase; then lowercase it.", "abstain")]
    [InlineData("Upcase the letters a.", "clarify")]
    [InlineData("Make A uppercase 'b'", "clarify")]
    [InlineData("Make A lower case 'B'", "clarify")]
    [InlineData("Make a uppercase the letter 'b'", "clarify")]
    [InlineData("Make a uppercase U+0218", "abstain")]
    public void MissingOrConflictingRolesNeverBecomeExecutable(string text, string status)
    {
        var proposal = LearningTaskParser.Propose(text);
        Assert.True(proposal.Status == status, $"TASK_VOCABULARY_RED {text}: {proposal.Status}, expected {status}");
        Assert.Null(proposal.Dataset);
        Assert.Null(proposal.Key);
    }
}
