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
    // Raw controls are refused before this internal marker is ever introduced.
    // Recognize direction once, then compose small whole-request frames without
    // multiplying their automata by the operation vocabulary at every site.
    private const string Case = @"\x1F";
    private static readonly Regex OperationLexeme = new(@"\b(?<op>upper(?:[ -]?cas(?:e|ing|ed))?|lower(?:[ -]?cas(?:e|ing|ed))?|capital(?:[ -]letter)?s?|capitali[sz]e(?:d)?|small[ -]letter)\b(?<noun> form| version| equivalent| letter| character| conversion)?", Options);
    private const string OutputVerb = @"(?:make|put|write|render|return)";
    private const string Input = @"(?<input>.+?)";
    private const string InputLabel = @"(?:(?:the |my )?input(?: scalar)?|character(?: supplied)?)";
    private const string Declaration = InputLabel + @"(?: is |: ?)" + Input;
    private const string NumericDeclaration = @"(?:the |my )?input (?<numeric>code[ -]?point)(?: is |: ?)" + Input;
    private const string Alternative = @"(?:(?:the )?(?:single )?(?:character|letter|scalar) )?(?:'.'|"".""|(?:(?:the )?code[ -]?point )?(?:U\+|0x)[0-9a-f]{1,6}|(?:decimal )?code[ -]?point [0-9]{1,6}|[0-9]+(?:\.[0-9]+)?|[^\s])";
    private static readonly Regex Canonical = new(@"\Aunicode (?<op>upper|lower) (?<input>[0-9]{1,3})\z", Options);
    private static readonly Regex PolitePrefix = new(@"\A(?:(?:can|could|would|will) you (?:please )?|(?:i would|i'd|i’d) like you to |for me, (?:please )?|please |kindly )", Options);
    private static readonly Regex PoliteSuffix = new(@"(?: for me(?:,? please)?|,? please)\z", Options);
    private static readonly Regex InputDescription = new(@"\A(?:the |an? )?(?:single )?(?:character|letter|scalar) ", Options);
    private static readonly Regex CodepointRelation = new(@"\A(?:with|at|represented by|whose) (?<input>(?:decimal )?code[ -]?point .+|U\+.+|0x.+)\z", Options);
    private static readonly Regex DecimalInput = new(@"\A(?:the )?(?:decimal )?code[ -]?point (?:is )?(?:decimal )?(?<number>[0-9]+)(?: in decimal)?\z", Options);
    private static readonly Regex HexInput = new(@"\A(?:(?:the )?code[ -]?point (?:is )?)?(?:U\+|0x)(?<number>[0-9a-f]{1,6})\z", Options);
    private static readonly Regex CodepointPrefix = new(@"\A(?:(?:the )?(?:decimal )?code[ -]?point\b|U\+|0x)", Options);
    private static readonly Regex BareNumber = new(@"\A[+-]?[0-9]+(?:\.[0-9]+)?\z", Options);
    private static readonly Regex MissingScalar = new(@"\A(?:the |an? |one )?(?:single )?(?:character|letter|codepoint)\z", Options);
    private static readonly Regex MissingDirection = new(@"\A(?:(?:change|adjust|set) (?:the )?(?:letter )?case(?: of " + Input + @")?|case-convert " + Input
        + @"|(?:convert|change) " + Input + @"(?: to (?:the )?(?:requested )?case)?|apply (?:a )?case (?:conversion|operation)(?: to " + Input + @")?)\z", Options);
    private static readonly Regex MissingOperand = new(@"\A(?:" + OutputVerb + @"|give(?: me)?|show(?: me)?|display) an? " + Case + @"\z", Options);
    private static readonly Regex CompoundOrNegated = new(@"\b(?:not|then|also)\b", Options);
    private static readonly Regex AlternativeToken = new(Alternative, Options);
    private static readonly Regex AlternativeSeparator = new(@" *, *(?:(?:and|or) +)?| +(?:(?:and|or) +)?", Options);
    private static readonly Regex HasConjunction = new(@"\b(?:and|or)\b", Options);
    private static readonly Regex QuotedString = new(@"\A(?:'[^']*'|""[^""]*"")\z", Options);
    private static readonly Regex UnsupportedOperand = new(@"(?:\b(?:using|locale|rules|string|word|sentence|paragraph|text)\b|\baccording to\b|;)", Options);
    private static readonly Regex[] Forms = [
        new(@"\A" + Case + @"(?: " + Input + @")?\z", Options),
        new(@"\A(?:convert|change|turn) (?:" + Input + @" )?(?:to|into) (?:an? |its )?" + Case + @"\z", Options),
        new(@"\Aset (?:the )?case of " + Input + @" to " + Case + @"\z", Options),
        new(@"\A" + OutputVerb + @" (?:" + Input + @" (?:(?:in|as|into|using) )?(?:an? |its )?)?" + Case + @"\z", Options),
        new(@"\A(?:give(?: me)?|show(?: me)?|return|display|use) (?:the |an? )?" + Case + @"(?: (?:of|corresponding to) " + Input + @")?\z", Options),
        new(@"\A(?:what is|what's|what’s|(?:may|can|could) i (?:have|get)) (?:the |an? )?" + Case + @"(?: of " + Input + @")?\z", Options),
        new(@"\A(?:i would like|i'd like|i’d like|i want|my request is) (?:the |an? )?" + Case + @"(?: of " + Input + @")?\z", Options),
        new(@"\A(?:i need|i want|i would like|i'd like|i’d like) " + Input + @" (?:in|as) (?:an? )?" + Case + @"\z", Options),
        new(@"\Athe " + Case + @" of " + Input + @"\z", Options),
        new(@"\Afor " + Input + @", (?:give|show|return|use|perform)(?: me)? (?:its |the )?" + Case + @"\z", Options),
        new(@"\Ahow (?:does|would|will) " + Input + @" look (?:in|as) " + Case + @"\z", Options),
        new(@"\Awith " + Input + @" as (?:the |my )?input, (?:return|give|show)(?: me)? (?:its |the )?" + Case + @"\z", Options),
        // A bounded declaration supplies one operand to one operation, not a
        // substring extracted from arbitrary prose or a second executable step.
        new(@"\Athe (?:character|letter|input) is " + Input + @"[;. ]+ (?:please )?" + Case + @" it\z", Options),
        new(@"\A" + Input + @" is (?:my|the) input[;. ]+ (?:please )?(?:convert|change|turn) it (?:to|into) " + Case + @"\z", Options),
        new(@"\A(?:use|apply|perform) (?:the )?" + Case + @"(?: (?:for|to|on) " + Input + @")?\z", Options),
        new(@"\Awhich " + Case + @" corresponds to " + Input + @"\z", Options),
        new(@"\Awhat do i get when i " + Case + @" " + Input + @"\z", Options),
        new(@"\Ause " + Input + @" as input for " + Case + @"\z", Options),
        new(@"\A(?:an? )?" + Case + @" is what i (?:need|want)\z", Options),
        // Compose an explicit input label with a single action or direction
        // label. Numeric declarations retain their codepoint type downstream.
        new(@"\A" + Declaration + @"[;.:] (?:please )?(?:convert|change|turn) (?:it|this(?: one)? character) (?:to|into) " + Case + @"\z", Options),
        new(@"\A" + Declaration + @"[;.:] (?:please )?(?:apply|perform|use) " + Case + @"\z", Options),
        new(@"\A" + Declaration + @"[;.:] (?:(?:the )?(?:requested )?(?:case )?operation(?: is |: ?)|desired case: ?)" + Case + @"\z", Options),
        new(@"\A" + NumericDeclaration + @"[;.:] (?:please )?(?:convert|change|turn) it (?:to|into) " + Case + @"\z", Options),
        new(@"\A" + NumericDeclaration + @"[;.:] (?:please )?(?:apply|perform|use) " + Case + @"\z", Options)
    ];
    private static readonly Regex[] DirectionlessForms = [
        MissingDirection,
        new(@"\A" + Declaration + @"\z", Options),
        new(@"\A" + NumericDeclaration + @"\z", Options),
        new(@"\Ause " + Input + @"\z", Options),
        new(@"\Ahere is " + Input + @": (?:please )?change its case\z", Options),
        new(@"\A(?:should " + Input + @" be made|set " + Input + @" to) (?:upper(?:[ -]?case)?|lower(?:[ -]?case)?) or (?:upper(?:[ -]?case)?|lower(?:[ -]?case)?)\z", Options)
    ];

    private static string OperandText(Match match) => (match.Groups["numeric"].Success ? "code point " : "") + match.Groups["input"].Value.Trim();
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
        if (text.EndsWith('?') || text.EndsWith('.')) text = text[..^1].TrimEnd();
        text = PolitePrefix.Replace(text, "", 1);
        text = PoliteSuffix.Replace(text, "", 1);
        if (CompoundOrNegated.IsMatch(text)) return Abstain("unsupported_intent");
        foreach (Match operation in OperationLexeme.Matches(text))
        {
            var operationName = operation.Groups["op"].Value;
            var framed = text[..operation.Index] + '\u001f' + text[(operation.Index + operation.Length)..];
            // An indefinite article is not an input. Quoted 'a' and explicit
            // "a as ..." remain operands; preserve the lexeme's noun evidence.
            if (MissingOperand.IsMatch(framed) && (operation.Groups["noun"].Success
                || operationName.Contains("letter", StringComparison.OrdinalIgnoreCase)
                || operationName.Equals("capital", StringComparison.OrdinalIgnoreCase)
                || operationName.Equals("capitals", StringComparison.OrdinalIgnoreCase))) return Clarify();
            foreach (var form in Forms)
            {
                var match = form.Match(framed);
                if (!match.Success) continue;
                // Other case words remain uninterpreted operand text: a frame
                // cannot turn a description, whole string or extra action into
                // one scalar merely because it contains an operation word.
                return ProposeOperand(operationName, OperandText(match));
            }
        }
        // Reuse operand refusal even when direction is absent. A hypothetical
        // ready scalar is discarded: missing direction can never authorize it.
        foreach (var form in DirectionlessForms)
        {
            var missing = form.Match(text);
            if (!missing.Success) continue;
            var operand = ProposeOperand("upper", OperandText(missing));
            return operand.Status == "abstain" ? operand : Clarify();
        }
        return Abstain("unsupported_intent");
    }

    private static LearningTaskProposal ProposeOperand(string operation, string token, bool allowAlternatives = true)
    {
        // Strip only the bounded operand description, never normalize the scalar.
        token = InputDescription.Replace(token, "", 1);
        // Relational descriptions require an explicit codepoint, never a guessed
        // byte from a location or an incomplete article such as "letter with a".
        var relation = CodepointRelation.Match(token);
        if (relation.Success) token = relation.Groups["input"].Value;
        if (token.Length == 0 || MissingScalar.IsMatch(token)) return Clarify();
        if (token.Length == 3 && (token[0] == '\'' && token[2] == '\'' || token[0] == '"' && token[2] == '"'))
            return token[1] <= 255 ? Ready(operation, (byte)token[1]) : Abstain("input_domain");
        if (QuotedString.IsMatch(token)) return token.Length == 2 ? Clarify() : Abstain("unsupported_intent");
        // Only lists of candidate operands are ambiguity. An arbitrary second
        // clause is unsupported regardless of its verb; no action lexicon needed.
        if (allowAlternatives && ProposeAlternatives(token) is { } alternatives) return alternatives;
        if (HasConjunction.IsMatch(token) || UnsupportedOperand.IsMatch(token)) return Abstain("unsupported_intent");
        var hexadecimal = HexInput.Match(token);
        if (hexadecimal.Success)
            return byte.TryParse(hexadecimal.Groups["number"].Value, NumberStyles.AllowHexSpecifier,
                CultureInfo.InvariantCulture, out var hex) ? Ready(operation, hex) : Abstain("input_domain");
        var numeric = DecimalInput.Match(token);
        if (numeric.Success)
            return byte.TryParse(numeric.Groups["number"].Value, NumberStyles.None, CultureInfo.InvariantCulture, out var number)
                ? Ready(operation, number) : Abstain("input_domain");
        if (CodepointPrefix.IsMatch(token)) return Abstain("input_domain");
        if (BareNumber.IsMatch(token)) return Clarify(); // A numeric string is not implicitly a codepoint.
        var first = AlternativeToken.Match(token);
        if (first.Success && first.Index == 0 && first.Length < token.Length
            && (char.IsWhiteSpace(token[first.Length]) || token[first.Length] is ',' or ';' or '.' or '!' or '?'))
            return Abstain("unsupported_intent"); // Identified operand followed by a non-alternative clause.
        if (token.Length != 1) return Clarify();
        if (token[0] > 255) return Abstain("input_domain");
        // Bare punctuation can be a terminator or an unterminated quote.
        return char.IsLetter(token[0]) ? Ready(operation, (byte)token[0]) : Clarify();
    }

    private static LearningTaskProposal? ProposeAlternatives(string token)
    {
        if (token.StartsWith("either ", StringComparison.OrdinalIgnoreCase)) token = token[7..];
        else if (token.StartsWith("one of ", StringComparison.OrdinalIgnoreCase)) token = token[7..];
        var position = 0;
        var candidates = new List<string>();
        // Every token/separator consumes input, and every match must start at the
        // exact cursor. Split bounded patterns avoid a large combined automaton.
        while (position < token.Length)
        {
            var candidate = AlternativeToken.Match(token, position);
            if (!candidate.Success || candidate.Index != position) return null;
            position += candidate.Length;
            candidates.Add(candidate.Value);
            if (position == token.Length)
            {
                if (candidates.Count < 2) return null;
                // Use the same scalar/domain checks, with list parsing disabled
                // to bound recursion. No candidate's executable proposal escapes.
                return candidates.Any(value => ProposeOperand("upper", value, false).Code == "input_domain")
                    ? Abstain("input_domain") : Clarify();
            }
            var separator = AlternativeSeparator.Match(token, position);
            if (!separator.Success || separator.Index != position) return null;
            position += separator.Length;
        }
        return null;
    }
}
