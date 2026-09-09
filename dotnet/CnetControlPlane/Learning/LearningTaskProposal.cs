using System.Globalization;
using System.Text.RegularExpressions;

namespace CnetControlPlane.Learning;

internal sealed record LearningTaskProposal(string Status, string Code, string? Dataset = null,
    byte? Key = null, string? Prompt = null);

// Bounded offline grammar, not a learned semantic model. It proposes input and
// operation only. No casing result, source authority, coverage or label is inferred here.
internal static class LearningTaskParser
{
    private const RegexOptions Options = RegexOptions.CultureInvariant | RegexOptions.IgnoreCase | RegexOptions.NonBacktracking;
    private const string Case = @"(?<op>upper[ -]?case|lower[ -]?case|capital(?:[ -]letter)?s?|capitali[sz]ed|small[ -]letter)(?: form| version| equivalent| letter)?";
    private const string Input = @"(?<input>.+?)";
    private static readonly Regex Canonical = new(@"\Aunicode (?<op>upper|lower) (?<input>[0-9]{1,3})\z", Options);
    private static readonly Regex PolitePrefix = new(@"\A(?:(?:can|could|would|will) you (?:please )?|please )", Options);
    private static readonly Regex PoliteSuffix = new(@"(?: for me(?:,? please)?|,? please)\z", Options);
    private static readonly Regex InputDescription = new(@"\A(?:the )?(?:single )?(?:character|letter) ", Options);
    private static readonly Regex MissingDirection = new(@"\A(?:(?:change|adjust|set) (?:the )?(?:letter )?case(?: of .+)?|case-convert .+|(?:convert|change) .+ to (?:the )?(?:requested )?case|apply (?:a )?case (?:conversion|operation)(?: to .+)?)\z", Options);
    // These words cannot be a single literal operand. Do not reject "a and b":
    // alternatives need clarification, while an additional action must abstain.
    private static readonly Regex CompoundOrNegated = new(@"\b(?:not|then|also)\b|\band (?:upper[ -]?case|lower[ -]?case|capitali[sz]e|run|execute|add|subtract|multiply|divide|explain|write|send|delete|open|print|translate|convert|change|make|summari[sz]e|reverse|tell|do)\b", Options);
    private static readonly Regex UnsupportedOperand = new(@"(?:\busing\b|\baccording to\b|;|\A(?:the )?(?:(?:whole|entire) )?(?:string|word)\b)", Options);
    private static readonly Regex[] Forms = [
        new(@"\A" + Case + @"(?: " + Input + @")?\z", Options),
        new(@"\A(?<op>capitali[sz]e)(?: " + Input + @")?\z", Options),
        new(@"\A(?:convert|change|turn) (?:" + Input + @" )?(?:to|into) (?:an? |its )?" + Case + @"\z", Options),
        new(@"\Aset (?:the )?case of " + Input + @" to " + Case + @"\z", Options),
        new(@"\A(?:make|put|write|render|return) (?:" + Input + @" (?:(?:in|as|into|using) )?(?:an? |its )?)?" + Case + @"\z", Options),
        new(@"\A(?:give(?: me)?|show(?: me)?|return|display|what is|what's|what’s|i would like|i want|(?:may|can|could) i (?:have|get)) (?:the )?" + Case + @"(?: of " + Input + @")?\z", Options),
        new(@"\A(?:i need|i want|i would like) " + Input + @" (?:in|as) (?:an? )?" + Case + @"\z", Options),
        new(@"\Athe " + Case + @" of " + Input + @"\z", Options),
        new(@"\Afor " + Input + @", (?:give|show)(?: me)? (?:its |the )?" + Case + @"\z", Options)
    ];
    private static LearningTaskProposal Clarify() => new("clarify", "specify_case_input", Prompt:
        "Specify uppercase or lowercase and one quoted Latin-1 character, or an explicit codepoint (for example U+00B5).");
    private static LearningTaskProposal Abstain(string code) => new("abstain", code);
    private static LearningTaskProposal Ready(string operation, byte key) => char.IsControl((char)key) ? Abstain("input_domain") : new("ready", "typed_case_change",
        operation.StartsWith("lower", StringComparison.OrdinalIgnoreCase) || operation.StartsWith("small", StringComparison.OrdinalIgnoreCase)
            ? "unicode17_lower_latin1" : "unicode17_upper_latin1", key);

    internal static LearningTaskProposal Propose(string text)
    {
        if (text.Length is < 1 or > 256 || text.Any(char.IsControl) || text.Any(char.IsSurrogate)) return Abstain("input_bounds");
        text = text.Trim();
        var exact = Canonical.Match(text);
        if (exact.Success)
        {
            var value = exact.Groups["input"].Value;
            return byte.TryParse(value, NumberStyles.None, CultureInfo.InvariantCulture, out var key)
                && key.ToString(CultureInfo.InvariantCulture) == value ? Ready(exact.Groups["op"].Value, key) : Abstain("input_domain");
        }
        if (text.EndsWith('?') || text.EndsWith('.')) text = text[..^1];
        text = PolitePrefix.Replace(text, "", 1);
        text = PoliteSuffix.Replace(text, "", 1);
        if (CompoundOrNegated.IsMatch(text)) return Abstain("unsupported_intent");
        foreach (var form in Forms)
        {
            var match = form.Match(text);
            if (!match.Success) continue;
            return ProposeOperand(match.Groups["op"].Value, match.Groups["input"].Value.Trim());
        }
        return MissingDirection.IsMatch(text) ? Clarify() : Abstain("unsupported_intent");
    }

    private static LearningTaskProposal ProposeOperand(string operation, string token)
    {
        // Strip only the bounded operand description, never normalize the scalar.
        token = InputDescription.Replace(token, "", 1);
        if (token.Length == 0) return Clarify();
        if (token.Length == 3 && (token[0] == '\'' && token[2] == '\'' || token[0] == '"' && token[2] == '"'))
            return token[1] <= 255 ? Ready(operation, (byte)token[1]) : Abstain("input_domain");
        if (UnsupportedOperand.IsMatch(token)) return Abstain("unsupported_intent");
        // Alternatives are ambiguous even when their first operand is explicitly encoded.
        if (token.Contains(" or ", StringComparison.OrdinalIgnoreCase)) return Clarify();
        if (token.Length > 3 && (token[0] == '\'' && token[^1] == '\'' || token[0] == '"' && token[^1] == '"'))
            return Abstain("unsupported_intent"); // A quoted string is not a single character.
        if (token.StartsWith("U+", StringComparison.OrdinalIgnoreCase))
            return token.Length is >= 3 and <= 6 && byte.TryParse(token[2..], NumberStyles.AllowHexSpecifier,
                CultureInfo.InvariantCulture, out var hex) ? Ready(operation, hex) : Abstain("input_domain");
        if (token.StartsWith("decimal codepoint ", StringComparison.OrdinalIgnoreCase)) token = token[8..];
        if (token.StartsWith("codepoint ", StringComparison.OrdinalIgnoreCase))
            return byte.TryParse(token[10..], NumberStyles.None, CultureInfo.InvariantCulture, out var number)
                ? Ready(operation, number) : Abstain("input_domain");
        if (token.All(char.IsAsciiDigit)) return Clarify(); // A numeric string is not implicitly a codepoint.
        if (token.Length != 1) return Clarify();
        if (token[0] > 255) return Abstain("input_domain");
        // Bare punctuation can be a terminator or an unterminated quote.
        return char.IsLetter(token[0]) ? Ready(operation, (byte)token[0]) : Clarify();
    }
}
