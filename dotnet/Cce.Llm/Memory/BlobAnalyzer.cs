namespace CNET.Cce.Llm.Memory;

/// <summary>
/// Turns text into index/query terms. One analyzer for both sides, so a word
/// matches itself.
/// </summary>
/// <remarks>
/// Terms are maximal runs of <c>[A-Za-z0-9_]</c>, lowercased. The underscore is
/// kept inside terms on purpose — this store's conversations are full of
/// identifiers like <c>q8_0</c> and <c>cnet_harness</c>, and splitting those
/// would turn the most discriminative keys in the corpus into noise. Pure
/// numbers are kept too: "the vault code is 7291" must be recallable by "7291".
/// Stopwords and single characters are dropped; they carry no recall value and
/// would let common words match everything.
/// </remarks>
internal static class BlobAnalyzer
{
    private const int MaxTermLength = 48;

    private static readonly HashSet<string> Stopwords = new(StringComparer.Ordinal)
    {
        "a", "an", "and", "are", "as", "at", "be", "been", "but", "by", "can",
        "did", "do", "does", "for", "from", "had", "has", "have", "he", "her",
        "him", "his", "how", "i", "if", "in", "into", "is", "it", "its", "me",
        "my", "no", "not", "of", "on", "or", "our", "she", "so", "than", "that",
        "the", "their", "them", "then", "there", "these", "they", "this", "to",
        "us", "was", "we", "were", "what", "when", "where", "which", "who",
        "why", "will", "with", "you", "your",
    };

    /// <summary>Extracts terms in order of appearance (duplicates preserved for tf).</summary>
    public static List<string> Tokenize(ReadOnlySpan<char> text)
    {
        var terms = new List<string>();
        int start = -1;

        for (int i = 0; i <= text.Length; i++)
        {
            bool inTerm = i < text.Length && (char.IsAsciiLetterOrDigit(text[i]) || text[i] == '_');
            if (inTerm)
            {
                if (start < 0) start = i;
                continue;
            }
            if (start < 0) continue;

            int len = i - start;
            if (len >= 2 && len <= MaxTermLength)
            {
                string term = text.Slice(start, len).ToString().ToLowerInvariant();
                if (!Stopwords.Contains(term))
                    terms.Add(term);
            }
            start = -1;
        }

        return terms;
    }
}
