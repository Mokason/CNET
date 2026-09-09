using System.Globalization;
using System.Text.RegularExpressions;

namespace CnetControlPlane.Learning;

// Learned frame + independently checked operand. A confident class cannot
// skip domain, quote or extra-scalar refusal.
internal static class LearningIntentProposer
{
    private readonly record struct Scalar(string Token, LearningTaskProposal Proposal);
    private static readonly Lazy<(LearningIntentModel Model, LearningIntentReport Report)> Trained = new(TrainAndScore);
    private static readonly Regex Codepoint = new(@"\b(?:U\+[0-9A-Fa-f]{1,6}|0x[0-9A-Fa-f]{1,6})\b",
        RegexOptions.CultureInvariant | RegexOptions.NonBacktracking);
    internal static LearningIntentReport Report => Trained.Value.Report;

    internal static LearningTaskProposal Propose(string text) => ProposeCore(Trained.Value.Model, text);

    private static (LearningIntentModel Model, LearningIntentReport Report) TrainAndScore()
    {
        var train = LearningIntentSpec.TrainCorpus();
        var calibration = LearningIntentSpec.CalibrationCorpus();
        var (model, _) = LearningIntentModel.Train(train, calibration);
        var predictedReady = 0;
        var wrongReady = 0;
        foreach (var example in calibration)
        {
            var proposal = ProposeCore(model, example.Text);
            if (proposal.Status != "ready") continue;
            var lower = example.Operation == "lower" && proposal.Dataset == "unicode17_lower_latin1";
            var upper = example.Operation == "upper" && proposal.Dataset == "unicode17_upper_latin1";
            if (example.Status == "ready" && (lower || upper)) predictedReady++;
            else wrongReady++;
        }
        return (model, new(train.Count, predictedReady, wrongReady, model.Threshold));
    }

    private static LearningTaskProposal ProposeCore(LearningIntentModel model, string text)
    {
        var predicted = model.Classify(text, out var confidence);
        var threshold = model.Threshold;
        var scalars = Scalars(text);
        var domain = scalars.Find(s => s.Proposal.Code == "input_domain");
        var ready = scalars.FindAll(s => s.Proposal.Status == "ready");
        var keys = ready.Select(s => s.Proposal.Key!.Value).Distinct().ToList();
        if (ContainsNegation(text) || ExtraClause(text) || UnknownResidue(text, scalars))
            return domain.Token is not null ? domain.Proposal : new("abstain", "unsupported_intent");
        // A forbidden candidate is terminal even beside a valid Latin-1 scalar.
        if (domain.Token is not null) return domain.Proposal;
        if (keys.Count > 1)
            return text.Contains(" or ", StringComparison.OrdinalIgnoreCase)
                ? LearningTaskParser.Clarify() : new("abstain", "unsupported_intent");
        if (predicted <= 1 && BareSmall(text)) return new("abstain", "unsupported_intent");
        if (predicted == 3) return new("abstain", "unsupported_intent");
        if (keys.Count != 1 || predicted > 1 || confidence < threshold)
            return keys.Count == 1 || predicted == 2 ? LearningTaskParser.Clarify() : new("abstain", "unsupported_intent");
        var quoted = ready.Find(s => s.Token.Length >= 3 && (s.Token[0] is '\'' or '"'));
        var token = quoted.Token ?? ready[0].Token;
        return LearningTaskParser.ProposeOperand(predicted == 0 ? "upper" : "lower", token);
    }

    private static bool BareSmall(string text)
    {
        var index = text.IndexOf("small", StringComparison.OrdinalIgnoreCase);
        if (index < 0) return false;
        var rest = text[(index + 5)..].TrimStart();
        return !rest.StartsWith("letter", StringComparison.OrdinalIgnoreCase);
    }

