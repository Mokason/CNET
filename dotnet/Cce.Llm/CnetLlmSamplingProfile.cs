// Sampling-profile mapping for the managed backend.
//
// The native harness resolves CnetHarnessSamplingMode through AICIMO, which
// inspects the request role and may override the caller's choice. The managed
// backend has no AICIMO, so it applies the static table below and always
// reports AicimoOverride = false. Generate() and ProbeRoute() both resolve
// through this one function so the probe never disagrees with what generation
// actually applied.

using CNET.Cce.CnetHarness;

namespace CNET.Cce.Llm;

/// <summary>Concrete sampler settings a <see cref="CnetHarnessSamplingMode"/> maps to.</summary>
public readonly record struct CnetLlmSamplingProfile(
    CnetHarnessSamplingMode Mode,
    float Temperature,
    float TopP,
    uint TopK,
    float MinP)
{
    /// <summary>
    /// Resolve a requested mode to concrete sampler settings.
    /// </summary>
    /// <remarks>
    /// <see cref="CnetHarnessSamplingMode.Auto"/> defers to AICIMO on the native
    /// backend. There is no AICIMO here, so Auto resolves to
    /// <see cref="CnetHarnessSamplingMode.Balanced"/> and the returned
    /// <see cref="Mode"/> reports Balanced — callers reading
    /// <c>EffectiveSampling</c> see what was actually applied, not the request.
    /// </remarks>
    public static CnetLlmSamplingProfile Resolve(CnetHarnessSamplingMode requested)
        => requested switch
        {
            CnetHarnessSamplingMode.Deterministic =>
                new(CnetHarnessSamplingMode.Deterministic, 0.0f, 1.0f, 0u, 0.0f),
            CnetHarnessSamplingMode.Focused =>
                new(CnetHarnessSamplingMode.Focused, 0.3f, 0.90f, 20u, 0.05f),
            CnetHarnessSamplingMode.Exploratory =>
                new(CnetHarnessSamplingMode.Exploratory, 1.0f, 0.98f, 80u, 0.02f),
            // Balanced, and Auto which has no AICIMO to defer to.
            _ => new(CnetHarnessSamplingMode.Balanced, 0.7f, 0.95f, 40u, 0.05f),
        };
}
