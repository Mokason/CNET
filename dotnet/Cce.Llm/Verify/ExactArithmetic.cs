using System.Numerics;

namespace CNET.Cce.Llm.Verify;

/// <summary>
/// The exact-arithmetic lane, ported from AICIMO (SkillRouterDriver, commit
/// 5f7b374 — measured there: escalation 90%→30%, answers in 0.2–9 ms vs
/// seconds of heavy decode). Arithmetic questions are answered by computation,
/// not generation; everything else declines and falls through to the model.
/// </summary>
/// <remarks>
/// The load-bearing design rule, preserved verbatim from AICIMO: <b>a wrong
/// exact answer is worse than escalating</b>. Hence:
///  - character whitelist after normalization — any unexpected byte declines;
///  - full-consumption parse — "47 times 89 apples" declines (residual text),
///    it does not answer 4183;
///  - every exception path (overflow, divide-by-zero, malformed grouping,
///    fractional/oversized exponents) declines;
///  - one deliberate tightening over the original: division must TERMINATE
///    (reduced denominator of 2^a·5^b — decided by factor arithmetic, since
///    decimal's own rounding defeats a naive round-trip check), so 100/8
///    answers 12.5 but 1/3 declines instead of serving 28 rounded digits
///    labeled "exact".
/// This is rung 2's first verifier: truth by construction, no model opinion.
/// </remarks>
public static class ExactArithmetic
{
    private static readonly (string Phrase, string Op)[] OperatorWords =
    [
        ("multiplied by", "*"), ("divided by", "/"), ("to the power of", "^"),
        ("times", "*"), ("plus", "+"), ("minus", "-"), ("modulo", "%"), ("mod ", "% "),
    ];

    private static readonly string[] LeadIns =
    [
        "what is the value of", "what is", "what's", "how much is",
        "calculate", "compute", "evaluate", "solve",
    ];

    /// <summary>
    /// Strips question phrasing down to a candidate expression. Iterative:
    /// "calculate what is 2+2?" sheds both lead-ins.
    /// </summary>
    public static string Normalize(string prompt)
    {
        string s = prompt.Trim().ToLowerInvariant();
        bool changed = true;
        while (changed)
        {
            changed = false;
            foreach (string lead in LeadIns)
            {
                if (s.StartsWith(lead, StringComparison.Ordinal))
                {
                    s = s[lead.Length..].TrimStart();
                    changed = true;
                }
            }
        }
        while (s.Length > 0 && (s[^1] == '?' || s[^1] == '=' || s[^1] == '.'))
            s = s[..^1].TrimEnd();
        foreach ((string phrase, string op) in OperatorWords)
            s = s.Replace(phrase, op, StringComparison.Ordinal);
        return s;
    }

    /// <summary>
    /// Answers exactly, or declines. Declining is the common case and the
    /// correct one for anything that is not purely arithmetic.
    /// </summary>
    public static bool TryAnswer(string prompt, out string answer)
    {
        answer = "";
        string expr = Normalize(prompt);
        if (expr.Length == 0 || expr.Length > 512) return false;
        if (!expr.Any(char.IsAsciiDigit)) return false;
        foreach (char c in expr)
            if (!char.IsAsciiDigit(c) && !" .,+-*/%^()".Contains(c))
                return false;

        // Pure-integer expressions (no '.' and no '/') evaluate over BigInteger:
        // exact at any magnitude, so 2^127 - 1 computes instead of overflowing
        // decimal or hitting the exponent cap. Fractions and division keep the
        // decimal path (terminating-division discipline lives there).
        if (!expr.Contains('.') && !expr.Contains('/'))
            return TryAnswerBigInteger(expr, out answer);

        try
        {
            int pos = 0;
            decimal value = ParseExpr(expr, ref pos);
            SkipSpace(expr, ref pos);
            if (pos != expr.Length) return false;   // residual text: decline
            answer = Render(value);
            return true;
        }
        catch (Exception ex) when (ex is FormatException or OverflowException
                                       or DivideByZeroException)
        {
            return false;
        }
    }

    // ── BigInteger path: exact integer arithmetic at any magnitude ──

    private static bool TryAnswerBigInteger(string expr, out string answer)
    {
        answer = "";
        try
        {
            int pos = 0;
            System.Numerics.BigInteger value = BigExpr(expr, ref pos);
            SkipSpace(expr, ref pos);
            if (pos != expr.Length) return false;
            answer = value.ToString();
            return true;
        }
        catch (Exception ex) when (ex is FormatException or OverflowException
                                       or DivideByZeroException)
        {
            return false;
        }
    }

