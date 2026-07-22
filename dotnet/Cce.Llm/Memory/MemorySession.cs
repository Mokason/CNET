using CNET.Cce.CnetHarness;

namespace CNET.Cce.Llm.Memory;

/// <summary>One memory-augmented generation: the inner result plus provenance.</summary>
/// <param name="Result">The unmodified result from the inner session.</param>
/// <param name="UsedBlobIds">Store ids of the memories that were in the prompt — resolve
/// them via <see cref="BlobStore.Get"/> to show the user exactly what the model saw.</param>
/// <param name="PromptSystemText">The full system text sent, for audit.</param>
public sealed record MemoryGenerationResult(
    CnetHarnessGenerationResult Result,
    IReadOnlyList<long> UsedBlobIds,
    string PromptSystemText);

/// <summary>
/// Composes an <see cref="ICnetInferenceSession"/> with a
/// <see cref="ConversationMemory"/>: every generation gets a fresh, budgeted
/// context rebuilt from keyword recall, and every exchange is stored back.
/// </summary>
/// <remarks>
/// This is the whole "ghost memory" loop. The context window stays small and
/// focused on purpose — long windows decay decode speed (measured 30 → 13 tok/s
/// from 0.2k to 8.6k context on Llama-1B) — while the store underneath is
/// unbounded and outlives every session. Works over either backend since it
/// only touches the shared interface.
/// </remarks>
public sealed class MemorySession
{
    private readonly ICnetInferenceSession _session;
    private readonly ConversationMemory _memory;
    private readonly int _contextWindowTokens;

    /// <param name="session">Inner session; not owned, caller disposes.</param>
    /// <param name="memory">Memory layer; its token counter must belong to this session's model.</param>
    /// <param name="contextWindowTokens">
    /// The session's window (native: ContextTokens/n_ctx; managed:
    /// <see cref="CnetLlmInferenceSession.EffectiveContextTokens"/>).
    /// </param>
    public MemorySession(ICnetInferenceSession session, ConversationMemory memory, int contextWindowTokens)
    {
        _session = session ?? throw new ArgumentNullException(nameof(session));
        _memory = memory ?? throw new ArgumentNullException(nameof(memory));
        ArgumentOutOfRangeException.ThrowIfLessThan(contextWindowTokens, 256);
        _contextWindowTokens = contextWindowTokens;
    }

    /// <summary>
    /// Generates with recalled memory in context, then stores the exchange.
    /// </summary>
    /// <param name="system">Stable system prompt — keep it stable: both backends
    /// skip re-prefilling the longest unchanged prompt prefix.</param>
    /// <param name="user">The user message; also the recall query.</param>
    /// <param name="maxTokens">Answer budget. The memory block is packed into
    /// what the window leaves after reserving this.</param>
    /// <param name="sampling">Sampling profile, as on the raw session.</param>
    /// <param name="seed">Sampling seed, as on the raw session.</param>
    /// <param name="role">AICIMO role string (native backend); ignored by the managed one.</param>
    public MemoryGenerationResult Generate(
        string? system, string user, uint maxTokens = 256,
        CnetHarnessSamplingMode sampling = CnetHarnessSamplingMode.Auto,
        uint seed = 424242, string role = "memory")
    {
        ArgumentException.ThrowIfNullOrEmpty(user);

        // The window must fit the answer AND a usable prompt. Clamp the answer
        // budget rather than crash on a negative prompt budget; reserve at
        // least 128 tokens of prompt so a clamped call still carries the
        // question. (Arithmetic in long: maxTokens is uint and may exceed int.)
        // The constructor floors the window at 256, so the clamp target is
        // always >= 128 — this can reduce maxTokens but never zero it.
        const int MinPromptReserve = 128;
        maxTokens = (uint)Math.Min((long)maxTokens, _contextWindowTokens - MinPromptReserve);

        int promptBudget = _contextWindowTokens - (int)maxTokens;
        MemoryContext context = _memory.BuildContext(system, user, promptBudget);

        CnetHarnessGenerationResult result = _session.Generate(new CnetHarnessGenerateOptions
        {
            System = context.SystemText.Length > 0 ? context.SystemText : null,
            User = user,
            Role = role,
            MaxTokens = maxTokens,
            Seed = seed,
            Sampling = sampling,
        });

        _memory.Remember("user", user);
        _memory.Remember("assistant", result.Text);
        _memory.NextTurn();

        return new MemoryGenerationResult(result, context.UsedBlobIds, context.SystemText);
    }
}
