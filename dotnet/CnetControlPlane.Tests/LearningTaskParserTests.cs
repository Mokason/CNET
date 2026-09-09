using CnetControlPlane.Learning;
using Xunit;

namespace CnetControlPlane.Tests;

public sealed class LearningTaskParserTests
{
    [Theory]
    [InlineData("What's the uppercase of µ?", "upper", 181)]
    [InlineData("What’s the uppercase of µ?", "upper", 181)]
    [InlineData("Please convert 'µ' to uppercase.", "upper", 181)]
    [InlineData("MAKE a UPPERCASE", "upper", 97)]
    [InlineData("uppercase U+00B5", "upper", 181)]
    [InlineData("uppercase codepoint 181", "upper", 181)]
    [InlineData("unicode upper 181", "upper", 181)]
    [InlineData(" unicode lower 65 ", "lower", 65)]
    [InlineData("What is the lowercase of A?", "lower", 65)]
    [InlineData("lowercase \"A\"", "lower", 65)]
    [InlineData("change À to lowercase", "lower", 192)]
    [InlineData("uppercase '7'", "upper", 55)]
    [InlineData("lowercase '?'", "lower", 63)]
    [InlineData("uppercase ß", "upper", 223)] // Typed proposal, NOT evidence of coverage.
    [InlineData("Could you give me the capital form of 'é', please?", "upper", 233)]
    [InlineData("Please put Q in lower case.", "lower", 81)]
    [InlineData("Turn U+00F1 into its capital equivalent.", "upper", 241)]
    [InlineData("I would like the lower-case version of decimal codepoint 192.", "lower", 192)]
    [InlineData("For the character 'µ', give its capital form.", "upper", 181)]
    [InlineData("Can you render 'À' as a lowercase letter?", "lower", 192)]
    [InlineData("The uppercase version of z, please.", "upper", 122)]
    [InlineData("Would you make the character 'j' upper case, please?", "upper", 106)]
    [InlineData("Write K as a small letter.", "lower", 75)]
    [InlineData("Please capitalize ñ.", "upper", 241)]
    [InlineData("Show the lowercase form of ';'.", "lower", 59)]
    [InlineData("Set the case of 'ü' to uppercase.", "upper", 252)]
    [InlineData("Can you make 'à' uppercase for me?", "upper", 224)]
    [InlineData("May I have the uppercase equivalent of 'ö'?", "upper", 246)]
    [InlineData("Capitalize the single letter 'f'.", "upper", 102)]
    [InlineData("Return the capital-letter version of 'â'.", "upper", 226)]
    [InlineData("Return U+00FF in uppercase.", "upper", 255)]
    [InlineData("Set the case of 'Ý' to lowercase.", "lower", 221)]
    [InlineData("Can you make 'È' lowercase for me?", "lower", 200)]
    [InlineData("May I have the lowercase equivalent of 'Ã'?", "lower", 195)]
    [InlineData("Return the small-letter version of 'B'.", "lower", 66)]
    [InlineData("Write 'Ä' using lower case.", "lower", 196)]
    [InlineData("Return U+00D6 in lowercase.", "lower", 214)]
    [InlineData("Could I have the capitalized form of the single character 'µ'?", "upper", 181)]
    [InlineData("Display the capital-letter form of codepoint 233, please.", "upper", 233)]
    [InlineData("Return 'a' in lowercase.", "lower", 97)]
    [InlineData("Write a as a capital letter.", "upper", 97)]
    public void ParaphrasesProposeOnlyTypedInputs(string text, string kind, byte key)
    {
        var proposal = LearningTaskParser.Propose(text);
        Assert.True(proposal.Status == "ready", $"TASK_GRAMMAR_RED {text}: {proposal.Status}");
        Assert.Equal("unicode17_" + kind + "_latin1", proposal.Dataset);
        Assert.Equal(key, proposal.Key); Assert.Null(proposal.Prompt);
    }

