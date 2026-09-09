using CnetControlPlane.Learning;
using Xunit;

namespace CnetControlPlane.Tests;

// The third Hermes confirmation is exposed development evidence. Verb forms
// compose at grammatical positions; operand spelling must never be normalized.
public sealed class LearningCaseVerbCompositionTests
{
    [Theory]
    [InlineData("Bring R down to lower case.", "lower", 82)]
    [InlineData("Change \"ñ\" so it is written in capitals.", "upper", 241)]
    [InlineData("Take 0xC9 and switch it to lower case.", "lower", 201)]
    [InlineData("Would you mind changing P to lowercase?", "lower", 80)]
    [InlineData("Rewrite 'Æ' using small letters.", "lower", 198)]
    [InlineData("Show me letter V after converting it to lower case.", "lower", 86)]
    [InlineData("Would you mind converting ' ' to upper case?", "upper", 32)]
    [InlineData("Could you uncapitalize '~'?", "lower", 126)]
    [InlineData("Map U+00E4 onto its uppercase counterpart.", "upper", 228)]
    [InlineData("Uncapitalize \"T\".", "lower", 84)]
    [InlineData("Would you mind giving me the uppercase of f?", "upper", 102)]
    [InlineData("I need U+00E0 mapped to uppercase.", "upper", 224)]
    public void SharedVerbRolesPreserveTheOriginalScalar(string text, string operation, byte key)
        => Ready(text, operation, key);

    public static IEnumerable<object[]> VerbForms()
    {
        var conversions = new[] { "convert", "change", "turn", "switch", "transform", "map", "fold", "rewrite", "recast", "take", "bring", "raise" };
        var conversionGerunds = new[] { "converting", "changing", "turning", "switching", "transforming", "mapping", "folding", "rewriting", "recasting", "taking", "bringing", "raising" };
        for (var index = 0; index < conversions.Length; index++)
        {
            var operation = conversions[index] == "raise" ? "upper" : "lower";
            yield return [$"Would you mind {conversionGerunds[index]} 'P' to {operation}case?", operation, (byte)80];
            yield return [$"Show me 'P' after {conversionGerunds[index]} it to {operation}case.", operation, (byte)80];
        }
        foreach (var output in new[] { "making", "putting", "writing", "rendering", "returning", "showing", "displaying", "supplying", "providing", "producing", "presenting", "expressing", "giving" })
            yield return [$"Would you mind {output} 'A' in lowercase?", "lower", (byte)65];
    }

    [Theory]
    [MemberData(nameof(VerbForms))]
    public void VerbInflectionsComposeWithoutChangingOperandText(string text, string operation, byte key)
        => Ready(text, operation, key);

    private static void Ready(string text, string operation, byte key)
    {
        var proposal = LearningTaskParser.Propose(text);
        Assert.True(proposal.Status == "ready", $"TASK_VERB_RED {text}: {proposal.Status}");
        Assert.Equal("unicode17_" + operation + "_latin1", proposal.Dataset);
        Assert.Equal(key, proposal.Key);
    }

    [Theory]
    [InlineData("Which should I lowercase, F or G?", "clarify")]
    [InlineData("Take the character k and convert it to the other case.", "clarify")]
    [InlineData("Do a case change on w.", "clarify")]
    [InlineData("Take 204 and convert it to uppercase.", "clarify")]
    [InlineData("Which should I uppercase, F?", "clarify")]
    [InlineData("Which should I lowercase, F or U+000A?", "abstain")]
    [InlineData("Do a case change on U+0218.", "abstain")]
    [InlineData("Take U+0218 and convert it to the other case.", "abstain")]
    [InlineData("Bring 'A' up to lowercase.", "clarify")]
    [InlineData("Show 'A' after raising it to lowercase.", "clarify")]
    [InlineData("Raise 'a' after converting it to lowercase.", "abstain")]
    [InlineData("Would you mind raising U+0218 to lowercase?", "abstain")]
    [InlineData("Show 'A' after converting 'B' to lowercase.", "abstain")]
    [InlineData("Show 'A' before converting it to lowercase.", "abstain")]
    [InlineData("Show 'A' after converting it to lowercase and deleting it.", "abstain")]
    [InlineData("Take 'A' and switch it to lowercase and send it.", "abstain")]
    [InlineData("Would you mind converting the word hello to uppercase?", "abstain")]
    [InlineData("Map 'A' onto its uppercase counterpart using Turkish rules.", "abstain")]
    [InlineData("Change 'A' so it is written in capitals and sent away.", "abstain")]
    [InlineData("Uncapitalize U+000A.", "abstain")]
    [InlineData("Uncapitalize 'AB'.", "abstain")]
    [InlineData("Rewrite 'A' using small.", "abstain")]
    [InlineData("Would you mind giving 'A' to 'B'?", "abstain")]
    public void ExtraActionsAndUnresolvedRolesNeverBecomeReady(string text, string expected)
    {
        var proposal = LearningTaskParser.Propose(text);
        Assert.True(proposal.Status == expected, $"TASK_VERB_RED {text}: {proposal.Status}, expected {expected}");
        Assert.Null(proposal.Dataset);
        Assert.Null(proposal.Key);
    }
}
