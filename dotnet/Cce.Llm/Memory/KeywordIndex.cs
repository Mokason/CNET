namespace CNET.Cce.Llm.Memory;

/// <summary>
/// In-memory inverted index over blobs: term → postings, scored with BM25 plus
/// a recency bonus.
/// </summary>
/// <remarks>
/// Rebuilt from the store file at open and updated incrementally on append —
/// there is no index file to corrupt or drift. At conversation scale (the store
/// is a text file measured in megabytes) a rebuild is milliseconds and a query
/// is microseconds; measured numbers live in the memory-layer README section.
/// Not thread-safe by itself: <see cref="BlobStore"/> serializes access.
/// </remarks>
internal sealed class KeywordIndex
{
    private sealed class Posting
    {
        public long BlobId;
        public int TermFrequency;
    }

    private readonly Dictionary<string, List<Posting>> _postings = new(StringComparer.Ordinal);
    private readonly Dictionary<long, int> _blobTermCounts = [];
    private readonly MemoryOptions _options;
    private long _totalTerms;

    public KeywordIndex(MemoryOptions options) => _options = options;

    /// <summary>Number of indexed blobs.</summary>
    public int DocumentCount => _blobTermCounts.Count;

    /// <summary>Adds one blob. Ids must be added in increasing order.</summary>
    public void Add(MemoryBlob blob)
    {
        List<string> terms = IndexTerms(blob.Text);
        _blobTermCounts[blob.Id] = terms.Count;
        _totalTerms += terms.Count;

        // Aggregate tf locally first so each term gets one posting per blob.
        var tf = new Dictionary<string, int>(StringComparer.Ordinal);
        foreach (string t in terms)
            tf[t] = tf.TryGetValue(t, out int c) ? c + 1 : 1;

        foreach ((string term, int count) in tf)
        {
            if (!_postings.TryGetValue(term, out var list))
                _postings[term] = list = [];
            list.Add(new Posting { BlobId = blob.Id, TermFrequency = count });
        }
    }

    /// <summary>Removes one blob's contributions (postings, lengths, totals).</summary>
    public void Remove(MemoryBlob blob)
    {
        if (!_blobTermCounts.Remove(blob.Id, out int termCount)) return;
        _totalTerms -= termCount;

        foreach (string term in new HashSet<string>(IndexTerms(blob.Text), StringComparer.Ordinal))
        {
            if (!_postings.TryGetValue(term, out var list)) continue;
            list.RemoveAll(posting => posting.BlobId == blob.Id);
            if (list.Count == 0)
                _postings.Remove(term);   // df must shrink or idf drifts
        }
    }