    private static bool ExtraClause(string text)
    {
        for (var i = 0; i < text.Length; i++)
        {
            if (text[i] != ';') continue;
            var quoted = false;
            var quote = '\0';
            for (var k = 0; k < i; k++)
            {
                if (text[k] is not ('\'' or '"')) continue;
                if (quote == '\0') { quote = text[k]; quoted = true; }
                else if (text[k] == quote) { quoted = false; quote = '\0'; }
            }
            if (!quoted) return true;
        }
        return false;
    }

    private static readonly HashSet<string> RequestWords = new(StringComparer.OrdinalIgnoreCase)
    {
        "kindly", "please", "could", "would", "you", "mind", "recode", "the", "glyph", "symbol",
        "toward", "to", "into", "onto", "using", "small", "letters", "letter", "lowercase", "lower",
        "case", "uppercase", "upper", "capitals", "convert", "change", "changing", "take", "and",
        "switch", "it", "show", "me", "after", "converting", "rewrite", "uncapitalize", "bring",
        "down", "do", "a", "an", "on", "of", "for", "other", "which", "should", "i", "or",
        "character", "that", "this", "my", "its", "in", "as", "caps", "upcase", "downcase"
    };

    private static bool UnknownResidue(string text, List<Scalar> scalars)
    {
        var chars = text.ToCharArray();
        foreach (var scalar in scalars)
        {
            var start = 0;
            while ((start = text.IndexOf(scalar.Token, start, StringComparison.Ordinal)) >= 0)
            {
                for (var i = start; i < start + scalar.Token.Length && i < chars.Length; i++) chars[i] = ' ';
                start += scalar.Token.Length;
            }
        }
        foreach (var word in new string(chars).Split((char[]?)null, StringSplitOptions.RemoveEmptyEntries))
        {
            var token = word.Trim(',', ';', ':', '.', '!', '?');
            if (token.Length == 0) continue;
            if (!RequestWords.Contains(token)) return true;
        }
        return false;
    }

    private static bool ContainsNegation(string text) =>
        text.Contains(" not ", StringComparison.OrdinalIgnoreCase)
        || text.StartsWith("Do not ", StringComparison.OrdinalIgnoreCase)
        || text.StartsWith("Never ", StringComparison.OrdinalIgnoreCase)
        || text.Contains(" never ", StringComparison.OrdinalIgnoreCase);

    private static List<Scalar> Scalars(string text)
    {
        var found = new List<Scalar>();
        var seen = new HashSet<string>(StringComparer.Ordinal);
        var skip = new bool[text.Length];
        void Cover(int start, int end)
        {
            for (var i = start; i < end && i < skip.Length; i++) skip[i] = true;
        }
        void Add(string token)
        {
            if (!seen.Add(token)) return;
            var proposal = LearningTaskParser.ProposeOperand("upper", token);
            if (proposal.Status == "ready" || proposal.Code == "input_domain") found.Add(new(token, proposal));
        }
        for (var start = 0; start < text.Length; start++)
        {
            var quote = text[start];
            if (quote is not ('\'' or '"') || quote == '\'' && start > 0 && char.IsLetterOrDigit(text[start - 1])) continue;
            if (start + 2 < text.Length && text[start + 1] == quote && text[start + 2] == quote) { start += 2; continue; }
            var end = text.IndexOf(quote, start + 1);
            if (end < 0) break;
            Add(text[start..(end + 1)]);
            Cover(start, end + 1);
            start = end;
        }
        foreach (Match match in Codepoint.Matches(text))
        {
            Add(match.Value);
            Cover(match.Index, match.Index + match.Length);
        }
        for (var i = 0; i < text.Length; i++)
        {
            if (skip[i] || !char.IsLetter(text[i])) continue;
            var letter = text[i];
            var edge = (i == 0 || !char.IsLetterOrDigit(text[i - 1])) && (i + 1 == text.Length || !char.IsLetterOrDigit(text[i + 1]));
            if (!edge || letter is 'a' or 'A' or 'I' or 'i') continue;
            Add(letter.ToString(CultureInfo.InvariantCulture));
        }
        return found;
    }
}