    private static System.Numerics.BigInteger BigExpr(string s, ref int pos)
    {
        System.Numerics.BigInteger left = BigTerm(s, ref pos);
        while (true)
        {
            SkipSpace(s, ref pos);
            if (pos < s.Length && s[pos] == '+') { pos++; left += BigTerm(s, ref pos); }
            else if (pos < s.Length && s[pos] == '-') { pos++; left -= BigTerm(s, ref pos); }
            else return left;
        }
    }

    private static System.Numerics.BigInteger BigTerm(string s, ref int pos)
    {
        System.Numerics.BigInteger left = BigFactor(s, ref pos);
        while (true)
        {
            SkipSpace(s, ref pos);
            if (pos < s.Length && s[pos] == '*') { pos++; left *= BigFactor(s, ref pos); }
            else if (pos < s.Length && s[pos] == '%')
            {
                pos++;
                System.Numerics.BigInteger m = BigFactor(s, ref pos);
                if (m == 0) throw new DivideByZeroException();
                left %= m;
            }
            else return left;
        }
    }

    private static System.Numerics.BigInteger BigFactor(string s, ref int pos)
    {
        System.Numerics.BigInteger baseValue = BigUnary(s, ref pos);
        SkipSpace(s, ref pos);
        if (pos < s.Length && s[pos] == '^')
        {
            pos++;
            System.Numerics.BigInteger exp = BigFactor(s, ref pos);   // right-associative
            // Bounded to keep the result and the work finite; 2^127 is trivial,
            // 2^100000 (~30k digits) is the ceiling, beyond which we decline.
            if (exp < 0 || exp > 100000) throw new FormatException("exponent out of range");
            return System.Numerics.BigInteger.Pow(baseValue, (int)exp);
        }
        return baseValue;
    }

    private static System.Numerics.BigInteger BigUnary(string s, ref int pos)
    {
        SkipSpace(s, ref pos);
        if (pos < s.Length && s[pos] == '-') { pos++; return -BigUnary(s, ref pos); }
        if (pos < s.Length && s[pos] == '+') { pos++; return BigUnary(s, ref pos); }
        return BigPrimary(s, ref pos);
    }

    private static System.Numerics.BigInteger BigPrimary(string s, ref int pos)
    {
        SkipSpace(s, ref pos);
        if (pos < s.Length && s[pos] == '(')
        {
            pos++;
            System.Numerics.BigInteger inner = BigExpr(s, ref pos);
            SkipSpace(s, ref pos);
            if (pos >= s.Length || s[pos] != ')') throw new FormatException("unbalanced paren");
            pos++;
            return inner;
        }

        int start = pos;
        while (pos < s.Length && (char.IsAsciiDigit(s[pos]) || s[pos] == ','))
            pos++;
        if (pos == start) throw new FormatException("number expected");
        string token = s[start..pos];

        if (token.Contains(','))   // strict thousands grouping, same as decimal
        {
            string[] groups = token.Split(',');
            if (groups.Length < 2 || groups[0].Length is < 1 or > 3 ||
                groups.Skip(1).Any(g => g.Length != 3))
                throw new FormatException("malformed thousands grouping");
            token = token.Replace(",", "");
        }
        return System.Numerics.BigInteger.Parse(token);
    }

    // ── strict recursive descent over decimal ──

    private static void SkipSpace(string s, ref int pos)
    {
        while (pos < s.Length && s[pos] == ' ') pos++;
    }

    private static decimal ParseExpr(string s, ref int pos)
    {
        decimal left = ParseTerm(s, ref pos);
        while (true)
        {
            SkipSpace(s, ref pos);
            if (pos < s.Length && s[pos] == '+') { pos++; left += ParseTerm(s, ref pos); }
            else if (pos < s.Length && s[pos] == '-') { pos++; left -= ParseTerm(s, ref pos); }
            else return left;
        }
    }

    private static decimal ParseTerm(string s, ref int pos)
    {
        decimal left = ParseFactor(s, ref pos);
        while (true)
        {
            SkipSpace(s, ref pos);
            if (pos < s.Length && s[pos] == '*') { pos++; left *= ParseFactor(s, ref pos); }
            else if (pos < s.Length && s[pos] == '/')
            {
                pos++;
                decimal divisor = ParseFactor(s, ref pos);
                if (divisor == 0) throw new DivideByZeroException();
                left = DivideExactly(left, divisor);    // declines non-terminating
            }
            else if (pos < s.Length && s[pos] == '%')
            {
                pos++;
                decimal m = ParseFactor(s, ref pos);
                if (m == 0) throw new DivideByZeroException();
                left %= m;
            }
            else return left;
        }
    }