    /// <summary>
    /// Scores blobs against the query. Returns (blobId, score) sorted best-first;
    /// empty when no blob passes the relevance gate.
    /// </summary>
    /// <remarks>
    /// The relevance gate is the precision mechanism: a blob only qualifies if it
    /// shares at least one <em>discriminative</em> term with the query — one whose
    /// document frequency is at most <see cref="MemoryOptions.RelevanceGateMaxDf"/>
    /// of the store (always allowing df = 1). Common words accumulate no right to
    /// recall anything, so a generic query returns nothing rather than the
    /// least-irrelevant blob. Recall that returns nothing is a feature.
    /// </remarks>
    public List<(long BlobId, double Score)> Query(string query, long newestBlobId,
                                                   bool relaxed = false)
    {
        int n = DocumentCount;
        if (n == 0) return [];

        // Distinct query terms; duplicate query words don't multiply evidence.
        var queryTerms = new HashSet<string>(BlobAnalyzer.Tokenize(query), StringComparer.Ordinal);
        if (_options.UseHashEmbedBoost)
            foreach (string tri in CharTrigrams(query))
                queryTerms.Add(tri);
        if (queryTerms.Count == 0) return [];

        int maxDiscriminativeDf = Math.Max(1, (int)Math.Floor(n * _options.RelevanceGateMaxDf));
        double avgLen = Math.Max(1.0, _totalTerms / (double)n);
        double k1 = _options.Bm25K1, b = _options.Bm25B;
        double embedW = _options.UseHashEmbedBoost ? _options.HashEmbedWeight : 0.0;

        var scores = new Dictionary<long, double>();
        var gatePassed = new HashSet<long>();
        var distinctMatches = new Dictionary<long, int>();

        foreach (string term in queryTerms)
        {
            if (!_postings.TryGetValue(term, out var postings)) continue;

            // A posting without length metadata is stale and cannot be scored.
            // Remove it defensively so an older/corrupt in-memory index fails
            // closed instead of throwing or influencing document frequency.
            postings.RemoveAll(p => !_blobTermCounts.ContainsKey(p.BlobId));
            if (postings.Count == 0)
            {
                _postings.Remove(term);
                continue;
            }

            int df = postings.Count;
            double idf = Math.Log(1.0 + (n - df + 0.5) / (df + 0.5));
            bool discriminative = df <= maxDiscriminativeDf;
            bool isTri = term.StartsWith("§t:", StringComparison.Ordinal);
            double w = isTri ? embedW : 1.0;

            foreach (Posting p in postings)
            {
                double len = _blobTermCounts[p.BlobId];
                double tfNorm = p.TermFrequency * (k1 + 1)
                              / (p.TermFrequency + k1 * (1 - b + b * len / avgLen));
                scores[p.BlobId] = scores.GetValueOrDefault(p.BlobId) + w * idf * tfNorm;
                if (discriminative && !isTri) gatePassed.Add(p.BlobId);
                if (!isTri)
                    distinctMatches[p.BlobId] = distinctMatches.GetValueOrDefault(p.BlobId) + 1;
            }
        }

        // Second gate clause: two or more distinct query terms are evidence even
        // when each is individually common. A store concentrated on one topic
        // makes its own core words high-df; without this clause the store's main
        // subject would become unrecallable. A single shared common word still
        // recalls nothing.
        foreach ((long id, int matches) in distinctMatches)
            if (matches >= 2) gatePassed.Add(id);

        // Relaxed mode drops the document-frequency gate, but still requires
        // at least one lexical token match. Character trigrams may improve the
        // ranking of a supported match; they are never evidence on their own.
        if (relaxed)
            foreach (long id in distinctMatches.Keys) gatePassed.Add(id);

        if (gatePassed.Count == 0) return [];

        var results = new List<(long BlobId, double Score)>(gatePassed.Count);
        foreach (long id in gatePassed)
        {
            // Newer blobs get up to +RecencyWeight; ancient ones approach +0.
            double ageRank = newestBlobId > 0 ? id / (double)newestBlobId : 1.0;
            results.Add((id, scores[id] * (1.0 + _options.RecencyWeight * ageRank)));
        }

        // Deterministic: score desc, then newest first.
        results.Sort((x, y) =>
        {
            int c = y.Score.CompareTo(x.Score);
            return c != 0 ? c : y.BlobId.CompareTo(x.BlobId);
        });
        return results;
    }

    private List<string> IndexTerms(string text)
    {
        List<string> terms = BlobAnalyzer.Tokenize(text);
        if (_options.UseHashEmbedBoost)
            terms.AddRange(CharTrigrams(text));
        return terms;
    }

    /// <summary>Character trigrams as pseudo-terms (§t:abc) for hash-embed boost.</summary>
    private static IEnumerable<string> CharTrigrams(string text)
    {
        if (string.IsNullOrEmpty(text) || text.Length < 3) yield break;
        string s = text.ToLowerInvariant();
        for (int i = 0; i + 2 < s.Length; i++)
        {
            char a = s[i], b = s[i + 1], c = s[i + 2];
            if (!char.IsLetterOrDigit(a) || !char.IsLetterOrDigit(b) || !char.IsLetterOrDigit(c))
                continue;
            yield return $"§t:{a}{b}{c}";
        }
    }
}
