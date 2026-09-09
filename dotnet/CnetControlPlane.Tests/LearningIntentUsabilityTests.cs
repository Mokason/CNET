using CnetControlPlane.Learning;
using Xunit;

namespace CnetControlPlane.Tests;

public sealed class LearningIntentUsabilityTests
{
    [Theory]
    [InlineData("Use capital-letter case for f.", "ready", "upper", 102)]
    [InlineData("Convert this quoted space to uppercase: ' '.", "ready", "upper", 32)]
    [InlineData("For the letter A, I'd like its lowercase form.", "ready", "lower", 65)]
    [InlineData("Use small-letter case for X.", "ready", "lower", 88)]
    [InlineData("Apply lowercase to this space character: \" \".", "ready", "lower", 32)]
    [InlineData("Convert decimal code point 81 to the desired case.", "clarify", null, 0)]
    [InlineData("Make one character lowercase; choose between U+0053 and U+0055.", "clarify", null, 0)]
    [InlineData("Uppercase the space shown as U+0020.", "ready", "upper", 32)]
    [InlineData("Uppercase the hyphen shown as U+002D.", "ready", "upper", 45)]
    [InlineData("Convert this quoted space to uppercase: the character shown as U+0020.", "ready", "upper", 32)]
    [InlineData("Uppercase the space shown as U+0020 or U+0058.", "clarify", null, 0)]
    [InlineData("recode the glyph 'q' or 'q' toward lowercase", "clarify", null, 0)]
    [InlineData("recode the glyph 'P' toward letters", "clarify", null, 0)]
    public void ExposedRequestsUseSharedRoles(string text, string status, string? operation, int key) =>
        Check(text, status, operation, key);

    [Fact]
    public void NewRolesComposeAcrossInputsAndDirections()
    {
        foreach (var (op, adjective) in new[] { ("upper", "capital"), ("lower", "small") })
        foreach (var scalar in new[] { 'B', 'q', 'é' })
        {
            Check($"Use {adjective}-letter case for '{scalar}'.", "ready", op, scalar);
            Check($"For the letter '{scalar}', I'd like its {op}case form.", "ready", op, scalar);
        }
        foreach (var direction in new[] { "uppercase", "lowercase" })
        {
            Check($"Convert this quoted space to {direction}: ' '.", "ready", direction[..^4], 32);
            Check($"Apply {direction} to this space character: ' '.", "ready", direction[..^4], 32);
            Check($"Make one character {direction}; choose between 'B' and 'C'.", "clarify");
        }
        foreach (var scalar in new[] { 65, 80, 233 })
            Check($"Convert decimal code point {scalar} to the desired case.", "clarify");
    }

    [Theory]
    [InlineData("Use capital-letter case for U+0218.")]
    [InlineData("Use small-letter case for 'P' and erase it.")]
    [InlineData("For the letter U+000A, I'd like its uppercase form.")]
    [InlineData("For the letter 'P', I'd like its uppercase form; erase it.")]
    [InlineData("Convert this quoted space to uppercase: ' ' and erase it.")]
    [InlineData("Apply lowercase to this space character: ' ' using Turkish rules.")]
    [InlineData("Convert decimal code point 522 to the desired case.")]
    [InlineData("Convert decimal code point 10 to the desired case.")]
    [InlineData("Convert decimal code point 81 to the desired case; erase it.")]
    [InlineData("Make one character lowercase; choose between 'P' and U+0218.")]
    [InlineData("Make one character lowercase; choose between 'P' and U+000A.")]
    [InlineData("Make one character lowercase; choose between 'P' and 'Q'; erase it.")]
    [InlineData("Make one character lowercase; choose between the word hello and 'Q'.")]
    [InlineData("Convert this quoted space to uppercase: delete ' '.")]
    [InlineData("Convert this quoted space to uppercase: lowercase U+0218.")]
    [InlineData("Apply lowercase to this space character: erase it.")]
    [InlineData("Apply lowercase to this space character: delete ' '.")]
    [InlineData("recode the glyph q toward lowercase qqq")]
    [InlineData("Kindly recode the glyph q toward small letters qqq.")]
    [InlineData("recode the glyph 'P' toward small letters, switch it to uppercase")]
    [InlineData("recode the glyph 'P' toward small letters, change it to capitals")]
    [InlineData("recode the glyph 'P' toward small letters, rewrite it using capitals")]
    [InlineData("recode the glyph q toward lowercase, uppercase")]
    [InlineData("recode the glyph 'P' toward small letters, show it")]
    [InlineData("recode the glyph q toward lowercase or caps")]
    [InlineData("recode the glyph 'P' toward small letters in caps")]
    [InlineData("recode the glyph 'q', 'q' toward lowercase")]
    [InlineData("recode the glyph q q toward lowercase")]
    public void NewRolesDoNotHideForbiddenDataOrActions(string text) => Check(text, "abstain");

    [Theory]
    [InlineData("Apply lowercase to this space character: 'X'.")]
    [InlineData("Convert this quoted space to uppercase: 'X'.")]
    public void ADescriptorCannotOverrideItsActualScalar(string text) => Check(text, "clarify");

    private static void Check(string text, string status, string? operation = null, int key = 0)
    {
        var p = LearningTaskParser.Propose(text);
        Assert.True(p.Status == status, $"INTENT_USABILITY_RED {text}: {p.Status}/{p.Code}, expected {status}");
        Assert.Equal(operation is null ? null : "unicode17_" + operation + "_latin1", p.Dataset);
        Assert.Equal(operation is null ? null : (byte?)key, p.Key);
    }
}
