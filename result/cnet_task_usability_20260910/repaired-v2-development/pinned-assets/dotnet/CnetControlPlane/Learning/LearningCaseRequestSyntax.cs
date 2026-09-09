namespace CnetControlPlane.Learning;

// A bounded single-clause grammar. Tokens retain the original spelling and
// offsets; only separators between grammatical tokens are normalized. Null
// means this construction belongs to a compatibility grammar, not a refusal.
internal static class LearningCaseRequestSyntax
{
    private readonly record struct Token(string Value, int Start, int Length, bool Literal)
    {
        internal bool Is(string word) => !Literal && Value.Equals(word, StringComparison.OrdinalIgnoreCase);
    }

    internal static LearningTaskProposal? Propose(string text)
    {
        var tokens = Lex(text);
        // Explicit choice commentary and multi-clause bindings are outside
        // this single-clause grammar. Compatibility parsing owns them.
        if (tokens.Any(token => IsAny(token, ";", "whichever"))) return null;
        var end = tokens.Count;
        if (end > 0 && IsAny(tokens[end - 1], ".", "?", "!")) end--;
        for (var count = 0; count < 2; count++)
        {
            if (end > 0 && tokens[end - 1].Is("please")) end--;
            else if (end > 1 && tokens[end - 2].Is("for") && tokens[end - 1].Is("me")) end -= 2;
            else break;
            if (end > 0 && tokens[end - 1].Is(",")) end--;
        }
        var start = 0;
        Polite(tokens, ref start, end);
        if (start + 5 < end && tokens[start].Is("if") && tokens[start + 1].Is("you") && tokens[start + 2].Is("have")
            && tokens[start + 3].Is("a") && tokens[start + 4].Is("moment") && tokens[start + 5].Is(",")) start += 6;
        if (start + 2 < end && tokens[start].Is("go") && tokens[start + 1].Is("ahead") && tokens[start + 2].Is("and")) start += 3;
        if (start + 1 < end && IsAny(tokens[start], "can", "could", "would", "will") && tokens[start + 1].Is("you"))
        {
            start += 2;
            if (start + 2 < end && tokens[start].Is("be") && tokens[start + 1].Is("able") && tokens[start + 2].Is("to")) start += 3;
            if (start < end && tokens[start].Is("mind")) start++;
        }
        Polite(tokens, ref start, end);
        if (start == end) return null;
        if (tokens.Any(token => IsAny(token, "not", "never", "then", "also"))) return Refuse();
        if (!IsAny(tokens[start], "uppercase", "lowercase", "capitalize", "capitalise", "upcase", "downcase",
                "uppercasing", "lowercasing", "uncapitalize", "uncapitalise", "uncapitalizing", "uncapitalising"))
            return Request(tokens, start, end);
        var descriptionNoun = start + 1 < end && IsAny(tokens[start + 1], "character", "letter");
        var nominalRelation = start + 2 < end && IsAny(tokens[start + 2], "of", "for", "from", "to", "on", "is", "when", "should");
        if (LearningTaskParser.HasOperationNounAt(text, tokens[start].Start) && (!descriptionNoun || nominalRelation)) return null;
        var operation = IsAny(tokens[start++], "lowercase", "lowercasing", "downcase",
            "uncapitalize", "uncapitalise", "uncapitalizing", "uncapitalising") ? "lower" : "upper";
        // These start nominal, relational or declaration constructions, not
        // the unary command owned here. The existing grammar owns their roles.
        if (start < end && IsAny(tokens[start], "is", "for", "on", "when", "should", "as", "with")) return null;
        if (start < end && tokens[start].Is(":")) start++;
        return Operand(tokens, start, end, operation);
    }

