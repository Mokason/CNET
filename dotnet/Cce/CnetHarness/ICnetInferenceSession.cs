// Backend-agnostic inference session contract.
//
// Two implementations exist:
//   * CnetHarnessSession  — the native path (cnet.so / llama context), with the
//     full AICIMO route decision. This is the default and is unchanged.
//   * CnetLlmInferenceSession (assembly CNET.Cce.Llm) — a fully managed path
//     over the CNET.Llm engine, for hosts that cannot or do not want to load
//     the native plugin.
//
// The interface deliberately lives here, in CNET.Cce, and names only types this
// assembly already owns. CNET.Cce therefore keeps no reference to CNET.Llm —
// see dotnet/Cce.Llm/README.md for why that separation is load-bearing.

using System;

namespace CNET.Cce.CnetHarness;

/// <summary>
/// One open inference session. Callers dispose the session to release whatever
/// the backend holds (a native lease and llama context, or managed model
/// memory).
/// </summary>
public interface ICnetInferenceSession : IDisposable
{
    /// <summary>Perform one synchronous, non-streaming generation.</summary>
    CnetHarnessGenerationResult Generate(CnetHarnessGenerateOptions options);

    /// <summary>
    /// Whether <see cref="CnetHarnessGenerateOptions.ContinueFrom"/> is honored
    /// — i.e. the backend can resume a partial assistant reply structurally,
    /// as its own turn in the conversation. Callers fall back to prompt-side
    /// anchoring when false. Default false so implementations opt in.
    /// </summary>
    bool SupportsContinuation => false;

    /// <summary>
    /// Report the sampling profile a generation would apply, without producing
    /// any tokens.
    /// </summary>
    /// <remarks>
    /// The native backend answers this from the AICIMO route decision. Managed
    /// backends have no AICIMO and report their static profile mapping instead;
    /// consult the implementation for what <see cref="CnetHarnessRouteInfo.SelectedAdapter"/>
    /// and <see cref="CnetHarnessRouteInfo.RouteUncertainty"/> mean there.
    /// </remarks>
    CnetHarnessRouteInfo ProbeRoute(string role,
        CnetHarnessSamplingMode overrideMode = CnetHarnessSamplingMode.Auto);
}
