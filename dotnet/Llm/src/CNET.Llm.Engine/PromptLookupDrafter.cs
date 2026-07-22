namespace CNET.Llm.Engine;

/// <summary>
/// Draft-free speculation: proposes continuations by matching the recent token
/// suffix against text already in the context, instead of running a draft model.
/// </summary>
/// <remarks>
/// The point of this over model-based speculation is bandwidth. Decode is pinned
/// at the memory roof — every weight byte is streamed once per token — so a draft
/// model is not free: its weights compete for exactly the bandwidth the target is
/// already saturating. Measured on this repo, a 0.73x-sized draft made decode
/// 2.6-3.9x <em>slower</em>. An n-gram drafter streams no weights at all, so a
/// speculation round costs one target pass regardless of how many tokens it
/// proposes, and any acceptance above zero is a win.
/// <para>
/// It only pays where the output repeats something already in context — quoted
/// input, structured or templated output, code, JSON, tool calls. On free-form
/// prose it finds nothing and cleanly reports zero candidates, which costs one
/// suffix scan over the sequence and nothing else.
/// </para>
/// </remarks>
public sealed class PromptLookupDrafter
{
    private readonly int _maxNgram;
    private readonly int _minNgram;
    private readonly int _maxCandidates;

    /// <param name="maxNgram">Longest suffix to try matching. Longer matches are more reliable but rarer.</param>
    /// <param name="minNgram">Shortest suffix to accept a match on. Below ~2 the matches are mostly noise.</param>
    /// <param name="maxCandidates">Most tokens to propose from a single match.</param>
    public PromptLookupDrafter(int maxNgram = 3, int minNgram = 2, int maxCandidates = 10)
    {
        if (maxNgram < 1) throw new ArgumentOutOfRangeException(nameof(maxNgram));
        if (minNgram < 1 || minNgram > maxNgram) throw new ArgumentOutOfRangeException(nameof(minNgram));
        if (maxCandidates < 1) throw new ArgumentOutOfRangeException(nameof(maxCandidates));

        _maxNgram = maxNgram;
        _minNgram = minNgram;
        _maxCandidates = maxCandidates;
    }

    /// <summary>Most tokens this drafter can propose in one round.</summary>
    public int MaxCandidates => _maxCandidates;

    /// <summary>
    /// Proposes the tokens that followed the most recent earlier occurrence of
    /// the current suffix.
    /// </summary>
    /// <param name="sequence">Full token sequence so far: prompt followed by everything generated.</param>
    /// <param name="candidates">Receives the proposal. Must hold at least <see cref="MaxCandidates"/>.</param>
    /// <returns>Number of tokens proposed; 0 when no usable match exists.</returns>
    public int Draft(ReadOnlySpan<int> sequence, Span<int> candidates)
    {
        if (candidates.Length < _maxCandidates)
            throw new ArgumentException(
                $"candidates must hold at least {_maxCandidates} tokens", nameof(candidates));

        int n = sequence.Length;
        if (n < _minNgram + 1) return 0;

        // Longest match first: a longer shared suffix is a stronger predictor, so
        // trying descending n gives better acceptance than a fixed window.
        for (int size = Math.Min(_maxNgram, n - 1); size >= _minNgram; size--)
        {
            ReadOnlySpan<int> suffix = sequence[^size..];

            // Search backwards: the most recent occurrence is the most likely to
            // continue the same way, and it is usually the nearest one too.
            for (int start = n - size - 1; start >= 0; start--)
            {
                if (!sequence.Slice(start, size).SequenceEqual(suffix))
                    continue;

                int from = start + size;
                int available = n - from;
                if (available <= 0) continue;

                int take = Math.Min(_maxCandidates, available);
                sequence.Slice(from, take).CopyTo(candidates);
                return take;
            }
        }

        return 0;
    }
}
