using CnetControlPlane.Learning;
using Xunit;

namespace CnetControlPlane.Tests;

// Exposed sixth confirmation: development regressions, not fresh evidence.
public sealed class LearningTaskRequestCompositionTests
{
    [Theory]
    [InlineData("The decimal code point I am supplying is 0x52. Lowercase its character.", "abstain", null, null)]
    [InlineData("The decimal code point I am supplying is U+0071. Uppercase its character.", "abstain", null, null)]
    [InlineData("Show the lowercase result of U+0052 in hexadecimal.", "abstain", null, null)]
    [InlineData("Return the uppercase form of U+0071 in hexadecimal.", "abstain", null, null)]
    [InlineData("The input code point is 0x0052 in hexadecimal; apply lowercase.", "ready", "lower", 82)]
    [InlineData("The decimal code point I am supplying is 113. Uppercase its character.", "ready", "upper", 113)]
    [InlineData("Input code point is decimal 65 in hexadecimal; apply uppercase.", "abstain", null, null)]
    [InlineData("The hexadecimal input code point is 80. Lowercase its character.", "abstain", null, null)]
    [InlineData("My input is the literal 'U+0041'; make it lowercase.", "abstain", null, null)]
    [InlineData("I need you to turn 'a' into uppercase and delete a file.", "abstain", null, null)]
    [InlineData("For 'a', the requested output format is hexadecimal uppercase.", "abstain", null, null)]
    [InlineData("The character is 'a'; give its uppercase form and execute a command.", "abstain", null, null)]
    [InlineData("Please let me see 'l' as a capital letter.", "ready", "upper", 108)]
    [InlineData("I need you to turn 'v' into a capital.", "ready", "upper", 118)]
    [InlineData("Could 'r' be written in all caps?", "ready", "upper", 114)]
    [InlineData("The character is 'á'; its uppercase form is requested.", "ready", "upper", 225)]
    [InlineData("Uppercase is the case I want for 'ë'.", "ready", "upper", 235)]
    [InlineData("The result I need is the uppercase version of 'û'.", "ready", "upper", 251)]
    [InlineData("Can I see how 'ê' appears in uppercase?", "ready", "upper", 234)]
    [InlineData("What is the upper-case form corresponding to 'ú'?", "ready", "upper", 250)]
    [InlineData("Tell me the capital form associated with 'ä'.", "ready", "upper", 228)]
    [InlineData("For 'í', use the uppercase case conversion.", "ready", "upper", 237)]
    [InlineData("Apply capitalisation to the one-character input 'ý'.", "ready", "upper", 253)]
    [InlineData("I have 'ð' as my input. Its target case is upper.", "ready", "upper", 240)]
    [InlineData("The chosen operation is uppercase, with 'ç' as the supplied character.", "ready", "upper", 231)]
    [InlineData("My input character is 'ñ'; give its capital-letter form.", "ready", "upper", 241)]
    [InlineData("Use 'å' for the input and uppercase for the case.", "ready", "upper", 229)]
    [InlineData("I'd like an uppercase result for Unicode code point U+00E2.", "ready", "upper", 226)]
    [InlineData("What would code point U+00F6 be in upper case?", "ready", "upper", 246)]
    [InlineData("Uppercase the character specified by hexadecimal code point 0x0068.", "ready", "upper", 104)]
    [InlineData("The input code point is 0x00F8 in hexadecimal; apply uppercase.", "ready", "upper", 248)]
    [InlineData("For the scalar with decimal code point 239, the requested output case is uppercase.", "ready", "upper", 239)]
    [InlineData("Please supply uppercase for the character numbered 121 in decimal code-point notation.", "ready", "upper", 121)]
    [InlineData("The decimal code point I am supplying is 254. I'd like its character uppercased.", "ready", "upper", 254)]
    [InlineData("The character '5' is my input for an uppercase conversion.", "ready", "upper", 53)]
    [InlineData("Render the literal '0' using uppercase.", "ready", "upper", 48)]
    [InlineData("For the quoted character ',', please use uppercase.", "ready", "upper", 44)]
    [InlineData("Change the supplied 'Z' to lowercase.", "ready", "lower", 90)]
    [InlineData("The desired case for 'Ú' is lowercase.", "ready", "lower", 218)]
    [InlineData("For the character 'Û', lowercasing is what I need.", "ready", "lower", 219)]
    [InlineData("The operation requested for 'Ò' is lower case.", "ready", "lower", 210)]
    [InlineData("What is the small-letter equivalent corresponding to 'Ç'?", "ready", "lower", 199)]
    [InlineData("How would 'Ä' appear in lower case?", "ready", "lower", 196)]
    [InlineData("Tell me the lowercase character corresponding to 'È'.", "ready", "lower", 200)]
    [InlineData("Lowercase is to be applied to the character 'Á'.", "ready", "lower", 193)]
    [InlineData("The input I chose is 'Ã'. Its requested case is lower.", "ready", "lower", 195)]
    [InlineData("Lower is the operation, and 'Ë' is the input character.", "ready", "lower", 203)]
    [InlineData("Here is the input 'Ê'; please provide its lowercase form.", "ready", "lower", 202)]
    [InlineData("The character to work on is 'Ü', with lowercase as the chosen case.", "ready", "lower", 220)]
    [InlineData("Give Unicode code point U+0044 its small-letter form.", "ready", "lower", 68)]
    [InlineData("With U+00D6 as input, what is its lowercase form?", "ready", "lower", 214)]
    [InlineData("The hexadecimal input code point is 0x00D8. Lowercase its character, please.", "ready", "lower", 216)]
    [InlineData("The scalar at decimal code point 204 is to receive lowercase conversion.", "ready", "lower", 204)]
    [InlineData("I am supplying decimal code point 208 for a lowercasing request.", "ready", "lower", 208)]
    [InlineData("Lowercase is the requested operation on hexadecimal code point 0x00D7.", "ready", "lower", 215)]
    [InlineData("My lowercase request has the quoted input '2'.", "ready", "lower", 50)]
    [InlineData("Show the literal '8' using lowercase.", "ready", "lower", 56)]
    [InlineData("The quoted input is ' '. Please return its lowercase form.", "ready", "lower", 32)]
    [InlineData("For the character '®', the chosen case conversion is lowercase.", "ready", "lower", 174)]
    [InlineData("I would like a character put into all caps.", "clarify", null, null)]
    [InlineData("The case I need is lower.", "clarify", null, null)]
    [InlineData("Please take 'y' as the input character.", "clarify", null, null)]
    [InlineData("The scalar I'm providing is U+00E7.", "clarify", null, null)]
    [InlineData("For the input, I have decimal code point 219.", "clarify", null, null)]
    [InlineData("My literal character is '6'.", "clarify", null, null)]
    [InlineData("The requested case for 'é' is either upper or lower.", "clarify", null, null)]
    [InlineData("My input is 232; make it uppercase.", "clarify", null, null)]
    [InlineData("Make the pair 'pq' into capital letters.", "abstain", null, null)]
    [InlineData("Use Turkish-specific uppercase conversion for 'i'.", "abstain", null, null)]
    public void RequestComponentsBindOnlyExplicitInputAndOperation(string text, string status, string? operation, int? key)
    {
        var proposal = LearningTaskParser.Propose(text);
        Assert.True(proposal.Status == status, $"TASK_REQUEST_COMPOSITION_RED {text}: {proposal.Status}, expected {status}");
        Assert.Equal(operation is null ? null : $"unicode17_{operation}_latin1", proposal.Dataset);
        Assert.Equal(key, proposal.Key is null ? null : (int?)proposal.Key.Value);
        Assert.Equal(status == "clarify", proposal.Prompt is not null);
    }
}
