using CnetControlPlane.Learning;
using Xunit;

namespace CnetControlPlane.Tests;

public sealed class LearningCaseRequestSyntaxTests
{
    [Fact]
    public void EveryLatin1LiteralRetainsItsByteAndControlsRemainRefused()
    {
        for (var value = 0; value <= 255; value++)
        foreach (var operation in new[] { "upper", "lower" })
        {
            var proposal = LearningTaskParser.Propose($"Could you kindly render '{(char)value}' in {operation}case!");
            Assert.Equal(char.IsControl((char)value) ? "abstain" : "ready", proposal.Status);
            Assert.Equal(char.IsControl((char)value) ? null : (byte?)value, proposal.Key);
            Assert.Equal(char.IsControl((char)value) ? null : $"unicode17_{operation}_latin1", proposal.Dataset);
        }
    }

    [Fact]
    public void BoundsApplyBeforeTrimmingAndEveryAdditionalActionRemainsUntrusted()
    {
        const string request = "Could you kindly write 'µ' in caps!";
        Assert.Equal("ready", LearningTaskParser.Propose(request.PadRight(256)).Status);
        Assert.Equal("abstain", LearningTaskParser.Propose(request.PadRight(257)).Status);
        foreach (var extra in new[] { " and email it", ", erase a file", " in Turkish", " without verification", " and uppercase it", " and lowercase it", "\u001f", "\ud800" })
        {
            var proposal = LearningTaskParser.Propose(request[..^1] + extra + "!");
            Assert.Equal("abstain", proposal.Status);
            Assert.Null(proposal.Dataset);
            Assert.Null(proposal.Key);
        }
    }

    // Authored component combinations are development tests, not blind evidence.
    public static IEnumerable<object[]> Requests()
    {
        string[] wrappers = ["", "Please, ", "Could you kindly ", "Would you please "];
        string[] descriptions = ["", "character ", "letter ", "the character ", "the single quoted character "];
        foreach (var wrapper in wrappers)
        foreach (var description in descriptions)
        foreach (var suffix in new[] { ".", "!", "?" })
        {
            yield return [wrapper + "uppercase " + description + " 'µ'" + suffix, "upper", (byte)181];
            yield return [wrapper + "lowercase " + description + " 'É'" + suffix, "lower", (byte)201];
            yield return [wrapper + "write " + description + "'ñ' in caps" + suffix, "upper", (byte)241];
            yield return [wrapper + "render " + description + "'À' in lowercase" + suffix, "lower", (byte)192];
            yield return [wrapper + "convert " + description + "'ö' to uppercase" + suffix, "upper", (byte)246];
        }
    }

    [Theory]
    [MemberData(nameof(Requests))]
    public void RequestConstituentsComposeWithoutChangingOriginalInput(string text, string operation, byte key)
    {
        var proposal = LearningTaskParser.Propose(text);
        Assert.True(proposal.Status == "ready", $"TASK_CONSTITUENT_RED {text}: {proposal.Status}");
        Assert.Equal("unicode17_" + operation + "_latin1", proposal.Dataset);
        Assert.Equal(key, proposal.Key);
        Assert.Null(proposal.Prompt);
    }

