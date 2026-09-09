using CnetControlPlane.Learning;
using Xunit;

namespace CnetControlPlane.Tests;

// Exposed eighth confirmation plus authored boundaries; development only.
public sealed class LearningTaskRoleCompositionTests
{
    [Theory]
    [InlineData("Show the uppercase result to 'Q'.", "abstain", null, null)]
    [InlineData("Present the lowercase version to 'a'.", "abstain", null, null)]
    [InlineData("Show uppercase applied to 'Q'.", "ready", "upper", 81)]
    [InlineData("Show the lowercase counterpart to 'Q'.", "ready", "lower", 81)]
    [InlineData("Please present a capital-letter version of 'a'.", "ready", "upper", 97)]
    [InlineData("Could you give me the upper-case counterpart to 'i'?", "ready", "upper", 105)]
    [InlineData("Show what 'o' becomes under uppercase conversion.", "ready", "upper", 111)]
    [InlineData("The requested case for the letter 'b' is upper.", "ready", "upper", 98)]
    [InlineData("I need uppercase applied to the character 'c'.", "ready", "upper", 99)]
    [InlineData("For the letter 'r', capitalisation is the operation I want.", "ready", "upper", 114)]
    [InlineData("What is the capital-letter counterpart to 'ç'?", "ready", "upper", 231)]
    [InlineData("Please let 'á' appear in uppercase.", "ready", "upper", 225)]
    [InlineData("I'm requesting the capital-letter form of 'ñ'.", "ready", "upper", 241)]
    [InlineData("The character I'd like uppercased is 'ß'.", "ready", "upper", 223)]
    [InlineData("I have chosen 'í' as the input; return its capital form.", "ready", "upper", 237)]
    [InlineData("The input for my request is 'è'. I need uppercase.", "ready", "upper", 232)]
    [InlineData("For this conversion, use the character 'ø' and the case uppercase.", "ready", "upper", 248)]
    [InlineData("My uppercase conversion should use the letter é.", "ready", "upper", 233)]
    [InlineData("The uppercase task uses Unicode scalar U+00F0.", "ready", "upper", 240)]
    [InlineData("The input character has decimal code point 250; convert it to uppercase.", "ready", "upper", 250)]
    [InlineData("I need uppercase conversion for the quoted space ' '.", "ready", "upper", 32)]
    [InlineData("Can you give me the small-letter counterpart to 'O'?", "ready", "lower", 79)]
    [InlineData("Show what the character 'I' becomes under lowercase conversion.", "ready", "lower", 73)]
    [InlineData("The case requested for the letter 'B' is lower.", "ready", "lower", 66)]
    [InlineData("The operation I want for 'R' is a lowercase conversion.", "ready", "lower", 82)]
    [InlineData("What is the lower-case counterpart to the character 'Ç'?", "ready", "lower", 199)]
    [InlineData("I'd like 'Á' to appear in lowercase.", "ready", "lower", 193)]
    [InlineData("The character I'd like lowercased is 'Ý'.", "ready", "lower", 221)]
    [InlineData("The input for this conversion is 'Ò'. I need lowercase.", "ready", "lower", 210)]
    [InlineData("For the conversion, use 'Ø' as the character and lowercase as the case.", "ready", "lower", 216)]
    [InlineData("My lowercase conversion should use the letter É.", "ready", "lower", 201)]
    [InlineData("This lowercase task uses Unicode scalar U+00D0.", "ready", "lower", 208)]
    [InlineData("The input character has decimal code point 218; convert it to lowercase.", "ready", "lower", 218)]
    [InlineData("For the quoted space ' ', I need a lowercase conversion.", "ready", "lower", 32)]
    [InlineData("Please uppercase the character I have in mind.", "clarify", null, null)]
    [InlineData("I want to see the small-letter result.", "clarify", null, null)]
    [InlineData("The input I want to use is 'é'.", "clarify", null, null)]
    [InlineData("Please take Unicode scalar U+00DC for this task.", "clarify", null, null)]
    [InlineData("The code point I am giving you is 233 in decimal.", "clarify", null, null)]
    [InlineData("My one-character input is '$'.", "clarify", null, null)]
    [InlineData("My requested form for 'ä' is uppercase or lowercase.", "clarify", null, null)]
    [InlineData("Convert code point U+00C1 to upper or lower case.", "clarify", null, null)]
    [InlineData("The uppercase operation should use &.", "clarify", null, null)]
    [InlineData("The lowercase input is \"Ö'.", "clarify", null, null)]
    [InlineData("Present the uppercase version of 'a' and open a file.", "abstain", null, null)]
    [InlineData("The requested case for U+0100 is upper.", "abstain", null, null)]
    [InlineData("For this conversion, use 'a'; run a shell.", "abstain", null, null)]
    [InlineData("Show what 'a' becomes under uppercase conversion in hexadecimal.", "abstain", null, null)]
    [InlineData("My uppercase input is 'p'.", "clarify", null, null)]
    [InlineData("The input character has decimal code point 0x61; convert it to uppercase.", "abstain", null, null)]
    [InlineData("The requested case for the quoted space ' ' is uppercase.", "ready", "upper", 32)]
    [InlineData("The requested case for the quoted space 'a' is uppercase.", "clarify", null, null)]
    [InlineData("Uppercase the character I have in mind and run code.", "abstain", null, null)]
    [InlineData("For the input 'x', the case is uppercase and the operation is lowercase.", "abstain", null, null)]
    public void CompleteRoleBindingsPreserveTheOriginalOperand(string text, string status, string? operation, int? key)
    {
        var proposal = LearningTaskParser.Propose(text);
        Assert.True(proposal.Status == status, $"TASK_ROLE_COMPOSITION_RED {text}: {proposal.Status}, expected {status}");
        Assert.Equal(operation is null ? null : $"unicode17_{operation}_latin1", proposal.Dataset);
        Assert.Equal(key, proposal.Key is null ? null : (int?)proposal.Key.Value);
        Assert.Equal(status == "clarify", proposal.Prompt is not null);
    }
}
