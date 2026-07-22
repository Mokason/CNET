namespace CNET.Cce.Llm;

/// <summary>
/// Managed-backend-specific session options. Kept separate from
/// <see cref="CNET.Cce.CnetHarness.CnetHarnessConfig"/>, which is shared with
/// the native backend and stays backend-neutral.
/// </summary>
public sealed class CnetLlmSessionOptions
{
    /// <summary>
    /// KV caches retained across Generate calls for longest-prefix reuse.
    /// 1 covers the sequential-conversation pattern (each prompt shares its
    /// stable header with the previous one — measured 13x cheaper prefill for a
    /// ~700-token header). 0 disables retention entirely.
    /// </summary>
    /// <remarks>
    /// Each retained entry holds a full KV cache: roughly
    /// <c>layers × kvHeads × headDim × 2 × bytes-per-value × windowTokens</c>.
    /// Keep this at 1 unless the host interleaves multiple conversations on one
    /// session.
    /// </remarks>
    public int PrefixCacheEntries { get; init; } = 1;
}
