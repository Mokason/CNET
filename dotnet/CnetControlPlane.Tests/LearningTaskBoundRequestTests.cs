using CnetControlPlane.Learning;
using Xunit;

namespace CnetControlPlane.Tests;

// Independently combined component tests, synthetic development checks only.
public sealed class LearningTaskBoundRequestTests
{
    public static IEnumerable<object[]> Combinations()
    {
        var inputs = new (string Declaration, int Key)[] {
            ("Input character: 'Q'; ", 81),
            ("With the supplied Unicode scalar U+0051 as input, ", 81),
            ("For the quoted symbol '?', ", 63),
            ("Given the single quoted character 'É' as input, ", 201),
            ("The input code point is 0x0051 in hexadecimal; ", 81),
            ("The chosen character is 'µ'; ", 181),
            ("My input character for this task is 'Q'; ", 81),
            ("The literal character I'm giving you is 'Q'; ", 81),
            ("The selected input has decimal code point 81; ", 81),
            ("I have supplied Unicode code point U+0051; ", 81)
        };
        var requests = new[] {
            "show its {0} counterpart.",
            "could you show its {0} counterpart?",
            "please show its {0} counterpart.",
            "would you please give its {0} form?",
            "I would like to see its {0} form.",
            "may I have its {0} counterpart?",
            "I need it converted to {0}.",
            "could you put it in {0}?",
            "could you produce its {0} version?"
        };
        foreach (var input in inputs)
        foreach (var request in requests)
        foreach (var operation in new[] { "upper", "lower" })
            yield return new object[] { input.Declaration + string.Format(request, operation + "case"), operation, input.Key };
    }

    [Theory]
    [MemberData(nameof(Combinations))]
    [InlineData("Use code point 0x54, in hexadecimal, lowercase it.", "lower", 84)]
    [InlineData("Input: 'T'; could you lowercase that character?", "lower", 84)]
    [InlineData("Input: 'T'; could you show me its lowercase counterpart?", "lower", 84)]
    [InlineData("Input: 'T'; display its lowercase form.", "lower", 84)]
    public void IndependentInputsAndRequestsCompose(string text, string operation, int key)
    {
        var proposal = LearningTaskParser.Propose(text);
        Assert.True(proposal.Status == "ready", $"BOUND_REQUEST_RED {text}: {proposal.Status}");
        Assert.Equal($"unicode17_{operation}_latin1", proposal.Dataset);
        Assert.Equal((byte)key, proposal.Key);
        Assert.Null(proposal.Prompt);
    }

    [Theory]
    [InlineData("Use code point 0x54, in hexadecimal, lowercase it; run code.", "abstain")]
    [InlineData("Use decimal code point 0x54, in hexadecimal, lowercase it.", "abstain")]
    [InlineData("Use code point 0x85, in hexadecimal, lowercase it.", "abstain")]
    [InlineData("Input: 'T'; display its lowercase form in hexadecimal.", "abstain")]
    [InlineData("Input: 'T'; could you lowercase the literal that character?", "abstain")]
    [InlineData("Input: 'A'; uppercase the literal it.", "abstain")]
    [InlineData("Input: 82; uppercase it and open a file.", "abstain")]
    [InlineData("Input: 82; show it to be a lowercase character.", "abstain")]
    [InlineData("For uppercase conversion, lowercase it.", "abstain")]
    [InlineData("Input: 'A'; the input is 'B' and lowercase it.", "abstain")]
    [InlineData("Input: 'A'; unicode lower 66.", "abstain")]
    [InlineData("Input: 'Q'; show its lowercase result to 'P'.", "abstain")]
    [InlineData("Input: 'Q'; give 'P' its lowercase result.", "abstain")]
    [InlineData("Input: 'Q'; show its lowercase result in hexadecimal.", "abstain")]
    [InlineData("Input: 'Q'; uppercase it; run a shell.", "abstain")]
    [InlineData("Input: 'Q'; input: 'P'; lowercase it.", "abstain")]
    [InlineData("Input: 'Q'; make 'P' lowercase and show it.", "abstain")]
    [InlineData("Input: 'Q'; show it to be a lowercase character.", "abstain")]
    [InlineData("Input: 'Q'; could it be lowercase?", "clarify")]
    [InlineData("Input: U+0100; could you show its lowercase counterpart?", "abstain")]
    [InlineData("Input: decimal code point 0x51; could you show its lowercase counterpart?", "abstain")]
    [InlineData("Input: 'Q' or 'P'; could you show its lowercase counterpart?", "clarify")]
    [InlineData("Input: 81; could you show its lowercase counterpart?", "clarify")]
    [InlineData("Input: \"Q'; could you show its lowercase counterpart?", "clarify")]
    [InlineData("Could you show its lowercase counterpart?", "clarify")]
    [InlineData("I would like to see its uppercase form.", "clarify")]
    [InlineData("For uppercase conversion, the input is 'q'.", "ready")]
    public void BindingCannotCreateAnInputOrDiscardAnotherRole(string text, string status)
    {
        var proposal = LearningTaskParser.Propose(text);
        Assert.True(proposal.Status == status, $"BOUND_ROLE_RED {text}: {proposal.Status}, expected {status}");
        if (status == "ready")
        {
            Assert.Equal("unicode17_upper_latin1", proposal.Dataset);
            Assert.Equal((byte)113, proposal.Key);
        }
        else
        {
            Assert.Null(proposal.Dataset);
            Assert.Null(proposal.Key);
        }
    }
}
