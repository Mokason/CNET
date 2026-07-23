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
    [InlineData("2^2.5")]                        // fractional exponent (decimal path)
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

    // ─────────────── big-integer path (no exponent cap, no 28-digit ceiling) ───────────────

    [Theory]
    [InlineData("What is 2 to the power of 127 minus 1?", "170141183460469231731687303715884105727")]
    [InlineData("2^127 - 1", "170141183460469231731687303715884105727")]
    [InlineData("What is 123456789 * 987654321?", "121932631112635269")]
    [InlineData("2^64", "18446744073709551616")]
    [InlineData("9^999", "194207916858072401073330513240517841169895831937243168645765334645631807358586165476831829984964567897289883410682808509863485381763945405279379355788182053541434708898886353264614403164257835946591015853500491562156765579388944516423770646547300211711400609344237550775485394558425026601257627110879613741893863295847627378504481736441703291029360564416718984718052676789493826372811349572386149787861703350363229770343522164432121091627871310618608734044108407173015970850780786711471108639762810760748899301375323974504010469298672123113693793242558662498267897607159946316136440215024585534972601864730717278590674861331708227340510282977338127859756479389076075528672989549862138485404935127984793120586289288424045660573066638008624179879066798350622453419082976217706653276687992598885030141711458658381360884807741768071789239593772708382532520992894115725948613681993478965648216640862698897925988931145600683858128653568049999074868783790048889")]
    [InlineData("999999999999 * 999999999999", "999999999998000000000001")]
    public void BigInteger_ComputesExactly_BeyondDecimal(string prompt, string expected)
    {
        Assert.True(ExactArithmetic.TryAnswer(prompt, out string answer), prompt);
        Assert.Equal(expected, answer);
    }

    [Fact]
    public void BigInteger_HugeExponent_Declines_NotHangs()
    {
        // Beyond the 100000 exponent bound: declines rather than blowing memory.
        Assert.False(ExactArithmetic.TryAnswer("2^999999999", out _));
    }

    [Fact]
    public void Fractions_StillUseTheDecimalPath()
    {
        Assert.True(ExactArithmetic.TryAnswer("100 / 8", out string a));
        Assert.Equal("12.5", a);
        Assert.False(ExactArithmetic.TryAnswer("1 / 3", out _));   // non-terminating still declines
    }

    // ─────────────── integer square root and friends ───────────────

    [Theory]
    [InlineData("floor(sqrt(2^127 - 1))", "13043817825332782212")]   // Hermes's question
    [InlineData("isqrt(2^127 - 1)", "13043817825332782212")]
    [InlineData("sqrt(20736)", "144")]                               // perfect square: exact
    [InlineData("isqrt(1000)", "31")]
    [InlineData("abs(0 - 4827)", "4827")]
    [InlineData("floor(144)", "144")]                                // floor of an integer
    public void IntegerFunctions_ComputeExactly(string prompt, string expected)
    {
        Assert.True(ExactArithmetic.TryAnswer(prompt, out string answer), prompt);
        Assert.Equal(expected, answer);
    }

    [Theory]
    [InlineData("sqrt(2)")]          // irrational: exact sqrt declines (isqrt would not)
    [InlineData("sqrt(20735)")]      // not a perfect square
    [InlineData("bogus(5)")]         // unknown function
    [InlineData("2 apples")]         // letters allowed by whitelist, still declines on residual
    public void IntegerFunctions_DeclineHonestly(string prompt) =>
        Assert.False(ExactArithmetic.TryAnswer(prompt, out _));
}
