namespace CNET.Cce.Llm.Memory;

/// <summary>Tunables for storage granularity, recall precision, and budgeting.</summary>
/// <remarks>
/// The defaults are set for precision over recall: it is better for the memory
/// block to be empty than to inject weakly-related text, because everything
/// recalled lands verbatim in the model's context and competes with the actual
/// question. That asymmetry is the anti-hallucination stance of this layer —
/// the model can only "remember" text that really exists in the store, and only
/// when the query gives a discriminative reason to surface it.
/// </remarks>
public sealed class MemoryOptions
{
    /// <summary>Most blobs recalled into one prompt.</summary>
    public int TopK { get; init; } = 8;

    /// <summary>Turns of the current session always included verbatim, unconditionally.</summary>
    public int RecentTurns { get; init; } = 2;

    /// <summary>
    /// Earliest session blobs injected when the question contains an ordering
    /// term ("first", "beginning", …) — keyword recall cannot see ordering, so
    /// temporal questions get the session's opening verbatim.
    /// </summary>
    public int TemporalAnchorBlobs { get; init; } = 10;

    /// <summary>
    /// Messages longer than this are split at paragraph boundaries into multiple
    /// blobs, so recall stays granular without producing orphaned fragments.
    /// </summary>
    public int MaxBlobTokens { get; init; } = 320;

    /// <summary>
    /// A query term only counts as evidence when its document frequency is at or
    /// below this fraction of the store — common words cannot summon memories.
    /// A blob with no discriminative query term is never recalled, whatever its
    /// aggregate score.
    /// </summary>
    public double RelevanceGateMaxDf { get; init; } = 0.25;

    /// <summary>Multiplicative bonus for newer blobs: score × (1 + w × age-rank).</summary>
    public double RecencyWeight { get; init; } = 0.25;

    /// <summary>BM25 term-frequency saturation.</summary>
    public double Bm25K1 { get; init; } = 1.2;

    /// <summary>BM25 length normalization.</summary>
    public double Bm25B { get; init; } = 0.75;

    /// <summary>
    /// Tokens held back from the prompt budget to absorb chat-template overhead
    /// (role markers, separators) that raw text counts miss.
    /// </summary>
    public int SafetyMarginTokens { get; init; } = 64;
}
