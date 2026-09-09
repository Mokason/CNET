using CnetControlPlane.Learning;
using Xunit;

namespace CnetControlPlane.Tests;

// Tenth confirmation is exposed development data; authored boundaries are not blind evidence.
public sealed class LearningTaskRequestRoleTests
{
    [Theory]
    [InlineData("Uppercase and uppercase 'R'.", "abstain", null, null)]
    [InlineData("Which small letter is the uppercase form of U+0000?", "abstain", null, null)]
    [InlineData("Show the lowercase form of U+0052 in hexadecimal for my result.", "abstain", null, null)]
    [InlineData("Can the letter \"i\" be shown in uppercase?", "ready", "upper", 105)]
    [InlineData("The uppercase conversion I am requesting uses \"c\".", "ready", "upper", 99)]
    [InlineData("I'd like 'ü' expressed as a capital letter.", "ready", "upper", 252)]
    [InlineData("Which capital letter is the uppercase form of 'ó'?", "ready", "upper", 243)]
    [InlineData("Use the capital-letter equivalent of 'î' for my result.", "ready", "upper", 238)]
    [InlineData("For the single character 'à', an uppercase conversion, please.", "ready", "upper", 224)]
    [InlineData("The case transformation for 'å' should be upper.", "ready", "upper", 229)]
    [InlineData("This request has the input 'æ' and the operation uppercase.", "ready", "upper", 230)]
    [InlineData("I've chosen 'ñ' as the input; please show its uppercase version.", "ready", "upper", 241)]
    [InlineData("The operation is uppercase. Use 'ß' as the input scalar.", "ready", "upper", 223)]
    [InlineData("Uppercase conversion is my request for the scalar 'µ'.", "ready", "upper", 181)]
    [InlineData("My requested operation for code point U+00FF is uppercase.", "ready", "upper", 255)]
    [InlineData("Use the character U+00F0 for this capital-letter conversion.", "ready", "upper", 240)]
    [InlineData("For hexadecimal code point 0x0076, could you give its uppercase form?", "ready", "upper", 118)]
    [InlineData("The decimal code point of my input is 242, and I want uppercase.", "ready", "upper", 242)]
    [InlineData("Use an uppercase conversion for my quoted input character '|'.", "ready", "upper", 124)]
    [InlineData("Can the letter \"I\" be shown in lower case?", "ready", "lower", 73)]
    [InlineData("The lowercase conversion I am requesting uses \"C\".", "ready", "lower", 67)]
    [InlineData("I'd like 'Ü' expressed as a small letter.", "ready", "lower", 220)]
    [InlineData("Which small letter is the lowercase form of 'Ó'?", "ready", "lower", 211)]
    [InlineData("Use the small-letter equivalent of 'Î' for my result.", "ready", "lower", 206)]
    [InlineData("For the single character 'À', a lowercase conversion, please.", "ready", "lower", 192)]
    [InlineData("The case transformation for 'Å' should be lower.", "ready", "lower", 197)]
    [InlineData("This request has the input 'Æ' and the operation lowercase.", "ready", "lower", 198)]
    [InlineData("I've chosen 'Ñ' as the input; please show its lowercase version.", "ready", "lower", 209)]
    [InlineData("The operation is lowercase. Use 'ß' as the input scalar.", "ready", "lower", 223)]
    [InlineData("Lowercase conversion is my request for the scalar 'ÿ'.", "ready", "lower", 255)]
    [InlineData("My requested operation for code point U+00D5 is lowercase.", "ready", "lower", 213)]
    [InlineData("Use the character U+00D0 for this small-letter conversion.", "ready", "lower", 208)]
    [InlineData("For hexadecimal code point 0x0056, could you give its lowercase form?", "ready", "lower", 86)]
    [InlineData("The decimal code point of my input is 210, and I want lowercase.", "ready", "lower", 210)]
    [InlineData("Use a lowercase conversion for my quoted input character '~'.", "ready", "lower", 126)]
    [InlineData("Could you produce a capital-letter version?", "clarify", null, null)]
    [InlineData("The transformation I am requesting is lowercasing.", "clarify", null, null)]
    [InlineData("My input character for this task is \"ö\".", "clarify", null, null)]
    [InlineData("I have supplied Unicode code point U+00C4.", "clarify", null, null)]
    [InlineData("The selected input has decimal code point 245.", "clarify", null, null)]
    [InlineData("The literal character I'm giving you is \"@\".", "clarify", null, null)]
    [InlineData("I would like \"å\" in uppercase or in lowercase.", "clarify", null, null)]
    [InlineData("Please convert decimal code point 195 to either upper or lower case.", "clarify", null, null)]
    [InlineData("Which capital letter is the lowercase form of 'A'?", "clarify", null, null)]
    [InlineData("Which small letter is the uppercase form of 'a'?", "clarify", null, null)]
    [InlineData("Which capital letter is the uppercase form of U+0100?", "abstain", null, null)]
    [InlineData("Which small letter is the lowercase form of 'A' in hexadecimal?", "abstain", null, null)]
    [InlineData("The decimal code point of my input is 0x61, and I want uppercase.", "abstain", null, null)]
    [InlineData("Can 'R' be shown to be lowercase?", "abstain", null, null)]
    [InlineData("Could 'R' be shown in lowercase?", "ready", "lower", 82)]
    [InlineData("The uppercase conversion I am requesting uses 'q' and runs a command.", "abstain", null, null)]
    [InlineData("Produce a lowercase version of 'Q'.", "ready", "lower", 81)]
    [InlineData("My input character for this task is U+0000.", "abstain", null, null)]
    [InlineData("My input character for this task is \"Ö'.", "clarify", null, null)]
    [InlineData("Use the uppercase equivalent of 'q' for my result in hexadecimal.", "abstain", null, null)]
    public void RequestedOperationsAndInputRolesRemainDistinct(string text, string status, string? operation, int? key)
    {
        var proposal = LearningTaskParser.Propose(text);
        Assert.True(proposal.Status == status, $"TASK_REQUEST_ROLE_RED {text}: {proposal.Status}, expected {status}");
        Assert.Equal(operation is null ? null : $"unicode17_{operation}_latin1", proposal.Dataset);
        Assert.Equal(key, proposal.Key is null ? null : (int?)proposal.Key.Value);
        Assert.Equal(status == "clarify", proposal.Prompt is not null);
    }
}