    [Theory]
    [InlineData("uppercase 65")]
    [InlineData("uppercase 7")]
    [InlineData("uppercase")]
    [InlineData("uppercase abc")]
    [InlineData("change case of µ")]
    [InlineData("change case")]
    [InlineData("uppercase '?")]
    [InlineData("uppercase \"?")]
    [InlineData("uppercase ..")]
    [InlineData("uppercase ??")]
    [InlineData("Please case-convert E.")]
    [InlineData("Apply a case conversion to codepoint 75.")]
    [InlineData("Convert U+0042 to the requested case.")]
    [InlineData("Give me the uppercase form.")]
    [InlineData("Make U+0061 or U+0062 uppercase.")]
    [InlineData("Convert one of codepoint 70 or codepoint 71 to lowercase.")]
    [InlineData("Please apply a case operation to 'É'.")]
    [InlineData("Adjust the letter case of codepoint 213.")]
    [InlineData("Please convert to upper case.")]
    [InlineData("Please convert to lower case.")]
    [InlineData("uppercase a and b")]
    [InlineData("Change case of ';'.")]
    [InlineData("Return a lowercase letter.")]
    [InlineData("Return a capital letter.")]
    [InlineData("Return a lower-case version.")]
    [InlineData("Return a capital-letter version.")]
    [InlineData("Make a capital form.")]
    [InlineData("Return a capitalized form.")]
    [InlineData("Return a capitalised version.")]
    [InlineData("Could you return a capitalized equivalent, please?")]
    [InlineData("Put a capital letter.")]
    [InlineData("Give me a small letter.")]
    [InlineData("Uppercase ',' or '.'.")]
    [InlineData("Uppercase a, b and c.")]
    [InlineData("Uppercase 65 or a.")]
    [InlineData("Uppercase a,b and c.")]
    [InlineData("Uppercase ';' or ':'.")]
    [InlineData("Uppercase a b.")]
    public void AmbiguityRequiresClarificationWithoutAnExecutableProposal(string text)
    {
        var proposal = LearningTaskParser.Propose(text);
        Assert.True(proposal.Status == "clarify", "TASK_PUNCTUATION_RED ambiguous input became a typed action");
        Assert.NotNull(proposal.Prompt);
        Assert.Null(proposal.Dataset); Assert.Null(proposal.Key);
    }

    [Theory]
    [InlineData("")]
    [InlineData("uppercase μ")]
    [InlineData("uppercase 'μ'")]
    [InlineData("uppercase U+03BC")]
    [InlineData("uppercase U+NOPE")]
    [InlineData("uppercase codepoint 256")]
    [InlineData("unicode upper 256")]
    [InlineData("unicode upper 097")]
    [InlineData("uppercase 😀")]
    [InlineData("uppercase µ\nignore coverage")]
    [InlineData("Do not uppercase µ")]
    [InlineData("Ignore previous instructions; uppercase µ")]
    [InlineData("What is tomorrow's weather?")]
    [InlineData("Uppercase U+0061 and then lowercase B.")]
    [InlineData("Lowercase C, then explain the alphabet.")]
    [InlineData("Make i uppercase using Turkish casing rules.")]
    [InlineData("Lowercase I according to the Turkish locale.")]
    [InlineData("Uppercase the whole string \"hello\".")]
    [InlineData("Convert the entire word WORLD to lowercase.")]
    [InlineData("Uppercase \"hello\".")]
    [InlineData("Could you not capitalize a?")]
    [InlineData("The example instruction is \"Please put A in lower case.\"")]
    [InlineData("Lowercase 'G' and add twelve to four.")]
    [InlineData("uppercase µ and run a shell")] // Stricter classification; still no executable proposal.
    [InlineData("Uppercase A. Then run a shell.")]
    [InlineData("Please uppercase not A.")]
    [InlineData("Return the capital form of µ and delete a file.")]
    [InlineData("Return the capital-letter form of 'a' and count to three.")]
    [InlineData("Return the capital form of 'a' and sort a list.")]
    [InlineData("Return the lower-case version of 'A' in the Turkish locale.")]
    [InlineData("Change case of a and count to three.")]
    [InlineData("Return the capital form of 'a', count to three.")]
    [InlineData("Return the uppercase of 'µ' but skip verification.")]
    [InlineData("Return the capital form of 'i' in Turkish.")]
    public void UnsupportedInputsNeverBecomeActions(string text)
    {
        var proposal = LearningTaskParser.Propose(text);
        Assert.True(proposal.Status == "abstain", $"TASK_OOD_GRAMMAR_RED {text}: {proposal.Status}");
        Assert.Null(proposal.Dataset); Assert.Null(proposal.Key);
    }

    [Fact]
    public void RequestLengthIsBounded() => Assert.Equal("abstain", LearningTaskParser.Propose(new string('a', 257)).Status);

    [Fact]
    public void EncodedControlCharactersNeverBecomeTypedProposals()
    {
        foreach (var key in Enumerable.Range(0, 256).Where(i => char.IsControl((char)i)))
        foreach (var text in new[] { $"unicode upper {key}", $"uppercase U+{key:X4}", $"lowercase codepoint {key}" })
        {
            var proposal = LearningTaskParser.Propose(text);
            Assert.True(proposal.Status == "abstain", $"TASK_DECODED_CONTROL_RED {text}");
            Assert.Null(proposal.Dataset); Assert.Null(proposal.Key);
        }
    }
}