    private static LearningTaskProposal? Request(List<Token> tokens, int start, int end)
    {
        var cursor = start;
        var question = tokens[cursor].Is("what's") || tokens[cursor].Is("what’s");
        if (question) cursor++;
        else if (cursor + 1 < end && tokens[cursor].Is("what") && tokens[cursor + 1].Is("is")) { cursor += 2; question = true; }
        var output = !question && IsOutputVerb(tokens[cursor]);
        var conversion = !question && IsConversionVerb(tokens[cursor]);
        var take = conversion && IsTakeOrBring(tokens[cursor]);
        var raise = conversion && IsRaiseVerb(tokens[cursor]);
        var make = output && IsAny(tokens[cursor], "make", "making");
        if (!question && !output && !conversion) return null;
        if (!question) cursor++;
        if (output && cursor < end && tokens[cursor].Is("me")) cursor++;
        if (conversion)
        {
            var role = cursor;
            if (role < end && tokens[role].Is("the")) role++;
            if (role + 1 < end && tokens[role].Is("case") && tokens[role + 1].Is("of")) cursor = role + 2;
        }
        var operandStart = cursor;
        if (question || output)
        {
            var nominal = Direction(tokens, ref cursor, end);
            if (nominal is not null && cursor < end && IsAny(tokens[cursor], "of", "for"))
                return Operand(tokens, cursor + 1, end, nominal, bareCapital: tokens[cursor - 1].Is("capital"));
            // With no following object, A/a may be the original input rather
            // than an article ("make a uppercase"). Legacy roles own that form.
            if (nominal is not null && make && cursor < end)
            {
                if (tokens[cursor].Is(":")) cursor++;
                var proposal = Operand(tokens, cursor, end, nominal);
                // A/a before the direction can itself be an input. A second
                // operand cannot silently displace it as an inferred article.
                return tokens[operandStart].Is("a") && proposal.Status != "abstain"
                    ? LearningTaskParser.Clarify() : proposal;
            }
        }
        if (question) return null;
        // The complement establishes the target-case role. Its complete tail
        // must match; extra actions or output instructions cannot be discarded.
        for (cursor = operandStart; cursor < end; cursor++)
        {
            if (conversion && tokens[cursor].Is("so"))
            {
                var complement = cursor + 1;
                if (complement < end && tokens[complement].Is("that")) complement++;
                if (complement < end && tokens[complement].Is("it")) complement++;
                else continue;
                if (complement < end && tokens[complement].Is("is")) complement++;
                else if (complement + 1 < end && tokens[complement].Is("appears") && tokens[complement + 1].Is("in")) complement += 2;
                else continue;
                if (complement < end && tokens[complement].Is("written"))
                {
                    complement++;
                    if (complement < end && tokens[complement].Is("in")) complement++;
                }
                var caseChoice = Direction(tokens, ref complement, end);
                if (caseChoice is not null && complement == end)
                    return ConstrainedOperand(tokens, operandStart, cursor, caseChoice, raise ? "upper" : null);
            }
            if (IsAny(tokens[cursor], "after", "before"))
            {
                // Show … after converting … is an output request. A second
                // transformation (Raise … after converting …) is unsupported.
                if (!output) return Refuse();
                var sequential = SequentialConversion(tokens, operandStart, cursor, end);
                if (sequential is not null) return sequential;
                continue;
            }
            if (!IsAny(tokens[cursor], conversion ? ["to", "into", "onto", "using"] : ["in", "as", "into", "using"])) continue;
            var target = cursor + 1;
            var direction = Direction(tokens, ref target, end);
            // An explicit trailing input slot can resolve this/that/it, but
            // cannot replace a preceding scalar or silently add a second one.
            if (direction is not null && target < end && tokens[target].Is(":")
                && cursor == operandStart + 1 && IsAny(tokens[operandStart], "this", "that", "it"))
                return ConstrainedOperand(tokens, target + 1, end, direction, raise ? "upper" : null);
            if (direction is not null && target < end && tokens[target].Is(":")
                && IsSpacePlaceholder(tokens, operandStart, cursor))
            {
                var proposal = Operand(tokens, target + 1, end, direction, requiredName: "space");
                return raise && direction != "upper" && proposal.Status != "abstain"
                    ? LearningTaskParser.Clarify() : proposal;
            }
            if (direction is not null && target == end)
            {
                // A later conversion clause is not this grammar's operand.
                if (ForeignClause(tokens, operandStart, cursor)) return null;
                var operandEnd = cursor;
                string? modifier = null;
                if (take && operandEnd > operandStart && IsAny(tokens[operandEnd - 1], "up", "down"))
                    modifier = tokens[--operandEnd].Is("down") ? "lower" : "upper";
                return ConstrainedOperand(tokens, operandStart, operandEnd, direction, modifier ?? (raise ? "upper" : null));
            }
        }
        return null;
    }

