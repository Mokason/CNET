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
    private static readonly Regex OperationLexeme = new(@"\b(?<op>upper(?:[ -]?cas(?:e|ing|ed))?|lower(?:[ -]?cas(?:e|ing|ed))?|capital(?:[ -]letter)?s?|capitali[sz]e(?:d)?|small[ -]letters?)\b(?<noun> form| version| equivalent| letter| character| conversion| casing| rendition| result| operation| transformation| counterpart)?", Options);
    private const string OutputVerb = @"(?:make|put|write|render|return|show|display|supply|provide|present|express|give(?: me)?)";
    private const string Desire = @"(?:i (?:need|want|request|am requesting|would like|would prefer)|i['’]d (?:like|prefer)|i['’]m asking for)";
    private const string Manner = @"(?<passive>to be )?(?:(?<transform>converted|changed|put|written|rendered) )?(?:(?:in|as|into|using|with|to) )?(?:an? |its )?";
    private const string Input = @"(?<input>.+?)";
    private const string InputLabel = @"(?:(?:here is )?(?:the |my )?input(?: scalar| character)?|(?:the )?(?:chosen |selected |supplied )?character(?: supplied)?)";
    private const string Declaration = InputLabel + @"(?: is |: ?)" + Input;
    private const string NumericDeclaration = @"(?:the |my )?input (?<numeric>code[ -]?point)(?: is |: ?)" + Input;
    private const string Alternative = @"(?:(?:the )?(?:single )?(?:character|letter|scalar) )?(?:'.'|"".""|(?:(?:the )?code[ -]?point )?(?:U\+|0x)[0-9a-f]{1,6}|(?:decimal )?code[ -]?point [0-9]{1,6}|[0-9]+(?:\.[0-9]+)?|[^\s])";
    private static readonly Regex Canonical = new(@"\Aunicode (?<op>upper|lower) (?<input>[0-9]{1,3})\z", Options);
    private static readonly Regex ContextPrefix = new(@"\Afor this (?:request|task), ", Options);
    private static readonly Regex PolitePrefix = new(@"\A(?:(?:can|could|would|will) you (?:be able to )?(?:please )?|how (?:would|do) you |(?:i would|i'd|i’d) like (?:you to |to (?:see )?)|i want to |(?:my request|the task) is to |this request concerns |for me, (?:please )?|please |kindly )", Options);
    private static readonly Regex PoliteSuffix = new(@"(?: for me(?:,? please)?|,? please)\z", Options);
    private static readonly Regex PreferenceSuffix = new(@" is (?:requested|what i (?:need|want)|the one i want|what i'm after|what i’m after)\z", Options);
    private static readonly Regex AmbiguitySuffix = new(@"(?:, whichever(?: i meant)?|; i haven['’]t chosen which input yet)\z", Options);
    private static readonly Regex InputDescription = new(@"\A(?:only )?(?:the |an? |this |that )?(?:single |single-character |one-character |supplied |provided |chosen |selected |quoted |literal )?(?:(?:unicode|latin1) )?(?:character|letter|scalar|input(?: scalar| character)?)(?:: ?| )", Options);
    private static readonly Regex QuotedDescription = new(@"\A(?:the )?quoted literal (?<input>.+)\z", Options);
    private static readonly Regex QuotedFullStop = new(@"\A(?:the )?quoted full stop (?<input>'\.'|""\."")\z", Options);
    private static readonly Regex ScalarWritten = new(@"\Awritten (?<input>(?:U\+|0x).+)\z", Options);
    private static readonly Regex DecimalRelation = new(@"\Awhose decimal value is (?<input>[0-9]+)\z", Options);
    private static readonly Regex ExplicitScalarSuffix = new(@"\A(?<input>(?:U\+|0x)[0-9a-f]{1,6}), interpreted as a Unicode scalar,?\z", Options);
    private static readonly Regex CodepointRelation = new(@"\A(?:with|at|represented by|whose) (?<input>(?:decimal )?code[ -]?point .+|U\+.+|0x.+)\z", Options);
    private static readonly Regex DecimalInput = new(@"\A(?:the )?(?:decimal )?code[ -]?point (?:is )?(?:decimal )?(?<number>[0-9]+)(?: in decimal| \(decimal\))?\z", Options);
    private static readonly Regex HexInput = new(@"\A(?:(?:the )?(?:hex|hexadecimal) code[ -]?point (?:(?:U\+|0x))?|(?:(?:the )?code[ -]?point (?:is )?)?(?:U\+|0x))(?<number>[0-9a-f]{1,6})\z", Options);
    private static readonly Regex CodepointPrefix = new(@"\A(?:(?:the )?(?:decimal |hex |hexadecimal )?code[ -]?point\b|U\+|0x)", Options);
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
    private static readonly Regex UnsupportedOperand = new(@"(?:\b(?:using|locale|rules|string|word|sentence|paragraph|text|phrase|sequence)\b|\bboth letters\b|\baccording to\b|;)", Options);
    private static readonly Regex FieldSeparator = new(@"[;.,] (?:and |with )?| and ", Options);
    private static readonly Regex[] OperationFields = [
        new(@"\A(?:the |my )?(?:desired |requested |target )?(?:case(?: choice| conversion| operation)?|operation)(?: is(?: to be)? |: ?| selected: ?| should be )" + Case + @"\z", Options),
        new(@"\A" + Case + @" (?:is (?:the )?(?:requested |chosen )?(?:case|operation)|as the chosen operation)\z", Options),
        new(@"\A(?:i (?:request|want|need) (?:it |its )?|(?:please )?(?:use|apply|request) )" + Case + @"\z", Options)
    ];
    private static readonly Regex ActionField = new(@"\A(?:please )?(?:(?:put|write|render) it (?:in|as|into) " + Case + @"|" + Case + @" it)\z", Options);
    private static readonly Regex[] InputFields = [
        new(@"\A(?:(?:here|let) )?(?:the |my )?(?:chosen |selected |specified |provided |single-character )?(?:input(?: scalar| character| for this task)?|character|scalar)(?: is |: ?| be )" + Input + @"\z", Options),
        new(@"\A" + Input + @" is (?:my|the) input\z", Options),
        new(@"\A(?:the )?(?<numeric>code[ -]?point)(?: for this task)? is " + Input + @"\z", Options),
        new(@"\A(?:my )?chosen scalar has (?<numeric>code[ -]?point) " + Input + @"\z", Options),
        new(@"\Acode point, (?<radix>hexadecimal): " + Input + @"\z", Options),
        new(@"\A(?:the )?input " + Input + @" is a single scalar\z", Options),
        new(@"\Ai am supplying " + Input + @" as the character\z", Options)
    ];
    private static readonly Regex[] Forms = [
        new(@"\A(?:the )?(?:character|letter|thing) (?:to|i want in) " + Case + @" is " + Input + @"\z", Options),
        new(@"\A(?:the )?conversion i need is " + Case + @" " + Input + @"\z", Options),
        new(@"\A(?:for|in) (?:an? )?" + Case + @", (?:(?:please )?(?:use|convert)|what is) " + Input + @"\z", Options),
        new(@"\Afor " + Input + @", what would " + Case + @" be\z", Options),
        new(@"\A(?:which|what) " + Case + @" (?:goes with|results from|is associated with) " + Input + @"\z", Options),
        new(@"\A(?:use|take) " + Input + @" as (?:the )?(?:input|argument) (?:for|of|to) (?:the |an? )?" + Case + @"\z", Options),
        new(@"\A(?:an? )?" + Case + @" with " + Input + @" as input\z", Options),
        new(@"\Ai have selected " + Input + @" for conversion (?:to|into) " + Case + @"\z", Options),
        new(@"\A" + Case + @"(?: is)? (?:the requested operation|requested)(?: for " + Input + @")?\z", Options),
        new(@"\A(?:the )?" + Case + @" should take " + Input + @" as (?:its|the) input\z", Options),
        new(@"\A(?:the )?case is to be " + Case + @" for " + Input + @"\z", Options),
        new(@"\A" + Input + @" (?:needs|is to be) " + Case + @"\z", Options),
        new(@"\Ause " + Input + @" for (?:an? )?" + Case + @"\z", Options),
        new(@"\Ahow (?:does|would) " + Input + @" look after " + Case + @"\z", Options),
        // Predicate and output relationships are explicit. Numeric descriptions
        // are consumed only inside operands, never silently as output modifiers.
        new(@"\A" + Case + @" is (?:requested|desired) for " + Input + @"\z", Options),
        new(@"\A(?:change|set) (?:the )?case of " + Input + @" to " + Case + @"\z", Options),
        new(@"\A(?:give(?: me)?|show|return|provide|supply) (?:this|the supplied) character (?:in|as) " + Case + @": " + Input + @"\z", Options),
        new(@"\Amake the supplied character " + Case + @": " + Input + @"\z", Options),
        new(@"\Awhat would (?:the )?" + Case + @" of " + Input + @" be\z", Options),
        new(@"\A" + Case + @"(?: " + Input + @")?\z", Options),
        new(@"\A(?:convert|change|turn|transform|take|switch) (?:" + Input + @" )?(?:to|into) (?:an? |its )?" + Case + @"\z", Options),
        new(@"\Aset (?:the )?case of " + Input + @" to " + Case + @"\z", Options),
        new(@"\A(?<output>" + OutputVerb + @") (?:" + Input + @" " + Manner + @")?" + Case + @"\z", Options),
        new(@"\A(?:give(?: me)?|show(?: me)?|return|display|use|provide|supply|select|choose|find) (?:the |an? )?" + Case + @"(?: (?:of|for|corresponding to) " + Input + @")?\z", Options),
        new(@"\A(?:what is|what's|what’s|(?:may|can|could) i (?:have|get)) (?:the |an? )?" + Case + @"(?: of " + Input + @")?\z", Options),
        new(@"\A(?:" + Desire + @"|my request is) (?:the |an? )?" + Case + @"(?: (?:of|for) " + Input + @")?\z", Options),
        new(@"\A" + Desire + @" " + Input + @" " + Manner + Case + @"\z", Options),
        new(@"\A(?:what is|what's|what’s|(?:can|could|may) i (?:have|get|see)) " + Input + @" " + Manner + Case + @"\z", Options),
        new(@"\A(?:the|an?) " + Case + @"(?: of " + Input + @")?\z", Options),
        new(@"\Afor " + Input + @", (?:(?:give|show|return|use|perform|apply)(?: me)?|" + Desire + @") (?:its |the )?" + Case + @"\z", Options),
        new(@"\Ahow (?:does|would|will) " + Input + @" look (?:in|as) " + Case + @"\z", Options),
        new(@"\Awith " + Input + @" as (?:the |my )?input, (?:return|give|show)(?: me)? (?:its |the )?" + Case + @"\z", Options),
        // A bounded declaration supplies one operand to one operation, not a
        // substring extracted from arbitrary prose or a second executable step.
        new(@"\Athe (?:character|letter|input) is " + Input + @"[;. ]+ (?:please )?" + Case + @" it\z", Options),
        new(@"\A" + Input + @" is (?:my|the) input[;. ]+ (?:please )?(?:convert|change|turn) it (?:to|into) " + Case + @"\z", Options),
        new(@"\Alet " + Input + @" be (?:the|my) input; (?<output>" + OutputVerb + @") (?:it|that) " + Manner + Case + @"\z", Options),
        new(@"\A(?<modal>can|could|would) " + Input + @" be " + Manner + Case + @"\z", Options),
        new(@"\A" + Input + @" (?:needs to be|should be|must be) " + Case + @"\z", Options),
        new(@"\Athe (?:case )?operation (?:should|must) be " + Case + @"\z", Options),
        new(@"\A(?:use|apply|perform|run|do) (?:the |an? )?" + Case + @"(?: (?:for|to|on) " + Input + @")?\z", Options),
        new(@"\Awhich " + Case + @" corresponds to " + Input + @"\z", Options),
        new(@"\Awhat do i get when i " + Case + @" " + Input + @"\z", Options),
        new(@"\Ause " + Input + @" as input for " + Case + @"\z", Options),
        new(@"\A(?:an? )?" + Case + @" is what i (?:need|want)\z", Options),
        // Compose an explicit input label with a single action or direction
        // label. Numeric declarations retain their codepoint type downstream.
        new(@"\A" + Declaration + @"[;.:] (?:please )?(?:convert|change|turn) (?:it|this(?: one)? character) (?:to|into) " + Case + @"\z", Options),
        new(@"\A" + Declaration + @"[;.:] (?:please )?(?:apply|perform|use) " + Case + @"(?: to it)?\z", Options),
        new(@"\A" + Declaration + @"[;.:] (?:please )?" + Case + @" it\z", Options),
        new(@"\A" + Declaration + @"[;.:] i (?:need|want) it " + Manner + Case + @"\z", Options),
        new(@"\A" + Declaration + @"[;.:] i(?: would|['’]d) (?:like|prefer) it " + Manner + Case + @"\z", Options),
        new(@"\A" + Declaration + @"[;.:] (?:(?:the )?(?:requested )?(?:case )?operation(?: is |: ?)|desired case: ?)" + Case + @"\z", Options),
        new(@"\A" + NumericDeclaration + @"[;.:] (?:please )?(?:convert|change|turn) it (?:to|into) " + Case + @"\z", Options),
        new(@"\A" + NumericDeclaration + @"[;.:] (?:please )?(?:apply|perform|use) " + Case + @"\z", Options)
    ];
    private static readonly Regex[] DirectionlessForms = [
        MissingDirection,
        new(@"\A" + Declaration + @"\z", Options),
        new(@"\A" + NumericDeclaration + @"\z", Options),
        new(@"\Ause " + Input + @"\z", Options),
        new(@"\Aprocess " + Input + @"\z", Options),
        new(@"\Ai have selected " + Input + @"\z", Options),
        new(@"\Athe character (?:i'd|i’d|i would) like you to work on is " + Input + @"\z", Options),
        new(@"\Ahere is " + Input + @": (?:please )?change its case\z", Options),
        new(@"\Ahere is " + Input + @" for a case operation\z", Options),
        new(@"\Athe (?:uppercase|lowercase) input could be " + Input + @"\z", Options)
    ];
    private static readonly Regex[] AmbiguousDirectionForms = [
        new(@"\A(?:should " + Input + @" be made|set " + Input + @" to) (?:upper(?:[ -]?case)?|lower(?:[ -]?case)?) or (?:upper(?:[ -]?case)?|lower(?:[ -]?case)?)\z", Options),
        new(@"\A(?:use|apply|perform) (?:upper(?:[ -]?case)?|lower(?:[ -]?case)?) or (?:upper(?:[ -]?case)?|lower(?:[ -]?case)?) (?:on|to|for) " + Input + @"\z", Options),
        new(@"\A" + OutputVerb + @" " + Input + @" (?:in|as) (?:upper(?:[ -]?case)?|lower(?:[ -]?case)?) or (?:upper(?:[ -]?case)?|lower(?:[ -]?case)?)\z", Options),
        new(@"\Afor " + Input + @", choose (?:uppercase or lowercase|lowercase or uppercase)\z", Options),
        new(@"\Amy case operation for " + Input + @" is (?:upper/lower|lower/upper)\z", Options)
    ];

    private static string OperandText(Match match) => (match.Groups["radix"].Success ? "hexadecimal code point "
        : match.Groups["numeric"].Success ? "code point " : "") + match.Groups["input"].Value.Trim();
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
        if (BareNumber.IsMatch(text) || text.Length == 1)
            return text.Length == 1 && text[0] > 255 ? Abstain("input_domain") : Clarify();
        var exact = Canonical.Match(text);
        if (exact.Success)
        {
            var value = exact.Groups["input"].Value;
            return byte.TryParse(value, NumberStyles.None, CultureInfo.InvariantCulture, out var key)
                && key.ToString(CultureInfo.InvariantCulture) == value ? Ready(exact.Groups["op"].Value, key) : Abstain("input_domain");
        }
        if (text.EndsWith('?') || text.EndsWith('.')) text = text[..^1].TrimEnd();
        text = ContextPrefix.Replace(text, "", 1);
        text = PolitePrefix.Replace(text, "", 1);
        text = PoliteSuffix.Replace(text, "", 1);
        text = PreferenceSuffix.Replace(text, "", 1);
        var ambiguity = AmbiguitySuffix.Match(text);
        if (ambiguity.Success) text = text[..ambiguity.Index];
        if (CompoundOrNegated.IsMatch(text)) return Abstain("unsupported_intent");
        foreach (var form in AmbiguousDirectionForms)
        {
            var match = form.Match(text);
            if (!match.Success) continue;
            var operand = ProposeOperand("upper", OperandText(match));
            return operand.Status == "abstain" ? operand : Clarify();
        }
        foreach (Match operation in OperationLexeme.Matches(text))
        {
            var operationName = operation.Groups["op"].Value;
            var framed = text[..operation.Index] + '\u001f' + text[(operation.Index + operation.Length)..];
            if (ProposeFields(operationName, framed) is { } fields)
                return ambiguity.Success && fields.Status == "ready" ? Clarify() : fields;
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
                var proposal = ProposeOperand(operationName, OperandText(match));
                // Preserve verb/adjective evidence lost by the direction marker.
                // Showing a property is not applying a conversion; a modal
                // adjective question permits both readings and must clarify.
                if (proposal.Status == "ready" && !operationName.EndsWith("ed", StringComparison.OrdinalIgnoreCase)
                    && !match.Groups["transform"].Success)
                {
                    if (match.Groups["passive"].Success && match.Groups["output"].Value.Equals("show", StringComparison.OrdinalIgnoreCase))
                        return Abstain("unsupported_intent");
                    if (match.Groups["modal"].Success) return Clarify();
                }
                return ambiguity.Success && proposal.Status == "ready" ? Clarify() : proposal;
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
        if (MatchInputField(text) is { } declared)
        {
            var operand = ProposeOperand("upper", declared);
            return operand.Status == "abstain" ? operand : Clarify();
        }
        return Abstain("unsupported_intent");
    }

    private static string? MatchInputField(string text)
    {
        foreach (var field in InputFields)
        {
            var match = field.Match(text);
            if (match.Success) return OperandText(match);
        }
        return null;
    }

    private static bool IsOperationField(string text) => OperationFields.Any(field => field.IsMatch(text)) || ActionField.IsMatch(text);

    private static LearningTaskProposal? ProposeFields(string operation, string text)
    {
        // Two bounded fields, exactly one of each role, in either order. A
        // separator is only a candidate boundary: both complete sides must
        // validate, so punctuation inside data cannot discard a trailing action.
        foreach (Match separator in FieldSeparator.Matches(text))
        {
            var left = text[..separator.Index];
            var right = text[(separator.Index + separator.Length)..];
            if (IsOperationField(right) && MatchInputField(left) is { } input)
                return ProposeOperand(operation, input);
            if (IsOperationField(left) && MatchInputField(right) is { } reverse)
                return ProposeOperand(operation, reverse);
        }
        return IsOperationField(text) ? Clarify() : null;
    }

    private static LearningTaskProposal ProposeOperand(string operation, string token, bool allowAlternatives = true)
    {
        // Strip only the bounded operand description, never normalize the scalar.
        token = InputDescription.Replace(token, "", 1);
        var quoted = QuotedDescription.Match(token);
        if (quoted.Success) token = quoted.Groups["input"].Value;
        var fullStop = QuotedFullStop.Match(token);
        if (fullStop.Success) token = fullStop.Groups["input"].Value;
        var written = ScalarWritten.Match(token);
        if (written.Success) token = written.Groups["input"].Value;
        var explicitScalar = ExplicitScalarSuffix.Match(token);
        if (explicitScalar.Success) token = explicitScalar.Groups["input"].Value;
        var decimalRelation = DecimalRelation.Match(token);
        if (decimalRelation.Success) token = "code point " + decimalRelation.Groups["input"].Value;
        // Relational descriptions require an explicit codepoint, never a guessed
        // byte from a location or an incomplete article such as "letter with a".
        var relation = CodepointRelation.Match(token);
        if (relation.Success) token = relation.Groups["input"].Value;
        if (token.Length == 0 || MissingScalar.IsMatch(token)) return Clarify();
        if (token.Length == 3 && (token[0] == '\'' && token[2] == '\'' || token[0] == '"' && token[2] == '"'))
            return token[1] <= 255 ? Ready(operation, (byte)token[1]) : Abstain("input_domain");
        if (QuotedString.IsMatch(token)) return token.Length == 2 ? Clarify() : Abstain("unsupported_intent");
        if (token.Length == 1 && token[0] <= 255 && !char.IsLetter(token[0])) return Clarify();
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
