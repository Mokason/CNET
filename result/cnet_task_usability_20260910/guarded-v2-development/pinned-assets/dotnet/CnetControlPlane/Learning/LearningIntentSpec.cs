using System.Text;
using System.Text.RegularExpressions;

namespace CnetControlPlane.Learning;

internal sealed record LearningIntentExample(string Text, string Status, string? Operation);

// Answer-free external spec. Train and calibration are disjoint template
// families. Evaluation "Kindly recode the glyph 'P'" is in neither.
internal static class LearningIntentSpec
{
    private static readonly Regex Codepoint = new(@"\b(?:U\+[0-9A-Fa-f]{1,6}|0x[0-9A-Fa-f]{1,6})\b",
        RegexOptions.CultureInvariant | RegexOptions.NonBacktracking);

    internal static IReadOnlyList<LearningIntentExample> TrainCorpus()
    {
        var examples = new List<LearningIntentExample>(2048);
        string[] operands = ["'A'", "'ñ'", "' '", "'-'", "0xC9", "U+00E4", "q"];
        string[] lower = ["lowercase", "lower case", "small letters"];
        string[] upper = ["uppercase", "upper case", "capitals"];
        foreach (var operand in operands)
        {
            foreach (var direction in lower)
            {
                AddReady(examples, "lower",
                    $"recode the glyph {operand} toward {direction}.",
                    $"Please recode the glyph {operand} toward {direction}.",
                    $"Could you recode the glyph {operand} toward {direction}?",
                    $"convert {operand} to {direction}",
                    $"Would you mind changing {operand} to {direction}?",
                    $"Take {operand} and switch it to {direction}.",
                    $"Show {operand} after converting it to {direction}.",
                    $"Rewrite {operand} using {direction}.");
            }
            foreach (var direction in upper)
            {
                AddReady(examples, "upper",
                    $"recode the glyph {operand} toward {direction}.",
                    $"Please recode the glyph {operand} toward {direction}.",
                    $"Could you recode the glyph {operand} toward {direction}?",
                    $"convert {operand} to {direction}",
                    $"Would you mind changing {operand} to {direction}?",
                    $"Take {operand} and switch it to {direction}.",
                    $"Show {operand} after converting it to {direction}.");
            }
            examples.Add(new($"recode the glyph {operand} toward small letters and email it.", "abstain", null));
            examples.Add(new($"Do not recode the glyph {operand} toward small letters.", "abstain", null));
            examples.Add(new($"Raise {operand} after converting it to lowercase.", "abstain", null));
            examples.Add(new($"Show {operand} before converting it to lowercase.", "abstain", null));
            examples.Add(new($"write {operand} in caps and email it.", "abstain", null));
            examples.Add(new($"Would you mind putting {operand} in lowercase and sending it?", "abstain", null));
        }
        examples.Add(new("recode the glyph F or G toward small letters.", "clarify", null));
        examples.Add(new("What is the capital of France?", "abstain", null));
        return examples;
    }

    internal static IReadOnlyList<LearningIntentExample> CalibrationCorpus()
    {
        var examples = new List<LearningIntentExample>(512);
        string[] operands = ["'A'", "'ñ'", "' '", "'-'", "0xC9", "U+00E4", "q"];
        string[] lower = ["lowercase", "lower case", "small letters"];
        string[] upper = ["uppercase", "upper case", "capitals"];
        foreach (var operand in operands)
        {
            foreach (var direction in lower)
                AddReady(examples, "lower", $"Would you recode the symbol {operand} toward {direction}.");
            foreach (var direction in upper)
                AddReady(examples, "upper", $"Would you recode the symbol {operand} toward {direction}.");
            examples.Add(new($"Never recode the symbol {operand} toward lowercase.", "abstain", null));
        }
        examples.Add(new("Would you recode the symbol F or G toward small letters.", "clarify", null));
        examples.Add(new("Would you recode the symbol U+000A toward small letters.", "abstain", null));
        examples.Add(new("Would you recode the symbol U+0218 toward uppercase.", "abstain", null));
        return examples;
    }

    internal static string Normalize(string text)
    {
        var masked = Codepoint.Replace(MaskQuotes(text), " X ");
        var builder = new StringBuilder(masked.Length);
        var space = false;
        foreach (var c in masked)
        {
            if (char.IsWhiteSpace(c) || c is ',' or ';' or ':' or '.' or '!' or '?')
            {
                if (!space && builder.Length > 0) { builder.Append(' '); space = true; }
                continue;
            }
            builder.Append(char.ToLowerInvariant(c));
            space = false;
        }
        return builder.ToString().Trim();
    }

    private static string MaskQuotes(string text)
    {
        var chars = text.ToCharArray();
        for (var i = 0; i < chars.Length; i++)
        {
            var quote = chars[i];
            if (quote is not ('\'' or '"')) continue;
            if (i > 0 && char.IsLetterOrDigit(chars[i - 1]) && quote == '\'') continue;
            var end = text.IndexOf(quote, i + 1);
            if (end < 0) break;
            for (var k = i; k <= end; k++) chars[k] = k == i || k == end ? ' ' : 'X';
            i = end;
        }
        return new string(chars);
    }

    private static void AddReady(List<LearningIntentExample> examples, string operation, params string[] texts)
    {
        foreach (var text in texts) examples.Add(new(text, "ready", operation));
    }
}
