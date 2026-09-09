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
        if (start + 1 < end && IsAny(tokens[start], "can", "could", "would", "will") && tokens[start + 1].Is("you"))
        {
            start += 2;
            if (start + 2 < end && tokens[start].Is("be") && tokens[start + 1].Is("able") && tokens[start + 2].Is("to")) start += 3;
        }
        Polite(tokens, ref start, end);
        if (start == end) return null;
        if (tokens.Any(token => IsAny(token, "not", "never", "then", "also"))) return Refuse();
        if (!IsAny(tokens[start], "uppercase", "lowercase", "capitalize", "capitalise"))
            return Request(tokens, start, end);
        var descriptionNoun = start + 1 < end && IsAny(tokens[start + 1], "character", "letter");
        var nominalRelation = start + 2 < end && IsAny(tokens[start + 2], "of", "for", "from", "to", "on", "is", "when", "should");
        if (LearningTaskParser.HasOperationNounAt(text, tokens[start].Start) && (!descriptionNoun || nominalRelation)) return null;
        var operation = tokens[start++].Is("lowercase") ? "lower" : "upper";
        // These start nominal, relational or declaration constructions, not
        // the unary command owned here. The existing grammar owns their roles.
        if (start < end && IsAny(tokens[start], "is", "for", "on", "when", "should", "as", "with")) return null;
        return Operand(tokens, start, end, operation);
    }

    private static LearningTaskProposal? Request(List<Token> tokens, int start, int end)
    {
        var cursor = start;
        var question = tokens[cursor].Is("what's") || tokens[cursor].Is("what’s");
        if (question) cursor++;
        else if (cursor + 1 < end && tokens[cursor].Is("what") && tokens[cursor + 1].Is("is")) { cursor += 2; question = true; }
        var output = !question && IsAny(tokens[cursor], "write", "render", "return", "show", "display", "supply", "provide", "produce", "present", "express", "give", "put", "make");
        var conversion = !question && IsAny(tokens[cursor], "convert", "change", "turn", "switch", "transform");
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
        }
        if (question) return null;
        // The complement establishes the target-case role. Its complete tail
        // must match; extra actions or output instructions cannot be discarded.
        for (cursor = operandStart; cursor < end; cursor++)
        {
            if (!IsAny(tokens[cursor], conversion ? ["to", "into"] : ["in", "as", "into", "using"])) continue;
            var target = cursor + 1;
            var direction = Direction(tokens, ref target, end);
            if (direction is not null && target == end) return Operand(tokens, operandStart, cursor, direction);
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

    private static void Polite(List<Token> tokens, ref int start, int end)
    {
        if (start >= end || !IsAny(tokens[start], "please", "kindly")) return;
        start++;
        if (start < end && tokens[start].Is(",")) start++;
    }

    private static LearningTaskProposal Operand(List<Token> tokens, int start, int end, string operation, bool bareCapital = false)
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
        var proposal = LearningTaskParser.ProposeOperand(operation, operand);
        // Unqualified "capital of France" does not establish a casing task.
        // Explicit case phrases with named characters still need clarification.
        if (proposal.Status == "clarify" && bareCapital && !declaredScalar && end - start == 1 && !tokens[start].Literal
            && tokens[start].Value.Length > 1 && tokens[start].Value.All(char.IsLetter)
            && !IsAny(tokens[start], "codepoint", "character", "letter", "scalar", "input")) return Refuse();
        return proposal;
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
