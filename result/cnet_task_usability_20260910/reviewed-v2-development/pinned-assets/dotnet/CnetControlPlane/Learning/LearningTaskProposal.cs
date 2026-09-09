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
    private static readonly Regex OperationLexeme = new(@"\b(?<op>upper(?:[ -]?cas(?:e|ing|ed))?|lower(?:[ -]?cas(?:e|ing|ed))?|(?:up|down)cas(?:e|ing|ed)|capital(?:[ -]letter)?s?|capitali[sz](?:e(?:d)?|ation)|uncapitali[sz](?:e(?:d)?|ing)|small[ -]letters?|all caps)\b", Options);
    // Separate automata avoid the operation × noun state-space product.
    private static readonly Regex OperationNoun = new(@"(?<noun>[ -](?:form|version|equivalent|lettering|letter|character|case(?: conversion)?|conversion|casing|rendition|result|operation|transformation|counterpart|mapping))", Options);
    private const string OutputVerb = @"(?:make|put|write|render|return|show(?: me)?|display|supply|provide|produce|present|express|give(?: me)?)";
    private const string Desire = @"(?:i (?:need|want|require|request|am requesting|am asking for|would like|would prefer)|i['’]d (?:like|prefer)|i['’]m (?:asking for|requesting))";
    private const string Manner = @"(?<passive>to be )?(?:(?<transform>converted|changed|put|written|rendered|made|shown|expressed) )?(?:(?:in|as|into|using|with|to) )?(?:an? |its )?";
    private const string Relation = @"(?:of|for|from|(?<counterpartTo>to)|applied to|corresponding to|associated with|produced from|obtained from)";
    private const string Input = @"(?<input>.+?)";
    private const string InputLabel = @"(?:(?:here is )?(?:the |my )?input(?: scalar| character)?|(?:the )?(?:chosen |selected |supplied )?character(?: supplied)?)";
    private const string Declaration = InputLabel + @"(?: is |: ?)" + Input;
    private const string NumericDeclaration = @"(?:the |my )?input (?<numeric>code[ -]?point)(?: is |: ?)" + Input;
    private const string Alternative = @"(?:(?:the )?(?:single )?(?:character|letter|scalar) )?(?:'.'|"".""|(?:(?:the )?code[ -]?point )?(?:U\+|0x)[0-9a-f]{1,6}|(?:decimal )?code[ -]?point [0-9]{1,6}|[0-9]+(?:\.[0-9]+)?|[^\s])";
    private static readonly Regex Canonical = new(@"\Aunicode (?<op>upper|lower) (?<input>[0-9]{1,3})\z", Options);
    private static readonly Regex ContextPrefix = new(@"\Afor (?:this|the) (?:request|task|conversion), ", Options);
    private static readonly Regex[] PolitePrefixes = [
        new(@"\A(?:(?:can|could|would|will) you (?:mind )?(?:be able to )?(?:please )?|how (?:would|do) you )", Options),
        new(@"\A(?:(?:i would|i'd|i’d) like (?:you to |to (?:see )?)|i (?:need|want) you to |i want to (?:see )?)", Options),
        new(@"\A(?:(?:my request|the task) is to |the result i need is |this request concerns |for me, (?:please )?|please |kindly )", Options)
    ];
    private static readonly Regex SeePrefix = new(@"\Alet me see ", Options);
    private static readonly Regex PoliteSuffix = new(@"(?: for me(?:,? please)?|,? please)\z", Options);
    private static readonly Regex ResultSuffix = new(@" for my result\z", Options);
    private static readonly Regex PreferenceSuffix = new(@" is (?:requested|what i (?:need|want)|the one i want|what i'm after|what i’m after)\z", Options);
    private static readonly Regex AmbiguitySuffix = new(@"(?:, whichever(?: i meant| you (?:prefer|choose))?|; i haven['’]t chosen which input yet|; either is a possible input)\z", Options);
    private static readonly Regex InputDescription = new(@"\A(?:only )?(?:the |my |an? |this |that )?(?:single |single-character |one-character |supplied |provided |chosen |selected |quoted |literal )?(?:(?:unicode|latin1|currency) )?(?:character|letter|scalar|digit|symbol|input(?: scalar| character| letter)?)(?:: ?| )", Options);
    private static readonly Regex LeadingModifier = new(@"\A(?:only )?(?:the |an? |this |that )?(?:single|single-character|one-character|supplied|provided|chosen|selected|quoted|literal) ", Options);
    private static readonly Regex QuotedDescription = new(@"\A(?:the )?quoted literal (?<input>.+)\z", Options);
    private static readonly Regex ShortDescription = new(@"\A(?:the )?(?:literal|supplied|provided) (?<input>.+)\z", Options);
    private static readonly Regex UnicodeCodepoint = new(@"\A(?:the )?Unicode (?<input>(?:decimal |hexadecimal |hex )?code[ -]?point .+)\z", Options);
    private static readonly Regex QuotedFullStop = new(@"\A(?:the )?quoted full stop (?<input>'\.'|""\."")\z", Options);
    private static readonly Regex QuotedNonbreakingSpace = new(@"\A(?:the )?quoted nonbreaking space (?<input>'\u00A0'|""\u00A0"")\z", Options);
    private static readonly Regex QuotedSpace = new(@"\A(?:the )?quoted space (?<input>' '|"" "")\z", Options);
    private static readonly Regex ScalarWritten = new(@"\A(?:written|identified as|specified as) (?<input>(?:U\+|0x).+)\z", Options);
    private static readonly Regex ShownInput = new(@"\Ashown as (?<input>.+)\z", Options);
    private static readonly Regex ByteInput = new(@"\A(?:the |my |an? |this |that )?(?:single |quoted |literal |supplied |provided )?byte(?: (?<input>.+))?\z", Options);
    private static readonly Regex NamedInput = new(@"\A(?:the |a |an |this |that |my )?(?:quoted |literal )?(?<name>hyphen|asterisk|space|plus sign|semicolon|punctuation)(?: character)?(?:(?:: ?| )(?<input>.+))?\z", Options);
    private static readonly Regex PluralInput = new(@"\A(?:the )?(?:letters|characters|scalars) (?<input>.+)\z", Options);
    private static readonly Regex DecimalRelation = new(@"\Awhose decimal value is (?<input>[0-9]+)\z", Options);
    private static readonly Regex ExplicitScalarSuffix = new(@"\A(?<input>(?:U\+|0x)[0-9a-f]{1,6}), interpreted as a Unicode scalar,?\z", Options);
    private static readonly Regex CodepointRelation = new(@"\A(?:with|at|represented by|specified by|encoded by|whose) (?<input>(?:decimal |hexadecimal |hex )?code[ -]?point .+|U\+.+|0x.+)\z", Options);
    private static readonly Regex NumberedDecimal = new(@"\Anumbered (?<input>[0-9]+) in decimal code[ -]?point notation\z", Options);
    private static readonly Regex DecimalInput = new(@"\A(?:the )?(?:decimal )?code[ -]?point (?:is )?(?:decimal )?(?<number>[0-9]+)(?: in decimal| \(decimal\))?\z", Options);
    private static readonly Regex HexInput = new(@"\A(?:(?:the )?(?:hex|hexadecimal) code[ -]?point (?:(?:U\+|0x))?|(?:(?:the )?code[ -]?point (?:is )?)?(?:U\+|0x))(?<number>[0-9a-f]{1,6})\z", Options);
    private static readonly Regex DeclaredHexSuffix = new(@"\A(?<input>(?:U\+|0x)[0-9a-f]{1,6}) in hexadecimal\z", Options);
    private static readonly Regex InputHexAside = new(@"\A(?<input>(?:(?:the )?code[ -]?point )?(?:U\+|0x)[0-9a-f]{1,6}), in hexadecimal,?\z", Options);
    private static readonly Regex CodepointPrefix = new(@"\A(?:(?:the )?(?:decimal |hex |hexadecimal )?code[ -]?point\b|U\+|0x)", Options);
    private static readonly Regex BareNumber = new(@"\A[+-]?[0-9]+(?:\.[0-9]+)?\z", Options);
    private static readonly Regex MissingScalar = new(@"\A(?:the |an? |one )?(?:single )?(?:character|letter|codepoint)\z", Options);
    private static readonly Regex UnspecifiedReference = new(@"\Ai have in mind\z", Options);
    private static readonly Regex MissingDirection = new(@"\A(?:(?:change|adjust|set) (?:the )?(?:letter )?case(?: (?:of|(?<partialRelation>for|on)) " + Input + @")?|case-convert " + Input
        + @"|(?:convert|change) " + Input + @"(?: to (?:the )?(?:requested |desired )?case)?|apply (?:a )?case (?:conversion|operation)(?: to " + Input + @")?)\z", Options);
    private static readonly Regex MissingOperand = new(@"\A(?:" + OutputVerb + @"|give(?: me)?|show(?: me)?|display) an? " + Case + @"\z", Options);
    private static readonly Regex CompoundOrNegated = new(@"\b(?:not|then|also)\b", Options);
    private static readonly Regex AlternativeToken = new(Alternative, Options);
    private static readonly Regex AlternativeSeparator = new(@" *, *(?:(?:and|or) +)?(?:as +)?| +(?:(?:and|or) +)?(?:as +)?", Options);
    private static readonly Regex HasConjunction = new(@"\b(?:and|or)\b", Options);
    private static readonly Regex QuotedString = new(@"\A(?:'[^']*'|""[^""]*"")\z", Options);
    private static readonly Regex UnsupportedOperand = new(@"(?:\b(?:using|locale|rules|string|word|name|sentence|paragraph|text|phrase|sequence|pair|control)\b|\bboth letters\b|\baccording to\b|\bspecific\b|;)", Options);
    private static readonly Regex FieldSeparator = new(@"[;.,] (?:and |with )?| and ", Options);
    private static readonly Regex PartialFieldSeparator = new(@"[;,] *", Options);
    private static readonly Regex PartialOperationField = new(@"\A(?:(?:convert|change|turn|switch|transform) (?:to|into) " + Case
        + @"|(?:use|apply|perform) (?:the |an? )?" + Case + @"(?: (?:to|on|for):?)?"
        + @"|" + Case + @" (?:one|a|an) (?:single )?(?:character|letter|input)"
        + @"|make (?:one|a|an) (?:single )?(?:character|letter|input) " + Case + @")\z", Options);
    private static readonly Regex AlternativeInputField = new(@"\A(?:the |my )?(?:(?:candidate input|input (?:candidates|possibilities))(?: is |: ?)|input (?:could|may|might) be |choose between )" + Input + @"\z", Options);
    private const string Reference = @"(?:it|this(?: one)? character|that(?: character)?)";
    private static readonly Regex ReferenceUse = new(@"\b(?:" + Reference + @"|its)\b", Options);
    private static readonly Regex ReferenceOperand = new(@"\A" + Reference + @"\z", Options);
    // Carries only validated input/status evidence, never a selected operation
    // or dataset from the shared operand-validation call. Source text retains
    // the declaration and its explicit representation qualifiers.
    private sealed record InputBinding(byte? OriginalByte, string Status, string Code, string SourceText);
    private static readonly Regex[] OperationFields = [
        new(@"\Athe transformation i am requesting is " + Case + @"\z", Options),
        new(@"\A(?:the )?conversion i want is " + Case + @"\z", Options),
        new(@"\Aoperation requested: " + Case + @"\z", Options),
        new(@"\A(?:the |my |its )?(?:desired |requested |target |chosen |preferred )?(?:output )?(?:case(?: choice| conversion| operation| i need)?|operation|transformation)(?: is(?: to be)? |: ?| selected: ?| should be )" + Case + @"\z", Options),
        new(@"\A" + Case + @" (?:is (?:the )?(?:requested |chosen )?(?:case|operation)|as the (?:chosen )?(?:case|operation)|for the case)\z", Options),
        new(@"\A(?:i (?:request|want|need) (?:it |its |an? |the )?|(?:please )?(?:use|apply|request|select|perform|do) (?:an? )?|needs )" + Case + @"\z", Options),
        new(@"\Athe (?:case|operation) " + Case + @"\z", Options),
        new(@"\A(?:its |an? |for (?:an? )?)?" + Case + @"\z", Options),
        new(@"\Amy preferred operation is to make an? " + Case + @"\z", Options)
    ];
    private static readonly Regex[] ActionFields = [
        new(@"\A" + Desire + @" (?:an? )?" + Case + @" applied to it\z", Options),
        new(@"\A(?:please )?(?:(?:put|write|render|make) it (?:(?:in|as|into) )?" + Case + @"|" + Case + @" (?:it|its character))\z", Options),
        new(@"\A(?:please )?(?:" + OutputVerb + @"|what is) its " + Case + @"\z", Options),
        new(@"\A(?:may|can|could) i (?:have|get|see) its " + Case + @"\z", Options),
        new(@"\A(?:please )?(?:convert|change|turn) it (?:to|into) " + Case + @"\z", Options),
        new(@"\A(?:i would|i['’]d) like its (?:character )?" + Case + @"\z", Options)
    ];
    private static readonly Regex[] InputFields = [
        new(@"\A(?:the |my )?input(?: character| scalar)? for this task(?: is |: ?)" + Input + @"\z", Options),
        new(@"\A(?:the )?(?:literal )?character i['’]m giving you is " + Input + @"\z", Options),
        new(@"\A(?:the )?selected input has (?<numeric>decimal code[ -]?point) " + Input + @"\z", Options),
        new(@"\Ai have supplied " + Input + @"\z", Options),
        new(@"\Athis request has (?:the )?input " + Input + @"\z", Options),
        new(@"\Athe (?<numeric>decimal code[ -]?point) of my input is " + Input + @"\z", Options),
        new(@"\Athis task has the input character " + Input + @"\z", Options),
        new(@"\Ai['’]m specifying the input " + Input + @"\z", Options),
        new(@"\Ai have a quoted input character: " + Input + @"\z", Options),
        new(@"\A(?:the |my )?input (?:for my request|for this conversion|i want to use) is " + Input + @"\z", Options),
        new(@"\Ai(?: have|['’]ve) chosen " + Input + @" as the input\z", Options),
        new(@"\Amy one-character input is " + Input + @"\z", Options),
        new(@"\A(?:the )?(?<numeric>code[ -]?point) i am giving you is " + Input + @"\z", Options),
        new(@"\A(?:the )?input character has (?<numeric>decimal code[ -]?point) " + Input + @"\z", Options),
        new(@"\A(?:(?:here|let) )?(?:the |my )?(?:chosen |selected |specified |supplied |provided |single-character |quoted |literal )?input(?: scalar| character| for this task| for conversion| i chose)?(?: is |: ?| be )" + Input + @"\z", Options),
        new(@"\A(?:(?:here|let) )?(?:the |my )?(?:chosen |selected |specified |supplied |provided |single-character |quoted |literal )?(?:character(?: to work on| for this task| i've selected)?|scalar(?: i'm providing)?)(?: is |: ?| be )" + Input + @"\z", Options),
        new(@"\A" + Input + @" is (?:my|the) input(?: character)?\z", Options),
        new(@"\Afor " + Input + @"\z", Options),
        new(@"\A(?:with|given) " + Input + @" as (?:my |the )?(?:one-character )?input(?: character)?\z", Options),
        new(@"\Agiven " + Input + @"\z", Options),
        new(@"\A(?:here is )?(?:the |my )?input (?<numeric>code[ -]?point)(?: is |: ?)" + Input + @"\z", Options),
        new(@"\Amy selected scalar has (?<numeric>decimal code[ -]?point) " + Input + @"\z", Options),
        new(@"\Ai have " + Input + @" as my input\z", Options),
        new(@"\A" + Input + @" as the supplied character\z", Options),
        new(@"\A(?:please )?(?:use|take) " + Input + @" (?:for|as) the (?:input(?: character| scalar)?|character)\z", Options),
        new(@"\A(?<inputUse>use|take) " + Input + @"\z", Options),
        new(@"\Ahere is the input " + Input + @"\z", Options),
        new(@"\Afor the input, i have " + Input + @"\z", Options),
        new(@"\Athe (?<numeric>decimal code point) i am supplying is " + Input + @"\z", Options),
        new(@"\Athe (?<radix>hexadecimal) input code point is " + Input + @"\z", Options),
        new(@"\A(?:the )?(?<numeric>code[ -]?point)(?: for this task)? is " + Input + @"\z", Options),
        new(@"\A(?:my )?chosen scalar has (?<numeric>code[ -]?point) " + Input + @"\z", Options),
        new(@"\Acode point, (?<radix>hexadecimal): " + Input + @"\z", Options),
        new(@"\A(?:the )?input " + Input + @" is a single scalar\z", Options),
        new(@"\Ai am supplying " + Input + @" as the character\z", Options)
    ];
    private static readonly Regex[] Forms = [
        // Explicitly unresolved slots remain non-executable. The candidate list
        // still traverses the shared operand/domain validator.
        new(@"\Aconvert to " + Case + @" (?<unresolved>whichever) (?:single )?(?:letter|character) is intended, " + Input + @"\z", Options),
        new(@"\Ai (?:still )?need " + Case + @" applied, yet no (?:letter|character|input) has been given\z", Options),
        new(@"\Awhich (?<alias>capital letter|small letter) is the " + Case + @" of " + Input + @"\z", Options),
        new(@"\A(?:the |my )?(?:case transformation|requested operation) for " + Input + @" (?:should be|is) " + Case + @"\z", Options),
        new(@"\Athe " + Case + @" i am requesting uses " + Input + @"\z", Options),
        new(@"\A" + Desire + @" (?:the |an? )?" + Case + @" that (?:goes with|corresponds to) " + Input + @"\z", Options),
        new(@"\A" + OutputVerb + @" (?:the |an? )?" + Case + @" that (?:goes with|corresponds to) " + Input + @"\z", Options),
        // Only causative make binds the first object as the conversion input.
        // Give/show/supply could instead make it the recipient of a result.
        new(@"\Amake " + Input + @" the corresponding " + Case + @"\z", Options),
        new(@"\A" + Input + @" is the one i want " + Case + @"\z", Options),
        new(@"\Awhat " + Case + @" would result from " + Input + @"\z", Options),
        new(@"\Athe " + Case + @" i need is the one for " + Input + @"\z", Options),
        new(@"\A" + OutputVerb + @" " + Input + @" after " + Case + @"\z", Options),
        new(@"\Ai have supplied " + Input + @" for (?:an? )?" + Case + @"\z", Options),
        new(@"\A(?:the )?(?:character|letter) to put into " + Case + @" is " + Input + @"\z", Options),
        new(@"\A" + Case + @" is the desired case for " + Input + @"\z", Options),
        new(@"\A(?<inputCase>could|can|would) " + Input + @" receive (?:an? )?" + Case + @"\z", Options),
        new(@"\A(?:show|display) what " + Input + @" (?:becomes|would become) (?:under|after) " + Case + @"\z", Options),
        new(@"\A(?:the |my )?(?:requested case|case requested|operation i want) for " + Input + @" is (?:an? )?" + Case + @"\z", Options),
        new(@"\Afor " + Input + @", " + Case + @" is the operation i want\z", Options),
        new(@"\Alet " + Input + @" appear in " + Case + @"\z", Options),
        new(@"\A" + Desire + @" " + Input + @" to appear in " + Case + @"\z", Options),
        new(@"\Athe (?:character|letter|scalar) i(?: would|['’]d) like " + Case + @" is " + Input + @"\z", Options),
        new(@"\A(?<inputCase>my|the) " + Case + @" should use " + Input + @"\z", Options),
        new(@"\A(?:the|this) " + Case + @" task uses " + Input + @"\z", Options),
        new(@"\A" + Case + @"(?: (?:for|on|when converting)| is wanted for| is what i want for) " + Input + @"\z", Options),
        new(@"\A(?:" + OutputVerb + @"|show me) (?:the |an? )?" + Case + @" " + Relation + @" " + Input + @"\z", Options),
        new(@"\A(?:show(?: me)?|give(?: me)?|return) (?:the |an? )?" + Case + @" " + Input + @"\z", Options),
        new(@"\A" + Desire + @" (?:the |an? )?" + Case + @" " + Relation + @" " + Input + @"\z", Options),
        new(@"\A(?:could|can|may) i see (?:the |an? )?" + Case + @" of " + Input + @"\z", Options),
        new(@"\A" + Input + @" (?:should appear in|is to be converted to|should receive|needs its case set to) (?:the )?" + Case + @"\z", Options),
        new(@"\Ahow is " + Input + @" written in " + Case + @"\z", Options),
        new(@"\Awhat " + Case + @" (?:would i get from|matches|comes from) " + Input + @"\z", Options),
        new(@"\A(?:the )?(?:requested )?transformation(?: requested)? (?:of|for) " + Input + @" is " + Case + @"\z", Options),
        new(@"\A(?:the |my )?(?:character|input) for " + Case + @" is " + Input + @"\z", Options),
        new(@"\A(?<inputCase>my) " + Case + @" input is " + Input + @"\z", Options),
        new(@"\A(?:use|apply) " + Case + @" when converting " + Input + @"\z", Options),
        new(@"\A" + Input + @" is the input for this " + Case + @"\z", Options),
        new(@"\A" + OutputVerb + @" " + Input + @" after applying (?:the )?" + Case + @"\z", Options),
        new(@"\A" + Case + @" is the case i want for " + Input + @"\z", Options),
        new(@"\A(?:can i see how|how would) " + Input + @" (?:appear|appears) in " + Case + @"\z", Options),
        new(@"\Awhat would " + Input + @" (?:be|become) in " + Case + @"\z", Options),
        new(@"\A(?:the )?(?:desired case|operation requested) for " + Input + @" is " + Case + @"\z", Options),
        new(@"\A" + Case + @" is to be applied to " + Input + @"\z", Options),
        new(@"\Ai am supplying " + Input + @" for (?:an? )?" + Case + @" request\z", Options),
        new(@"\Amy " + Case + @" request has the quoted input " + Input + @"\z", Options),
        new(@"\A" + Input + @" is my input for (?:an? )?" + Case + @"\z", Options),
        new(@"\A" + Input + @" is to receive " + Case + @"\z", Options),
        new(@"\A(?:the )?(?:single )?(?:character|letter|thing) (?:to|i want in|i am asking you to) " + Case + @" is " + Input + @"\z", Options),
        new(@"\A(?:the )?conversion i need is " + Case + @" " + Input + @"\z", Options),
        new(@"\A(?:for|in) (?:an? )?" + Case + @", (?:(?:please )?(?:use|convert)|what is) " + Input + @"\z", Options),
        new(@"\Afor " + Input + @", what would " + Case + @" be\z", Options),
        new(@"\A(?:which|what) " + Case + @" (?:goes with|results from|is associated with) " + Input + @"\z", Options),
        new(@"\A(?<which>which) should i " + Case + @", " + Input + @"\z", Options),
        new(@"\A" + Desire + @" " + Input + @" mapped to " + Case + @"\z", Options),
        new(@"\A(?:use|take) " + Input + @" as (?:the )?(?:input|argument) (?:for|of|to) (?:the |an? )?" + Case + @"\z", Options),
        new(@"\A(?:an? )?" + Case + @" with " + Input + @" as input\z", Options),
        new(@"\Ai have selected " + Input + @" for conversion (?:to|into) " + Case + @"\z", Options),
        new(@"\A" + Case + @"(?: is)? (?:the requested operation|requested)(?: (?:for|on) " + Input + @")?\z", Options),
        new(@"\A(?:the )?" + Case + @" should take " + Input + @" as (?:its|the) input\z", Options),
        new(@"\A(?:the )?case is to be " + Case + @" for " + Input + @"\z", Options),
        new(@"\A" + Input + @" (?:needs|is to be) " + Case + @"\z", Options),
        new(@"\A(?<inputUse>use|take) " + Input + @" for (?:an? |my |the |this )?" + Case + @"\z", Options),
        new(@"\Ahow (?:does|would) " + Input + @" look after " + Case + @"\z", Options),
        // Predicate and output relationships are explicit. Numeric descriptions
        // are consumed only inside operands, never silently as output modifiers.
        new(@"\A" + Case + @" is (?:requested|desired|my request) for " + Input + @"\z", Options),
        new(@"\A(?:change|set) (?:the )?case of " + Input + @" to " + Case + @"\z", Options),
        new(@"\A(?:give(?: me)?|show|return|provide|supply) (?:this|the supplied) character (?:in|as) " + Case + @": " + Input + @"\z", Options),
        new(@"\Amake the supplied character " + Case + @": " + Input + @"\z", Options),
        new(@"\Awhat would (?:the )?" + Case + @" of " + Input + @" be\z", Options),
        new(@"\A" + Case + @"(?: " + Input + @")?\z", Options),
        new(@"\A(?:convert|change|turn|transform|take|switch|rewrite|recast|map|fold|bring) (?:" + Input + @" )?(?:to|into|onto|using) (?:an? |its |the corresponding )?" + Case + @"\z", Options),
        new(@"\Aset (?:the )?case of " + Input + @" to " + Case + @"\z", Options),
        new(@"\A(?<output>" + OutputVerb + @") (?:" + Input + @" " + Manner + @")?" + Case + @"\z", Options),
        new(@"\A(?:give(?: me)?|show(?: me)?|tell me|return|display|use|provide|supply|select|choose|find) (?:the |an? )?" + Case + @"(?: (?:of|for|corresponding to|associated with) " + Input + @")?\z", Options),
        new(@"\A(?:what is|what's|what’s|(?:may|can|could) i (?:have|get)) (?:the |an? )?" + Case + @"(?: " + Relation + @" " + Input + @")?\z", Options),
        new(@"\A(?:" + Desire + @"|my request is) (?:the |an? )?" + Case + @"(?: (?:of|for) " + Input + @")?\z", Options),
        new(@"\A" + Desire + @" " + Input + @" " + Manner + Case + @"\z", Options),
        new(@"\A(?:what is|what's|what’s|(?:can|could|may) i (?:have|get|see)) " + Input + @" " + Manner + Case + @"\z", Options),
        new(@"\A(?:the|an?) " + Case + @"(?: of " + Input + @")?\z", Options),
        new(@"\Afor " + Input + @", (?:(?:give|show|return|use|perform|apply)(?: me)?|" + Desire + @") (?:its |the |an? )?" + Case + @"\z", Options),
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
        new(@"\A(?:use|apply|perform|run|do|carry out|force) (?:the |an? )?" + Case + @"(?: (?:for|to|on) " + Input + @")?\z", Options),
        new(@"\Awhich " + Case + @" corresponds to " + Input + @"\z", Options),
        new(@"\Awhat do i get when i " + Case + @" " + Input + @"\z", Options),
        new(@"\Ause " + Input + @" as input for " + Case + @"\z", Options),
        new(@"\A(?:an? )?" + Case + @" is what i(?: (?:need|want|am requesting)|['’]m requesting)\z", Options),
        // Compose an explicit input label with a single action or direction
        // label. Numeric declarations retain their codepoint type downstream.
        new(@"\A" + Declaration + @"[;.:] (?:please )?(?:convert|change|turn) (?:it|this(?: one)? character) (?:to|into) " + Case + @"\z", Options),
        new(@"\A" + Declaration + @"[;.:] (?:please )?(?:apply|perform|use) " + Case + @"(?: to it)?\z", Options),
        new(@"\A" + Declaration + @"[;.:] (?:please )?" + Case + @" it\z", Options),
        new(@"\A" + Declaration + @"[;.:] i (?:need|want) it " + Manner + Case + @"\z", Options),
        new(@"\A" + Declaration + @"[;.:] i would like it " + Manner + Case + @"\z", Options),
        new(@"\A" + Declaration + @"[;.:] i would prefer it " + Manner + Case + @"\z", Options),
        new(@"\A" + Declaration + @"[;.:] i['’]d (?:like|prefer) it " + Manner + Case + @"\z", Options),
        new(@"\A" + Declaration + @"[;.:] (?:(?:the )?(?:requested )?(?:case )?operation(?: is |: ?)|desired case: ?)" + Case + @"\z", Options),
        new(@"\A" + NumericDeclaration + @"[;.:] (?:please )?(?:convert|change|turn) it (?:to|into) " + Case + @"\z", Options),
        new(@"\A" + NumericDeclaration + @"[;.:] (?:please )?(?:apply|perform|use) " + Case + @"\z", Options)
    ];
    private static readonly Regex[] DirectionlessForms = [
        new(@"\Ado a case (?:change|conversion) on " + Input + @"\z", Options),
        new(@"\A(?:convert|change|turn|switch) (?<input>it) to (?:the )?other case\z", Options),
        new(@"\A(?:take|use) " + Input + @" and (?:convert|change|turn|switch) it to (?:the )?other case\z", Options),
        MissingDirection,
        new(@"\A" + Input + @" should undergo case (?:conversion|change)\z", Options),
        new(@"\A" + Desire + @" a case (?:change|conversion) (?:for|applied to|on|of) " + Input + @"\z", Options),
        new(@"\A" + Declaration + @"\z", Options),
        new(@"\A" + NumericDeclaration + @"\z", Options),
        new(@"\Ause " + Input + @"\z", Options),
        new(@"\Aprocess " + Input + @"\z", Options),
        new(@"\Atake " + Input + @" for this task\z", Options),
        new(@"\Ai have selected " + Input + @"\z", Options),
        new(@"\Athe character (?:i'd|i’d|i would) like you to work on is " + Input + @"\z", Options),
        new(@"\Ahere is " + Input + @": (?:please )?change its case\z", Options),
        new(@"\Ahere is " + Input + @" for a case operation\z", Options),
        new(@"\Athe (?:uppercase|lowercase) input (?:could be|is) " + Input + @"\z", Options)
    ];
    private static readonly Regex[] AmbiguousDirectionForms = [
        new(@"\A" + Desire + @" " + Input + @" in (?:uppercase or in lowercase|lowercase or in uppercase)\z", Options),
        new(@"\Ai want " + Input + @" changed to either (?:uppercase or lowercase|lowercase or uppercase)\z", Options),
        new(@"\Athe case i need for " + Input + @" is (?:upper or lower|lower or upper)\z", Options),
        new(@"\Amy requested form for " + Input + @" is (?:uppercase or lowercase|lowercase or uppercase)\z", Options),
        new(@"\Aconvert " + Input + @" to (?:either )?(?:upper or lower|lower or upper) case\z", Options),
        new(@"\A(?:should " + Input + @" be made|set " + Input + @" to) (?:upper(?:[ -]?case)?|lower(?:[ -]?case)?) or (?:upper(?:[ -]?case)?|lower(?:[ -]?case)?)\z", Options),
        new(@"\A(?:use|apply|perform) (?:upper(?:[ -]?case)?|lower(?:[ -]?case)?) or (?:upper(?:[ -]?case)?|lower(?:[ -]?case)?) (?:on|to|for) " + Input + @"\z", Options),
        new(@"\A" + OutputVerb + @" " + Input + @" (?:in|as) (?:upper(?:[ -]?case)?|lower(?:[ -]?case)?) or (?:upper(?:[ -]?case)?|lower(?:[ -]?case)?)\z", Options),
        new(@"\Afor " + Input + @", choose (?:uppercase or lowercase|lowercase or uppercase)\z", Options),
        new(@"\Amy case operation for " + Input + @" is (?:upper/lower|lower/upper)\z", Options),
        new(@"\Athe requested case for " + Input + @" is either (?:upper or lower|lower or upper)\z", Options),
        new(@"\Ai might want " + Input + @" in (?:upper case or lower case|lower case or upper case)\z", Options),
        new(@"\Amy target case for " + Input + @" could be (?:uppercase or lowercase|lowercase or uppercase)\z", Options)
    ];

    private static string OperandText(Match match)
    {
        var numeric = match.Groups["numeric"];
        var radix = match.Groups["radix"];
        var input = match.Groups["input"].Value.Trim();
        // This aside describes an explicitly encoded input used FOR an
        // operation. It is not consumed from output/result-format requests.
        if (match.Groups["inputUse"].Success)
        {
            var aside = InputHexAside.Match(input);
            if (aside.Success) input = aside.Groups["input"].Value;
        }
        // Preserve declaration qualifiers. A hexadecimal suffix is an input
        // representation only in an explicit numeric declaration; consuming it
        // on arbitrary operands could discard a requested result format.
        if ((numeric.Success || radix.Success) && !numeric.Value.StartsWith("decimal", StringComparison.OrdinalIgnoreCase))
        {
            var representation = DeclaredHexSuffix.Match(input);
            if (representation.Success) input = representation.Groups["input"].Value;
        }
        return (radix.Success ? "hexadecimal code point " : numeric.Success ? numeric.Value + " " : "") + input;
    }
    internal static LearningTaskProposal Clarify() => new("clarify", "specify_case_input", Prompt:
        "Specify uppercase or lowercase and one quoted Latin-1 character, or an explicit codepoint (for example U+00B5).");
    private static LearningTaskProposal Abstain(string code) => new("abstain", code);
    private static bool IsLowerOperation(string operation) => operation.StartsWith("lower", StringComparison.OrdinalIgnoreCase)
        || operation.StartsWith("down", StringComparison.OrdinalIgnoreCase)
        || operation.StartsWith("small", StringComparison.OrdinalIgnoreCase)
        || operation.StartsWith("uncapital", StringComparison.OrdinalIgnoreCase);
    internal static bool HasOperationNounAt(string text, int position)
    {
        var match = OperationLexeme.Match(text, position);
        return match.Success && match.Index == position && OperationNounAt(text, match.Index + match.Length).Success;
    }
    private static Match OperationNounAt(string text, int position)
    {
        var match = OperationNoun.Match(text, position);
        return match.Success && match.Index == position ? match : Match.Empty;
    }
    private static LearningTaskProposal Ready(string operation, byte key) => char.IsControl((char)key) ? Abstain("input_domain") : new("ready", "typed_case_change",
        IsLowerOperation(operation) ? "unicode17_lower_latin1" : "unicode17_upper_latin1", key);
    private static bool IsQuotedScalar(string token) => token.Length == 3
        && (token[0] == '\'' && token[2] == '\'' || token[0] == '"' && token[2] == '"');

    internal static LearningTaskProposal Propose(string text) => ProposeRequest(text, null, allowLearned: true);

    private static LearningTaskProposal ProposeRequest(string text, InputBinding? binding, bool allowLearned = false)
    {
        if (text.Length is < 1 or > 256 || text.Any(char.IsControl) || text.Any(char.IsSurrogate)) return Abstain("input_bounds");
        if (QuotedDomainRefusal(text) is { } quotedRefusal) return quotedRefusal;
        text = text.Trim();
        if (BareNumber.IsMatch(text) || text.Length == 1)
            return text.Length == 1 && text[0] > 255 ? Abstain("input_domain") : Clarify();
        var exact = Canonical.Match(text);
        if (exact.Success)
        {
            if (binding is not null) return Abstain("unsupported_intent");
            var value = exact.Groups["input"].Value;
            return byte.TryParse(value, NumberStyles.None, CultureInfo.InvariantCulture, out var key)
                && key.ToString(CultureInfo.InvariantCulture) == value ? Ready(exact.Groups["op"].Value, key) : Abstain("input_domain");
        }
        if (binding is null && ProposePartialRequest(text) is { } partial) return partial;
        if (binding is null && LearningCaseRequestSyntax.Propose(text) is { } constituent) return constituent;
        if (text.EndsWith('?') || text.EndsWith('.')) text = text[..^1].TrimEnd();
        text = ContextPrefix.Replace(text, "", 1);
        foreach (var prefix in PolitePrefixes)
        {
            var match = prefix.Match(text);
            if (!match.Success) continue;
            text = text[match.Length..];
            break;
        }
        text = SeePrefix.Replace(text, "show ", 1);
        text = PoliteSuffix.Replace(text, "", 1);
        text = ResultSuffix.Replace(text, "", 1);
        text = PreferenceSuffix.Replace(text, "", 1);
        var ambiguity = AmbiguitySuffix.Match(text);
        if (ambiguity.Success) text = text[..ambiguity.Index];
        if (CompoundOrNegated.IsMatch(text)) return Abstain("unsupported_intent");
        if (binding is not null && FieldSeparator.IsMatch(text)) return Abstain("unsupported_intent");
        if (binding is null && ProposeBoundRequest(text) is { } contextual)
            return ambiguity.Success && contextual.Status == "ready" ? Clarify() : contextual;
        foreach (var form in AmbiguousDirectionForms)
        {
            var match = form.Match(text);
            if (!match.Success) continue;
            var operand = ProposeRequestOperand("upper", OperandText(match), binding);
            return operand.Status == "abstain" ? operand : Clarify();
        }
        foreach (Match operation in OperationLexeme.Matches(text))
        {
            var operationName = operation.Groups["op"].Value;
            var operationNoun = OperationNounAt(text, operation.Index + operation.Length);
            var framed = text[..operation.Index] + '\u001f' + text[(operation.Index + operation.Length + operationNoun.Length)..];
            if (binding is null && ProposeFields(operationName, framed) is { } fields)
                return ambiguity.Success && fields.Status == "ready" ? Clarify() : fields;
            if (binding is not null && IsOperationField(framed))
                return ambiguity.Success ? Clarify() : BoundOperation(operationName, binding);
            // An indefinite article is not an input. Quoted 'a' and explicit
            // "a as ..." remain operands; preserve the lexeme's noun evidence.
            if (MissingOperand.IsMatch(framed) && (operationNoun.Success
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
                var operandText = OperandText(match);
                var proposal = ProposeRequestOperand(operationName, operandText, binding);
                // Even an ambiguous binding must not hide a malformed request.
                var typedOperand = proposal.Status == "ready" || binding is not null && ReferenceOperand.IsMatch(operandText);
                // Two names in an equivalence question must denote the SAME
                // operation. Conflicting aliases cannot choose a direction.
                if (typedOperand && match.Groups["alias"].Success
                    && IsLowerOperation(match.Groups["alias"].Value) != IsLowerOperation(operationName)) return Clarify();
                // A result sent "to" someone does not identify an operand.
                // Only the explicit counterpart relation has that meaning here.
                if (typedOperand && match.Groups["counterpartTo"].Success
                    && !operationNoun.Value.TrimStart(' ', '-').Equals("counterpart", StringComparison.OrdinalIgnoreCase))
                    return Abstain("unsupported_intent");
                // An input's case describes its current state, not a requested
                // conversion. This frame needs an explicit operation noun.
                if (typedOperand && match.Groups["inputCase"].Success)
                {
                    var noun = operationNoun.Value.TrimStart(' ', '-');
                    if (!new[] { "conversion", "operation", "transformation", "case conversion" }
                        .Contains(noun, StringComparer.OrdinalIgnoreCase)) return Clarify();
                }
                // Preserve verb/adjective evidence lost by the direction marker.
                // Showing a property is not applying a conversion; a modal
                // adjective question permits both readings and must clarify.
                if (typedOperand && !operationName.EndsWith("ed", StringComparison.OrdinalIgnoreCase)
                    && !match.Groups["transform"].Success)
                {
                    if (match.Groups["passive"].Success && match.Groups["output"].Value.StartsWith("show", StringComparison.OrdinalIgnoreCase))
                        return Abstain("unsupported_intent");
                    if (match.Groups["modal"].Success) return Clarify();
                }
                return (ambiguity.Success || match.Groups["unresolved"].Success || match.Groups["which"].Success) && proposal.Status == "ready" ? Clarify() : proposal;
            }
        }
        // Reuse operand refusal even when direction is absent. A hypothetical
        // ready scalar is discarded: missing direction can never authorize it.
        foreach (var form in DirectionlessForms)
        {
            var missing = form.Match(text);
            if (!missing.Success) continue;
            if (binding is null && missing.Groups["partialRelation"].Success)
                return ClarifyPartialOperand(OperandText(missing));
            var operand = ProposeRequestOperand("upper", OperandText(missing), binding);
            return operand.Status == "abstain" ? operand : Clarify();
        }
        if (binding is null && MatchInputField(text) is { } declared)
        {
            var operand = ProposeOperand("upper", declared);
            return operand.Status == "abstain" ? operand : Clarify();
        }
        // Unclaimed constructions may be recovered by the offline proposer.
        // Claimed ready/clarify/domain refusals never reach here.
        return allowLearned && binding is null ? LearningIntentProposer.Propose(text) : Abstain("unsupported_intent");
    }

    private static LearningTaskProposal BoundOperation(string operation, InputBinding binding) =>
        binding.OriginalByte is { } key ? Ready(operation, key)
            : binding.Status == "abstain" ? Abstain(binding.Code) : Clarify();

    private static LearningTaskProposal ProposeRequestOperand(string operation, string operand, InputBinding? binding)
    {
        if (binding is null) return ProposeOperand(operation, operand);
        // Resolve raw reference syntax BEFORE any description stripping. A
        // literal named "it" and a second explicit input cannot become a ref.
        return ReferenceOperand.IsMatch(operand) ? BoundOperation(operation, binding) : Abstain("unsupported_intent");
    }

    private static LearningTaskProposal? ProposeBoundRequest(string text)
    {
        foreach (Match separator in FieldSeparator.Matches(text))
        {
            var left = text[..separator.Index];
            var right = text[(separator.Index + separator.Length)..];
            // A comma in a supported explicit input representation belongs to
            // that input phrase, not to the following request. Ask the same
            // field parser to validate the extended phrase before deferring.
            if (separator.Value.StartsWith(',') && right.StartsWith("in hexadecimal, ", StringComparison.OrdinalIgnoreCase)
                && MatchInputField(left + ", in hexadecimal") is { } extended && HexInput.IsMatch(extended)) continue;
            if (!ReferenceUse.IsMatch(right) || MatchInputField(left) is not { } operand) continue;
            // A generic "For uppercase conversion" phrase is not an input
            // declaration. It must not bind a missing scalar to another action.
            if (OperationLexeme.IsMatch(operand)) return Abstain("unsupported_intent");
            var checkedInput = ProposeOperand("upper", operand);
            if (checkedInput.Status == "abstain") return checkedInput;
            var binding = new InputBinding(checkedInput.Key, checkedInput.Status, checkedInput.Code, left);
            // Bound mode cannot re-enter declaration/field composition. Once
            // claimed, refusal is final: legacy frames cannot reinterpret it.
            return ProposeRequest(right, binding);
        }
        return null;
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

    private static LearningTaskProposal? QuotedDomainRefusal(string text)
    {
        // Inspect explicit literal boundaries, not English words or operation
        // truth. Word-internal apostrophes are contractions, not opening quotes.
        // Matching triple quotes denote the quote scalar, as in the operand gate.
        for (var start = 0; start < text.Length; start++)
        {
            var quote = text[start];
            if (quote is not ('\'' or '"') || quote == '\'' && start > 0 && char.IsLetterOrDigit(text[start - 1])) continue;
            if (start + 2 < text.Length && text[start + 1] == quote && text[start + 2] == quote)
            {
                start += 2;
                continue;
            }
            var end = text.IndexOf(quote, start + 1);
            if (end < 0) continue; // Malformed quoting stays with clarification.
            if (end - start > 2) return Abstain("unsupported_intent");
            if (end - start == 2 && text[start + 1] > 255) return Abstain("input_domain");
            start = end;
        }
        return null;
    }

    private static bool IsOperationField(string text) => OperationFields.Any(field => field.IsMatch(text)) || ActionFields.Any(field => field.IsMatch(text));

    private static LearningTaskProposal? ProposePartialRequest(string text)
    {
        // Partial field roles must be claimed before a unary grammar can treat
        // the entire input declaration as one operand. This path never emits ready.
        if (text.EndsWith('?') || text.EndsWith('.')) text = text[..^1].TrimEnd();
        foreach (var prefix in PolitePrefixes)
        {
            var match = prefix.Match(text);
            if (!match.Success) continue;
            text = text[match.Length..];
            break;
        }
        if (CompoundOrNegated.IsMatch(text)) return null;
        foreach (Match operation in OperationLexeme.Matches(text))
        {
            var noun = OperationNounAt(text, operation.Index + operation.Length);
            var framed = text[..operation.Index] + '\u001f' + text[(operation.Index + operation.Length + noun.Length)..];
            if (PartialOperationField.IsMatch(framed)) return Clarify();
            foreach (Match separator in PartialFieldSeparator.Matches(framed))
            {
                var left = framed[..separator.Index];
                var right = framed[(separator.Index + separator.Length)..];
                var field = PartialOperationField.IsMatch(left) || IsOperationField(left)
                    ? AlternativeInputField.Match(right)
                    : PartialOperationField.IsMatch(right) || IsOperationField(right)
                        ? AlternativeInputField.Match(left) : Match.Empty;
                if (field.Success) return ClarifyPartialOperand(OperandText(field));
            }
        }
        return null;
    }

    private static LearningTaskProposal ClarifyPartialOperand(string input)
    {
        var operand = ProposeOperand("upper", input);
        if (operand.Status == "abstain") return operand;
        if (operand.Status == "ready" || ProposeAlternatives(input) is not null) return Clarify();
        // The legacy validator tolerates unknown operand prose. Newly claimed
        // partial roles accept only explicit missing slots or scalar ambiguity,
        // not arbitrary action text. No action-word blacklist is involved.
        if (input.Length <= 1 || MissingScalar.IsMatch(input) || UnspecifiedReference.IsMatch(input)
            || BareNumber.IsMatch(input) || input[0] is '\'' or '"') return Clarify();
        return Abstain("unsupported_intent");
    }

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

    internal static LearningTaskProposal ProposeOperand(string operation, string token, bool allowAlternatives = true, bool declaredScalar = false)
    {
        var encoded = ByteInput.Match(token);
        if (encoded.Success)
        {
            var input = encoded.Groups["input"].Value;
            var encodedProposal = ProposeOperandCore(operation, input, allowAlternatives, true);
            // Byte spelling must be explicit. Bare A–F cannot silently choose
            // ASCII or hexadecimal; controls/OOD still refuse before ambiguity.
            return encodedProposal.Status == "ready" && !IsQuotedScalar(input) && !HexInput.IsMatch(input) && !DecimalInput.IsMatch(input)
                ? Clarify() : encodedProposal;
        }
        var plural = PluralInput.Match(token);
        if (plural.Success)
        {
            var candidates = ProposeOperandCore(operation, plural.Groups["input"].Value, allowAlternatives, true);
            return candidates.Status == "abstain" ? candidates : Clarify();
        }
        var named = NamedInput.Match(token);
        if (!named.Success) return ProposeOperandCore(operation, token, allowAlternatives, declaredScalar);
        // An explicit descriptor constrains an operand, not arbitrary prose.
        // Validate once, after canonical normalization, retaining recognized
        // scalar ambiguity while refusing unknown operand prose.
        var proposal = ProposeOperandCore(operation, named.Groups["input"].Value, allowAlternatives, true, strict: true);
        if (proposal.Status != "ready") return proposal;
        // A name constrains an explicit operand; it never supplies its byte.
        // Domain refusal precedes descriptor disagreement.
        var value = (char)proposal.Key!.Value;
        var agrees = named.Groups["name"].Value.ToLowerInvariant() switch
        {
            "hyphen" => value == '-',
            "asterisk" => value == '*',
            "space" => value == ' ',
            "plus sign" => value == '+',
            "semicolon" => value == ';',
            "punctuation" => char.IsPunctuation(value) || char.IsSymbol(value),
            _ => false
        };
        return agrees ? proposal : Clarify();
    }

    internal static int CountOperationLexemes(string text) => OperationLexeme.Matches(text).Count;

    private static LearningTaskProposal ProposeOperandCore(string operation, string token, bool allowAlternatives, bool declaredScalar, bool strict = false)
    {
        // Strip only the bounded operand description, never normalize the scalar.
        var description = InputDescription.Match(token);
        if (description.Success) { token = token[description.Length..]; declaredScalar = true; }
        else
        {
            // Compose at most one additional modifier, only if the remaining
            // text starts with a complete noun description. No free deletion.
            var leading = LeadingModifier.Match(token);
            if (leading.Success)
            {
                var nested = InputDescription.Match(token[leading.Length..]);
                if (nested.Success) { token = token[(leading.Length + nested.Length)..]; declaredScalar = true; }
            }
        }
        var shortDescription = ShortDescription.Match(token);
        if (shortDescription.Success) token = shortDescription.Groups["input"].Value;
        var unicodeCodepoint = UnicodeCodepoint.Match(token);
        if (unicodeCodepoint.Success) token = unicodeCodepoint.Groups["input"].Value;
        var quoted = QuotedDescription.Match(token);
        if (quoted.Success) token = quoted.Groups["input"].Value;
        var fullStop = QuotedFullStop.Match(token);
        if (fullStop.Success) token = fullStop.Groups["input"].Value;
        var nonbreaking = QuotedNonbreakingSpace.Match(token);
        if (nonbreaking.Success) token = nonbreaking.Groups["input"].Value;
        var space = QuotedSpace.Match(token);
        if (space.Success) token = space.Groups["input"].Value;
        var written = ScalarWritten.Match(token);
        if (written.Success) token = written.Groups["input"].Value;
        var shown = ShownInput.Match(token);
        // "Shown as" describes an already established input noun. Without
        // that role it could be an output instruction, not a supplied scalar.
        if (shown.Success && declaredScalar) token = shown.Groups["input"].Value;
        var explicitScalar = ExplicitScalarSuffix.Match(token);
        if (explicitScalar.Success) token = explicitScalar.Groups["input"].Value;
        var decimalRelation = DecimalRelation.Match(token);
        if (decimalRelation.Success) token = "code point " + decimalRelation.Groups["input"].Value;
        var numbered = NumberedDecimal.Match(token);
        if (numbered.Success) token = "code point " + numbered.Groups["input"].Value;
        // Relational descriptions require an explicit codepoint, never a guessed
        // byte from a location or an incomplete article such as "letter with a".
        var relation = CodepointRelation.Match(token);
        if (relation.Success) token = relation.Groups["input"].Value;
        if (token.Length == 0 || MissingScalar.IsMatch(token) || UnspecifiedReference.IsMatch(token)) return Clarify();
        if (IsQuotedScalar(token))
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
        if (token.Length != 1)
            return strict && token[0] is not ('\'' or '"') ? Abstain("unsupported_intent") : Clarify();
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