    private static decimal ParseFactor(string s, ref int pos)
    {
        decimal baseValue = ParseUnary(s, ref pos);
        SkipSpace(s, ref pos);
        if (pos < s.Length && s[pos] == '^')
        {
            pos++;
            decimal exponent = ParseFactor(s, ref pos);   // right-associative
            if (exponent != decimal.Truncate(exponent) || exponent < 0 || exponent > 64)
                throw new FormatException("exponent must be an integer in [0,64]");
            decimal result = 1m;
            for (int i = 0; i < (int)exponent; i++) result *= baseValue;   // overflow throws
            return result;
        }
        return baseValue;
    }

    private static decimal ParseUnary(string s, ref int pos)
    {
        SkipSpace(s, ref pos);
        if (pos < s.Length && s[pos] == '-') { pos++; return -ParseUnary(s, ref pos); }
        if (pos < s.Length && s[pos] == '+') { pos++; return ParseUnary(s, ref pos); }
        return ParsePrimary(s, ref pos);
    }

    private static decimal ParsePrimary(string s, ref int pos)
    {
        SkipSpace(s, ref pos);
        if (pos < s.Length && s[pos] == '(')
        {
            pos++;
            decimal inner = ParseExpr(s, ref pos);
            SkipSpace(s, ref pos);
            if (pos >= s.Length || s[pos] != ')') throw new FormatException("unbalanced paren");
            pos++;
            return inner;
        }

        int start = pos;
        while (pos < s.Length && (char.IsAsciiDigit(s[pos]) || s[pos] == ',' || s[pos] == '.'))
            pos++;
        if (pos == start) throw new FormatException("number expected");
        string token = s[start..pos];

        // Strict thousands grouping: "1,234" is a number, "1,2" is a lie.
        if (token.Contains(','))
        {
            string intPart = token.Split('.')[0];
            string[] groups = intPart.Split(',');
            if (groups.Length < 2 || groups[0].Length is < 1 or > 3 ||
                groups.Skip(1).Any(g => g.Length != 3))
                throw new FormatException("malformed thousands grouping");
            token = token.Replace(",", "");
        }

        return decimal.Parse(token, System.Globalization.NumberStyles.AllowDecimalPoint,
                             System.Globalization.CultureInfo.InvariantCulture);
    }

    /// <summary>
    /// Division that is exact or refuses. decimal's own division rounds
    /// non-terminating expansions to 28 digits — and the naive round-trip
    /// check (quotient × divisor == dividend) is defeated by the multiply
    /// rounding the error back out (10/3 passes it). Number theory does not
    /// round: a reduced fraction terminates in base 10 iff its denominator
    /// is 2^a·5^b.
    /// </summary>
    private static decimal DivideExactly(decimal left, decimal divisor)
    {
        // decimal = mantissa / 10^scale, exactly.
        static (BigInteger Mantissa, int Scale) Parts(decimal d)
        {
            int[] bits = decimal.GetBits(d);
            var mantissa = new BigInteger((uint)bits[2]) << 64
                         | new BigInteger((uint)bits[1]) << 32
                         | new BigInteger((uint)bits[0]);
            if (d < 0) mantissa = -mantissa;
            return (mantissa, (bits[3] >> 16) & 0xFF);
        }

        (BigInteger ln, int ls) = Parts(left);
        (BigInteger dn, int ds) = Parts(divisor);
        BigInteger num = ln * BigInteger.Pow(10, ds);
        BigInteger den = dn * BigInteger.Pow(10, ls);
        if (den < 0) { num = -num; den = -den; }

        BigInteger g = BigInteger.GreatestCommonDivisor(BigInteger.Abs(num), den);
        if (g > 1) { num /= g; den /= g; }

        int twos = 0, fives = 0;
        while (den % 2 == 0) { den /= 2; twos++; }
        while (den % 5 == 0) { den /= 5; fives++; }
        if (den != 1) throw new FormatException("non-terminating division");

        // Scale to a power of ten: pad with the missing factor.
        int scale = Math.Max(twos, fives);
        if (scale > 28) throw new OverflowException();
        num *= BigInteger.Pow(twos > fives ? 5 : 2, Math.Abs(twos - fives));

        decimal result = (decimal)num;                 // throws if > 96 bits
        return result / Pow10(scale);                  // exact: scale <= 28
    }

    private static decimal Pow10(int n)
    {
        decimal p = 1m;
        for (int i = 0; i < n; i++) p *= 10m;
        return p;
    }

    /// <summary>Canonical rendering: trims trailing zeros, no scientific notation, -0 → 0.</summary>
    public static string Render(decimal value)
    {
        if (value == 0m) return "0";
        string s = value.ToString(System.Globalization.CultureInfo.InvariantCulture);
        if (s.Contains('.'))
        {
            s = s.TrimEnd('0').TrimEnd('.');
            if (s.Length == 0 || s == "-") s = "0";
        }
        return s;
    }
}