    [Theory]
    [InlineData("Could you kindly render the single quoted character in lowercase?", "clarify")]
    [InlineData("Please write 81 in caps.", "clarify")]
    [InlineData("Could you kindly uppercase 'a' or 'b'!", "clarify")]
    [InlineData("Please, uppercase my name!", "abstain")]
    [InlineData("What is the capital of France?", "abstain")]
    [InlineData("Please write 'ñ' in caps and email it.", "abstain")]
    [InlineData("Could you kindly render 'É' in lowercase, then email it?", "abstain")]
    [InlineData("Could you kindly render 'É' in lowercase in Turkish?", "abstain")]
    [InlineData("Could you kindly not render 'É' in lowercase?", "abstain")]
    [InlineData("Please, uppercase U+000A!", "abstain")]
    [InlineData("Please write 'μ' in caps.", "abstain")]
    [InlineData("Please write 'ab' in caps.", "abstain")]
    [InlineData("Please write 'a' in caps and caps.", "abstain")]
    [InlineData("Please show 'a' to be uppercase.", "abstain")]
    [InlineData("Please give 'a' the uppercase.", "abstain")]
    [InlineData("Please write the uppercase of France.", "clarify")]
    [InlineData("What is the uppercase of mu?", "clarify")]
    [InlineData("What is the lowercase of sigma?", "clarify")]
    [InlineData("What is the capital letter of mu?", "clarify")]
    [InlineData("What is the capital of the character mu?", "clarify")]
    [InlineData("What is the capital of the letter sigma?", "clarify")]
    [InlineData("render 'A' as small", "abstain")]
    [InlineData("display 'A' as small", "abstain")]
    [InlineData("change 'A' to small", "abstain")]
    public void ClaimedRequestsCannotDiscardUnknownActionsOrAcquireAuthority(string text, string status)
    {
        var proposal = LearningTaskParser.Propose(text);
        Assert.True(proposal.Status == status, $"TASK_CONSTITUENT_RED {text}: {proposal.Status}, expected {status}");
        Assert.Null(proposal.Dataset);
        Assert.Null(proposal.Key);
    }

    [Theory]
    [InlineData("equivalent")]
    [InlineData("counterpart")]
    [InlineData("letter")]
    [InlineData("character")]
    [InlineData("result")]
    [InlineData("case conversion")]
    [InlineData("casing")]
    [InlineData("rendition")]
    [InlineData("lettering")]
    public void NominalCompatibilityRequestsAreNotClaimedAsUnaryOperands(string noun)
    {
        var proposal = LearningTaskParser.Propose($"uppercase {noun} for 'a'");
        Assert.True(proposal.Status == "ready", $"TASK_CONSTITUENT_REVIEW_RED {noun}: {proposal.Status}");
        Assert.Equal("unicode17_upper_latin1", proposal.Dataset);
        Assert.Equal((byte)97, proposal.Key);
    }

    [Theory]
    [InlineData("uppercase character is wanted for 'a'")]
    [InlineData("uppercase letter is the desired case for 'a'")]
    [InlineData("uppercase character when converting 'a'")]
    [InlineData("uppercase letter is to be applied to 'a'")]
    [InlineData("uppercase character should take 'a' as its input")]
    [InlineData("uppercase character is the requested operation for 'a'")]
    public void NominalDeclarationsKeepTheirExistingOwner(string text)
    {
        var proposal = LearningTaskParser.Propose(text);
        Assert.True(proposal.Status == "ready", $"TASK_CONSTITUENT_REVIEW3_RED {text}: {proposal.Status}");
        Assert.Equal("unicode17_upper_latin1", proposal.Dataset);
        Assert.Equal((byte)97, proposal.Key);
    }

    [Theory]
    [InlineData("Could you kindly write ' ' in caps!", 32)]
    [InlineData("Could you kindly write '!' in caps!", 33)]
    [InlineData("Could you kindly write '.' in caps!", 46)]
    [InlineData("Could you kindly write ''' in caps!", 39)]
    [InlineData("Please, uppercase code  point   181!", 181)]
    [InlineData("Please, uppercase 0x00B5!", 181)]
    [InlineData("uppercase character: 'a'", 97)]
    public void GrammarSpacingAndPunctuationDoNotNormalizeLiteralData(string text, byte key)
    {
        var proposal = LearningTaskParser.Propose(text);
        Assert.True(proposal.Status == "ready", $"TASK_CONSTITUENT_RED {text}: {proposal.Status}");
        Assert.Equal(key, proposal.Key);
        Assert.Equal("unicode17_upper_latin1", proposal.Dataset);
    }
}
