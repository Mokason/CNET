using CnetControlPlane.Learning;
using Xunit;

namespace CnetControlPlane.Tests;

// Ninth confirmation is exposed development data, never fresh validation.
public sealed class LearningTaskClauseCompositionTests
{
    [Theory]
    [InlineData("Give 'Q' the corresponding lowercase result.", "abstain", null, null)]
    [InlineData("Show 'Q' the corresponding uppercase result.", "abstain", null, null)]
    [InlineData("Supply 'Q' the corresponding lowercase letter.", "abstain", null, null)]
    [InlineData("Make 'Q' the corresponding lowercase letter.", "ready", "lower", 81)]
    [InlineData("Turn 'q' into the corresponding capital letter.", "ready", "upper", 113)]
    [InlineData("I'd like the uppercase character that goes with 'q'.", "ready", "upper", 113)]
    [InlineData("The character 'f' is the one I want capitalized.", "ready", "upper", 102)]
    [InlineData("I am asking for 'l' to be written in uppercase.", "ready", "upper", 108)]
    [InlineData("Please turn 'd' into the corresponding capital letter.", "ready", "upper", 100)]
    [InlineData("What capital would result from the letter 'ô'?", "ready", "upper", 244)]
    [InlineData("The uppercase form I need is the one for 'ò'.", "ready", "upper", 242)]
    [InlineData("Show me the character 'õ' after uppercasing.", "ready", "upper", 245)]
    [InlineData("The input scalar is 'ÿ'; I want uppercase applied to it.", "ready", "upper", 255)]
    [InlineData("My character is 'ß'. The conversion I want is uppercase.", "ready", "upper", 223)]
    [InlineData("I have supplied 'æ' for a capital-letter conversion.", "ready", "upper", 230)]
    [InlineData("Input character: 'à'; operation requested: upper.", "ready", "upper", 224)]
    [InlineData("Make g the corresponding uppercase letter.", "ready", "upper", 103)]
    [InlineData("The letter to put into capitals is ñ.", "ready", "upper", 241)]
    [InlineData("The character identified as U+00F8 should be uppercased.", "ready", "upper", 248)]
    [InlineData("Use code point 0x0077, in hexadecimal, for an uppercase conversion.", "ready", "upper", 119)]
    [InlineData("Uppercase is the desired case for hexadecimal code point 0x00E3.", "ready", "upper", 227)]
    [InlineData("Make the single quoted digit '4' uppercase.", "ready", "upper", 52)]
    [InlineData("The literal digit '6' should receive the uppercase operation.", "ready", "upper", 54)]
    [InlineData("Could the quoted space ' ' receive an uppercase conversion?", "ready", "upper", 32)]
    [InlineData("Give the uppercase form for the currency character '£'.", "ready", "upper", 163)]
    [InlineData("I'd like the lowercase character that corresponds to 'Q'.", "ready", "lower", 81)]
    [InlineData("The letter 'F' is the one I want lowercased.", "ready", "lower", 70)]
    [InlineData("I am asking for 'L' to be written in lowercase.", "ready", "lower", 76)]
    [InlineData("Please turn 'D' into the corresponding small letter.", "ready", "lower", 68)]
    [InlineData("What small letter would result from the input 'Ô'?", "ready", "lower", 212)]
    [InlineData("The lowercase form I need is the one for 'Ò'.", "ready", "lower", 210)]
    [InlineData("Show me the character 'Õ' after lowercasing.", "ready", "lower", 213)]
    [InlineData("For 'Î', I am requesting a lower-case conversion.", "ready", "lower", 206)]
    [InlineData("The input scalar is 'Ý'; I want lowercase applied to it.", "ready", "lower", 221)]
    [InlineData("My character is 'Ð'. The conversion I want is lowercase.", "ready", "lower", 208)]
    [InlineData("I have supplied 'Æ' for a small-letter conversion.", "ready", "lower", 198)]
    [InlineData("Operation requested: lower; input character: 'À'.", "ready", "lower", 192)]
    [InlineData("Make B the corresponding lowercase letter.", "ready", "lower", 66)]
    [InlineData("The letter to put into small-letter form is Ñ.", "ready", "lower", 209)]
    [InlineData("The character identified as U+00D8 should be lowercased.", "ready", "lower", 216)]
    [InlineData("Use code point 0x0057, in hexadecimal, for a lowercase conversion.", "ready", "lower", 87)]
    [InlineData("Lowercase is the desired case for hexadecimal code point 0x00C3.", "ready", "lower", 195)]
    [InlineData("Make the single quoted digit '4' lowercase.", "ready", "lower", 52)]
    [InlineData("The literal digit '6' should receive the lowercase operation.", "ready", "lower", 54)]
    [InlineData("Could the quoted space ' ' receive a lowercase conversion?", "ready", "lower", 32)]
    [InlineData("Give the lowercase form for the currency character '¥'.", "ready", "lower", 165)]
    [InlineData("Please carry out lowercase conversion.", "clarify", null, null)]
    [InlineData("A capital-letter form is what I'm requesting.", "clarify", null, null)]
    [InlineData("This task has the input character 'î'.", "clarify", null, null)]
    [InlineData("I'm specifying the input U+00D8.", "clarify", null, null)]
    [InlineData("I have a quoted input character: '!'.", "clarify", null, null)]
    [InlineData("Use 'u' or 'v' for my uppercase conversion.", "clarify", null, null)]
    [InlineData("I want 'ë' changed to either uppercase or lowercase.", "clarify", null, null)]
    [InlineData("The case I need for decimal code point 217 is upper or lower.", "clarify", null, null)]
    [InlineData("Show me the lowercase result to 'Q'.", "abstain", null, null)]
    [InlineData("Use code point 0x0085, in hexadecimal, for an uppercase conversion.", "abstain", null, null)]
    [InlineData("Use decimal code point 0x61, in hexadecimal, for an uppercase conversion.", "abstain", null, null)]
    [InlineData("Return the lowercase result of code point 0x0052 in hexadecimal.", "abstain", null, null)]
    [InlineData("I want uppercase applied to it.", "clarify", null, null)]
    [InlineData("The letter to put into capitals is 'a'; run code.", "abstain", null, null)]
    [InlineData("Make the single quoted digit '42' uppercase.", "abstain", null, null)]
    [InlineData("Make the supplied quoted character 'a' uppercase.", "ready", "upper", 97)]
    [InlineData("The letter to put into capitals is the character identified as U+0061.", "ready", "upper", 97)]
    [InlineData("This task has the input character U+0100.", "abstain", null, null)]
    [InlineData("Could 'R' receive a lowercase conversion?", "ready", "lower", 82)]
    [InlineData("Could 'R' receive a lowercase result?", "clarify", null, null)]
    [InlineData("Show me 'R' to be a lowercase character.", "abstain", null, null)]
    [InlineData("My uppercase input is 'a'.", "clarify", null, null)]
    public void ClausesAndDescriptionsPreserveTypedBoundaries(string text, string status, string? operation, int? key)
    {
        var proposal = LearningTaskParser.Propose(text);
        Assert.True(proposal.Status == status, $"TASK_CLAUSE_COMPOSITION_RED {text}: {proposal.Status}, expected {status}");
        Assert.Equal(operation is null ? null : $"unicode17_{operation}_latin1", proposal.Dataset);
        Assert.Equal(key, proposal.Key is null ? null : (int?)proposal.Key.Value);
        Assert.Equal(status == "clarify", proposal.Prompt is not null);
    }
}
