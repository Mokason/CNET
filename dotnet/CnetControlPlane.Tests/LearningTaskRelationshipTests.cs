using CnetControlPlane.Learning;
using Xunit;

namespace CnetControlPlane.Tests;

// Exposed seventh confirmation; development regressions, never fresh evidence.
public sealed class LearningTaskRelationshipTests
{
    [Theory]
    [InlineData("My uppercase input is 'P'.", "clarify", null, null)]
    [InlineData("My lowercase input is 'P'.", "clarify", null, null)]
    [InlineData("My capital-letter input is 'P'.", "clarify", null, null)]
    [InlineData("My lowercase-conversion input is 'P'.", "ready", "lower", 80)]
    [InlineData("My uppercase operation input is 'p'.", "ready", "upper", 112)]
    [InlineData("I'd like 'a' converted to uppercase.", "ready", "upper", 97)]
    [InlineData("Please uppercase '''.", "ready", "upper", 39)]
    [InlineData("Please lowercase \"\"\" .", "ready", "lower", 34)]
    [InlineData("Make everything in 'several words' lowercase.", "abstain", null, null)]
    [InlineData("The given input is 'a'; requested transformation: uppercase; execute a script.", "abstain", null, null)]
    [InlineData("Give the uppercase code point produced from 'a' in decimal.", "abstain", null, null)]
    [InlineData("Could you show me an uppercase 'j'?", "ready", "upper", 106)]
    [InlineData("The letter 'w' should appear in upper case.", "ready", "upper", 119)]
    [InlineData("Capital-letter conversion for 'd', please.", "ready", "upper", 100)]
    [InlineData("Capitalize the input letter ø.", "ready", "upper", 248)]
    [InlineData("The single letter I am asking you to uppercase is m.", "ready", "upper", 109)]
    [InlineData("Upper case is wanted for the single character 'è'.", "ready", "upper", 232)]
    [InlineData("Please show the capital produced from 'ó'.", "ready", "upper", 243)]
    [InlineData("How is the letter 'î' written in uppercase?", "ready", "upper", 238)]
    [InlineData("What uppercase letter would I get from 'à'?", "ready", "upper", 224)]
    [InlineData("I would like the capital corresponding to 'é'.", "ready", "upper", 233)]
    [InlineData("The requested transformation of 'ö' is uppercasing.", "ready", "upper", 246)]
    [InlineData("My character for uppercase conversion is 'æ'.", "ready", "upper", 230)]
    [InlineData("Please use upper case when converting the input 'ï'.", "ready", "upper", 239)]
    [InlineData("Character for this task: 'ù'. Requested transformation: uppercase.", "ready", "upper", 249)]
    [InlineData("Given 'ê' as my one-character input, I request its capital.", "ready", "upper", 234)]
    [InlineData("For uppercase conversion, the input code point is U+0073.", "ready", "upper", 115)]
    [InlineData("My selected scalar has decimal code point 253 and needs uppercase.", "ready", "upper", 253)]
    [InlineData("Give me the capital for the character encoded by decimal code point 231.", "ready", "upper", 231)]
    [InlineData("For the input U+00FF, please select uppercase.", "ready", "upper", 255)]
    [InlineData("Use upper case on the Unicode code point U+00B5.", "ready", "upper", 181)]
    [InlineData("With 'ß' as the input character, perform an uppercase conversion.", "ready", "upper", 223)]
    [InlineData("Can you apply uppercase to the literal digit '1'?", "ready", "upper", 49)]
    [InlineData("The quoted input ':' is to be converted to upper case.", "ready", "upper", 58)]
    [InlineData("Apply an uppercase conversion to the symbol '±'.", "ready", "upper", 177)]
    [InlineData("Please uppercase the quoted nonbreaking space ' '.", "ready", "upper", 160)]
    [InlineData("The single input 'A' should receive uppercase conversion.", "ready", "upper", 65)]
    [InlineData("Could I see a lowercase version of 'J'?", "ready", "lower", 74)]
    [InlineData("Lower-case lettering is what I want for the character 'W'.", "ready", "lower", 87)]
    [InlineData("Small-letter conversion for 'C', please.", "ready", "lower", 67)]
    [InlineData("The single letter to lowercase is M.", "ready", "lower", 77)]
    [InlineData("I require 'È' in lower case.", "ready", "lower", 200)]
    [InlineData("Show me the small letter obtained from 'Ó'.", "ready", "lower", 211)]
    [InlineData("What lowercase letter matches the input 'Î'?", "ready", "lower", 206)]
    [InlineData("What would 'À' become in lower case?", "ready", "lower", 192)]
    [InlineData("The transformation requested for 'Ö' is lowercasing.", "ready", "lower", 214)]
    [InlineData("My lowercase-conversion input is the character 'Æ'.", "ready", "lower", 198)]
    [InlineData("For the input 'Ï', use a lowercase conversion.", "ready", "lower", 207)]
    [InlineData("Requested transformation: lowercase. Character for this task: 'Ù'.", "ready", "lower", 217)]
    [InlineData("Given the one-character input 'Ê', I want its small-letter counterpart.", "ready", "lower", 202)]
    [InlineData("The supplied character 'Ú' needs its case set to lower.", "ready", "lower", 218)]
    [InlineData("The input for lowercasing is Unicode code point U+0053.", "ready", "lower", 83)]
    [InlineData("What lowercase form comes from the scalar U+00D1?", "ready", "lower", 209)]
    [InlineData("My case choice is lowercase and the input code point is 0x00C5.", "ready", "lower", 197)]
    [InlineData("The letter 'n' is the input for this lowercase operation.", "ready", "lower", 110)]
    [InlineData("Please apply lowercase to the quoted digit '7'.", "ready", "lower", 55)]
    [InlineData("For the symbol '¢', please do a lowercase conversion.", "ready", "lower", 162)]
    [InlineData("The input is the quoted nonbreaking space ' '; apply lowercase.", "ready", "lower", 160)]
    [InlineData("Please return 'a' after applying the lowercase operation.", "ready", "lower", 97)]
    [InlineData("My preferred operation is to make a small letter.", "clarify", null, null)]
    [InlineData("My supplied input for conversion is 'ò'.", "clarify", null, null)]
    [InlineData("Here is my input code point: decimal 248.", "clarify", null, null)]
    [InlineData("The character I've selected is '%'.", "clarify", null, null)]
    [InlineData("Please lowercase 'D' or 'E'; either is a possible input.", "clarify", null, null)]
    [InlineData("I might want 'ç' in upper case or lower case.", "clarify", null, null)]
    [InlineData("My target case for code point U+00DC could be uppercase or lowercase.", "clarify", null, null)]
    [InlineData("I'd like 0008 made lowercase.", "clarify", null, null)]
    [InlineData("I'd like ~ made lowercase.", "clarify", null, null)]
    [InlineData("Render all of 'CAFÉ NOIR' in lowercase.", "abstain", null, null)]
    [InlineData("Put the two letters 'üa' into uppercase.", "abstain", null, null)]
    [InlineData("Use upper case for the control code point U+001B.", "abstain", null, null)]
    public void RelationshipsAndDescriptionsKeepTypedRoles(string text, string status, string? operation, int? key)
    {
        var proposal = LearningTaskParser.Propose(text);
        Assert.True(proposal.Status == status, $"TASK_RELATIONSHIP_RED {text}: {proposal.Status}, expected {status}");
        Assert.Equal(operation is null ? null : $"unicode17_{operation}_latin1", proposal.Dataset);
        Assert.Equal(key, proposal.Key is null ? null : (int?)proposal.Key.Value);
        Assert.Equal(status == "clarify", proposal.Prompt is not null);
    }
}