    private static string? Direction(List<Token> tokens, ref int cursor, int end)
    {
        if (cursor < end && IsAny(tokens[cursor], "the", "a", "an", "its")) cursor++;
        if (cursor == end) return null;
        var word = tokens[cursor++];
        string? direction = IsAny(word, "upper", "upper-case", "uppercase", "capital", "capital-letter", "capitals", "capitalized", "capitalised", "caps") ? "upper"
            : IsAny(word, "lower", "lower-case", "lowercase", "small", "small-letter") ? "lower" : null;
        if (word.Is("all") && cursor < end && tokens[cursor].Is("caps")) { cursor++; direction = "upper"; }
        if (direction is null) return null;
        // Size is not letter case. Bare "small" cannot choose lowercase.
        if (word.Is("small") && (cursor == end || !IsAny(tokens[cursor], "letter", "letters"))) return null;
        if (cursor < end && IsAny(word, "upper", "lower") && tokens[cursor].Is("case")) cursor++;
        if (cursor < end && IsAny(tokens[cursor], "form", "version", "equivalent", "letter", "letters", "character", "counterpart")) cursor++;
        return direction;
    }

    private static LearningTaskProposal Refuse() => new("abstain", "unsupported_intent");

    private static LearningTaskProposal ConstrainedOperand(List<Token> tokens, int start, int end, string direction, string? constraint)
    {
        var proposal = Operand(tokens, start, end, direction);
        return constraint is not null && constraint != direction && proposal.Status != "abstain"
            ? LearningTaskParser.Clarify() : proposal;
    }

    private static void Polite(List<Token> tokens, ref int start, int end)
    {
        if (start >= end || !IsAny(tokens[start], "please", "kindly")) return;
        start++;
        if (start < end && tokens[start].Is(",")) start++;
    }

    private static bool IsSpacePlaceholder(List<Token> tokens, int start, int end)
    {
        if (start < end && IsAny(tokens[start], "this", "that", "the")) start++;
        if (start < end && IsAny(tokens[start], "quoted", "literal")) start++;
        if (start == end || !tokens[start++].Is("space")) return false;
        if (start < end && tokens[start].Is("character")) start++;
        return start == end;
    }

    private static LearningTaskProposal Operand(List<Token> tokens, int start, int end, string operation, bool bareCapital = false, string? requiredName = null)
    {
        var cursor = start;
        var declaredScalar = false;
        if (cursor < end && IsAny(tokens[cursor], "the", "my", "a", "an", "this", "that")) cursor++;
        for (var count = 0; count < 3 && cursor < end && IsAny(tokens[cursor],
                 "single", "single-character", "one-character", "quoted", "literal", "supplied", "provided", "selected", "chosen"); count++) cursor++;
        if (cursor < end && IsAny(tokens[cursor], "character", "letter", "scalar", "digit", "symbol", "input"))
        {
            declaredScalar = true;
            var input = tokens[cursor++].Is("input");
            if (input && cursor < end && IsAny(tokens[cursor], "character", "letter", "scalar")) cursor++;
            if (cursor < end && tokens[cursor].Is(":")) cursor++;
            start = cursor;
        }
        var builder = new System.Text.StringBuilder();
        for (var index = start; index < end; index++)
        {
            // Preserve punctuation adjacency and all literal bytes. Only an
            // original gap between tokens becomes one grammatical space.
            if (index > start && tokens[index].Start > tokens[index - 1].Start + tokens[index - 1].Length) builder.Append(' ');
            builder.Append(tokens[index].Value);
        }
        var operand = builder.ToString();
        if (start < end && tokens[start].Literal && (tokens[start].Length < 2 || tokens[start].Value[^1] != tokens[start].Value[0]))
            return LearningTaskParser.Clarify();
        var proposal = LearningTaskParser.ProposeOperand(operation,
            requiredName is null ? operand : requiredName + " " + operand, declaredScalar: declaredScalar);
        // Unqualified "capital of France" does not establish a casing task.
        // Explicit case phrases with named characters still need clarification.
        if (proposal.Status == "clarify" && bareCapital && !declaredScalar && end - start == 1 && !tokens[start].Literal
            && tokens[start].Value.Length > 1 && tokens[start].Value.All(char.IsLetter)
            && !IsAny(tokens[start], "codepoint", "character", "letter", "scalar", "input")) return Refuse();
        return proposal;
    }

