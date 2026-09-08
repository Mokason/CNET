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
    private static readonly Regex Canonical = new(@"\Aunicode (?<op>upper|lower) (?<input>[0-9]{1,3})\z", Options);
    private static readonly Regex[] Forms = [
        new(@"\A(?<op>uppercase|lowercase)(?: (?<input>.+))?\z", Options),
        new(@"\A(?:convert|change) (?<input>.+) to (?<op>uppercase|lowercase)\z", Options),
        new(@"\Amake (?<input>.+) (?<op>uppercase|lowercase)\z", Options),
        new(@"\A(?:what is|what's|what’s) (?:the )?(?<op>uppercase|lowercase) of (?<input>.+)\z", Options)
    ];
    private static LearningTaskProposal Clarify() => new("clarify", "specify_case_input", Prompt:
        "Specify uppercase or lowercase and one quoted Latin-1 character, or an explicit codepoint (for example U+00B5).");
    private static LearningTaskProposal Abstain(string code) => new("abstain", code);
    private static LearningTaskProposal Ready(string operation, byte key) => new("ready", "typed_case_change",
        operation.StartsWith("upper", StringComparison.OrdinalIgnoreCase) ? "unicode17_upper_latin1" : "unicode17_lower_latin1", key);

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
        if (text.StartsWith("please ", StringComparison.OrdinalIgnoreCase)) text = text[7..];
        if (text.EndsWith('?') || text.EndsWith('.')) text = text[..^1];
        foreach (var form in Forms)
        {
            var match = form.Match(text);
            if (!match.Success) continue;
            var token = match.Groups["input"].Value.Trim();
            if (token.Length == 0) return Clarify();
            if (token.Length == 3 && (token[0] == '\'' && token[2] == '\'' || token[0] == '"' && token[2] == '"'))
                return token[1] <= 255 ? Ready(match.Groups["op"].Value, (byte)token[1]) : Abstain("input_domain");
            if (token.StartsWith("U+", StringComparison.OrdinalIgnoreCase))
                return token.Length is >= 3 and <= 6 && byte.TryParse(token[2..], NumberStyles.AllowHexSpecifier,
                    CultureInfo.InvariantCulture, out var hex) ? Ready(match.Groups["op"].Value, hex) : Abstain("input_domain");
            if (token.StartsWith("codepoint ", StringComparison.OrdinalIgnoreCase))
                return byte.TryParse(token[10..], NumberStyles.None, CultureInfo.InvariantCulture, out var number)
                    ? Ready(match.Groups["op"].Value, number) : Abstain("input_domain");
            if (token.All(char.IsAsciiDigit)) return Clarify(); // A numeric string is not implicitly a codepoint.
            if (token.Length != 1) return Clarify();
            if (token[0] > 255) return Abstain("input_domain");
            // Bare punctuation can be a terminator or an unterminated quote.
            // Only letters are unambiguous without quotes or an explicit codepoint.
            return char.IsLetter(token[0]) ? Ready(match.Groups["op"].Value, (byte)token[0]) : Clarify();
        }
        return text.Equals("change case", StringComparison.OrdinalIgnoreCase)
            || text.StartsWith("change case of ", StringComparison.OrdinalIgnoreCase) ? Clarify() : Abstain("unsupported_intent");
    }
}
