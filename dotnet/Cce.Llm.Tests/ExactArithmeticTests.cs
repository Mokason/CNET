using CNET.Cce.Llm.Verify;
using Xunit;

namespace CNET.Cce.Llm.Tests;

/// <summary>
/// The exact-arithmetic lane: AICIMO's regression corpus (computable and
/// decline cases carried over), plus the one deliberate tightening — division
/// must terminate or decline. The invariant under test everywhere: a wrong
/// exact answer is worse than declining.
/// </summary>
public sealed class ExactArithmeticTests
{
    // ── AICIMO's computable corpus ──
    [Theory]
    [InlineData("47 times 89", "4183")]
    [InlineData("128+256+512", "896")]
    [InlineData("what is 2^10", "1024")]
    [InlineData("(2+3)*4", "20")]
    [InlineData("100/8", "12.5")]
    [InlineData("1,234+1", "1235")]
    [InlineData("6.5*4", "26")]
    [InlineData("2 to the power of 8", "256")]
    [InlineData("what is the value of 17 minus 5", "12")]
    [InlineData("calculate 10 modulo 3", "1")]
    [InlineData("how much is 9 plus 1?", "10")]
    [InlineData("compute -4 * -5", "20")]
    public void ComputableExpressions_AnswerExactly(string prompt, string expected)
    {
        Assert.True(ExactArithmetic.TryAnswer(prompt, out string answer), prompt);
        Assert.Equal(expected, answer);
    }

    // ── AICIMO's decline corpus + the tightening ──
    [Theory]
    [InlineData("47 times 89 apples")]           // residual text: the load-bearing decline
    [InlineData("1/0")]
    [InlineData("10%0")]
    [InlineData("2+")]                           // dangling operator
    [InlineData("(2+3")]                         // unbalanced paren
    [InlineData("1,2 + 1")]                      // malformed thousands grouping
    [InlineData("2^2.5")]                        // fractional exponent
    [InlineData("9^999")]                        // exponent out of range
    [InlineData("79228162514264337593543950335 * 10")]   // decimal overflow
    [InlineData("what is the capital of france")]
    [InlineData("two plus two")]                 // no digits
    [InlineData("")]
    [InlineData("1/3")]                          // tightened: non-terminating != exact
    [InlineData("10/3")]
    public void NonExactPrompts_Decline(string prompt) =>
        Assert.False(ExactArithmetic.TryAnswer(prompt, out _));

    [Theory]
    [InlineData("what is what's 2+2?", "2+2")]   // iterative lead-in shedding
    [InlineData("calculate 3 multiplied by 4 =", "3 * 4")]
    [InlineData("SOLVE 5 MOD 2.", "5 % 2")]
    public void Normalize_StripsPhrasingToExpression(string prompt, string expected) =>
        Assert.Equal(expected, ExactArithmetic.Normalize(prompt));

    [Theory]
    [InlineData("12.500", "12.5")]
    [InlineData("0.000", "0")]
    [InlineData("-0.0", "0")]
    [InlineData("100", "100")]
    public void Render_IsCanonical(string raw, string expected) =>
        Assert.Equal(expected, ExactArithmetic.Render(decimal.Parse(raw,
            System.Globalization.CultureInfo.InvariantCulture)));

    /// <summary>Terminating division still answers — the tightening only
    /// declines what decimal would silently round.</summary>
    [Theory]
    [InlineData("1/4", "0.25")]
    [InlineData("7/8", "0.875")]
    [InlineData("22/11", "2")]
    public void TerminatingDivision_StillExact(string prompt, string expected)
    {
        Assert.True(ExactArithmetic.TryAnswer(prompt, out string answer));
        Assert.Equal(expected, answer);
    }
}