    private static bool IsOutputVerb(Token token) => IsAny(token,
        "write", "writing", "render", "rendering", "return", "returning", "show", "showing",
        "display", "displaying", "supply", "supplying", "provide", "providing", "produce", "producing",
        "present", "presenting", "express", "expressing", "give", "giving", "put", "putting", "make", "making");

    private static bool IsConversionVerb(Token token) => IsAny(token,
        "convert", "converting", "converted", "change", "changing", "changed", "turn", "turning", "turned",
        "switch", "switching", "switched", "transform", "transforming", "transformed",
        "map", "mapping", "mapped", "fold", "folding", "folded", "rewrite", "rewriting", "rewritten",
        "recast", "recasting", "take", "taking", "taken", "raise", "raising", "raised",
        "bring", "bringing", "brought");

    private static bool IsRaiseVerb(Token token) => IsAny(token, "raise", "raising", "raised");

    private static bool IsTakeOrBring(Token token) =>
        IsAny(token, "take", "taking", "taken", "bring", "bringing", "brought");

    // "Take 0xC9 and switch it …" belongs to the bound-request grammar.
    private static bool ForeignClause(List<Token> tokens, int start, int end)
    {
        for (var index = start; index < end; index++)
        {
            if (!IsAny(tokens[index], "and", "after", "before")) continue;
            for (var next = index + 1; next < end; next++)
                if (IsConversionVerb(tokens[next]) || IsOutputVerb(tokens[next])) return true;
        }
        return false;
    }

    private static LearningTaskProposal? SequentialConversion(List<Token> tokens, int operandStart, int marker, int end)
    {
        var cursor = marker + 1;
        if (cursor >= end || !IsConversionVerb(tokens[cursor])) return null;
        var verb = tokens[cursor++];
        if (cursor >= end || !tokens[cursor].Is("it")) return Refuse();
        cursor++;
        if (cursor >= end || !IsAny(tokens[cursor], "to", "into", "onto", "using")) return null;
        cursor++;
        var direction = Direction(tokens, ref cursor, end);
        if (direction is null) return null;
        if (cursor != end || tokens[marker].Is("before")) return Refuse();
        return ConstrainedOperand(tokens, operandStart, marker, direction, IsRaiseVerb(verb) ? "upper" : null);
    }

    private static bool IsAny(Token token, params string[] words) => words.Any(token.Is);

    private static List<Token> Lex(string text)
    {
        var tokens = new List<Token>();
        for (var cursor = 0; cursor < text.Length;)
        {
            if (char.IsWhiteSpace(text[cursor])) { cursor++; continue; }
            var start = cursor;
            var quote = text[cursor];
            var literal = quote is '\'' or '"';
            if (literal)
            {
                if (cursor + 2 < text.Length && text[cursor + 1] == quote && text[cursor + 2] == quote) cursor += 3;
                else
                {
                    var close = text.IndexOf(quote, cursor + 1);
                    cursor = close < 0 ? text.Length : close + 1;
                }
            }
            else if (quote is ',' or ';' or ':' or '.' or '!' or '?') cursor++;
            else
            {
                while (cursor < text.Length && !char.IsWhiteSpace(text[cursor]) && text[cursor] is not (',' or ';' or ':' or '.' or '!' or '?')) cursor++;
            }
            tokens.Add(new Token(text[start..cursor], start, cursor - start, literal));
        }
        return tokens;
    }
}
