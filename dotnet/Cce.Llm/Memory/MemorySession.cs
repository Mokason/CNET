using CNET.Cce.CnetHarness;

namespace CNET.Cce.Llm.Memory;

/// <summary>One model-directed memory lookup performed during a generation.</summary>
/// <param name="Query">The keywords the model asked for.</param>
/// <param name="BlobIds">What the store served — empty when nothing matched.</param>
public sealed record LookupRound(string Query, IReadOnlyList<long> BlobIds);

/// <summary>One memory-augmented generation: the inner result plus provenance.</summary>
/// <param name="Result">The final result from the inner session.</param>
/// <param name="UsedBlobIds">Store ids of every memory that reached the prompt —
/// gate-recalled and model-looked-up alike; resolve via <see cref="BlobStore.Get"/>.</param>
/// <param name="PromptSystemText">The final system text sent, for audit.</param>
/// <param name="Lookups">Model-directed recall rounds, in order.</param>
public sealed record MemoryGenerationResult(
    CnetHarnessGenerationResult Result,
    IReadOnlyList<long> UsedBlobIds,
    string PromptSystemText,
    IReadOnlyList<LookupRound> Lookups);

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
    /// <summary>
    /// The lookup protocol taught to the model. Text-based so it works over
    /// every backend — the ABI has no tool-calling. The model can only REQUEST
    /// a search; results come back as the same verbatim, receipted blobs as
    /// gate recall, and an empty result is stated rather than papered over.
    /// </summary>
    private const string LookupProtocol =
        "\n### Memory lookup\n" +
        "Any memory shown above is only what keyword-matched this question — the " +
        "store may hold the answer under different words. Before saying you have " +
        "no record of something the user refers to, search for it: reply with " +
        "EXACTLY one line and nothing else:\nRECALL: <two to five keywords>\n" +
        "Use the words the original conversation would have used, not the user's " +
        "current phrasing. Results will be added and you will be asked again. " +
        "Only after a lookup finds nothing, say memory does not contain it — " +
        "never invent memories.\n";

    private readonly ICnetInferenceSession _session;
    private readonly ConversationMemory _memory;
    private readonly int _contextWindowTokens;
    private readonly Func<string, int> _countTokens;
    private readonly int _maxLookupRounds;
    private readonly int _resultsPerLookup;

    /// <summary>Invoked after each model-directed lookup — lets a UI narrate the search.</summary>
    public Action<LookupRound>? OnLookup { get; set; }

    /// <param name="session">Inner session; not owned, caller disposes.</param>
    /// <param name="memory">Memory layer; its token counter must belong to this session's model.</param>
    /// <param name="contextWindowTokens">
    /// The session's window (native: ContextTokens/n_ctx; managed:
    /// <see cref="CnetLlmInferenceSession.EffectiveContextTokens"/>).
    /// </param>
    /// <param name="countTokens">
    /// Token counter used to budget the lookup blocks appended by the recall
    /// loop; pass the session's real counter. Defaults to the chars/3 heuristic.
    /// </param>
    /// <param name="maxLookupRounds">
    /// Model-directed recall rounds per generation. 0 disables the loop; each
    /// round costs one extra inner generation. Models that ignore the protocol
    /// simply never trigger it.
    /// </param>
    /// <param name="resultsPerLookup">Most blobs served per lookup.</param>
    public MemorySession(ICnetInferenceSession session, ConversationMemory memory,
                         int contextWindowTokens, Func<string, int>? countTokens = null,
                         int maxLookupRounds = 2, int resultsPerLookup = 5)
    {
        _session = session ?? throw new ArgumentNullException(nameof(session));
        _memory = memory ?? throw new ArgumentNullException(nameof(memory));
        ArgumentOutOfRangeException.ThrowIfLessThan(contextWindowTokens, 256);
        ArgumentOutOfRangeException.ThrowIfNegative(maxLookupRounds);
        ArgumentOutOfRangeException.ThrowIfLessThan(resultsPerLookup, 1);
        _contextWindowTokens = contextWindowTokens;
        _countTokens = countTokens ?? (text => text.Length / 3 + 1);
        _maxLookupRounds = maxLookupRounds;
        _resultsPerLookup = resultsPerLookup;
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

        // ── model-directed recall (chain-of-thought over the store) ──
        // Gate recall is push-only: it guesses relevance from the user's exact
        // words. The loop below adds pull: the model reads what the gate found,
        // and when the missing fact is phrased differently than it was stored
        // ("connect to the network" vs "wifi password"), it requests a lookup
        // itself. Each round is one extra inner generation; the results are the
        // same verbatim receipted blobs as gate recall.
        string systemText = context.SystemText;
        var usedIds = new List<long>(context.UsedBlobIds);
        var visibleIds = new HashSet<long>(context.UsedBlobIds);
        var lookups = new List<LookupRound>();
        var seenQueries = new HashSet<string>(StringComparer.OrdinalIgnoreCase);

        // Remaining prompt headroom for protocol + lookup blocks. BuildContext
        // packed SystemText under promptBudget; everything we append must fit
        // in what it left over (the user message was already costed there).
        int headroom = promptBudget - _countTokens(systemText) - _countTokens(user);
        bool loopEnabled = _maxLookupRounds > 0 && headroom > _countTokens(LookupProtocol) + 32;
        if (loopEnabled)
        {
            systemText += LookupProtocol;
            headroom -= _countTokens(LookupProtocol);
        }

        CnetHarnessGenerationResult result = GenerateOnce(systemText, user, maxTokens, sampling, seed, role);

        int rounds = 0;
        while (loopEnabled && rounds < _maxLookupRounds && TryParseRecall(result.Text, out string query))
        {
            rounds++;
            var served = new List<long>();
            string header = $"\n### Lookup \"{query}\"\n";
            var block = new System.Text.StringBuilder(header);

            if (!seenQueries.Add(query))
            {
                block.Append("(already searched — answer now with what is shown)\n");
            }
            else
            {
                List<MemoryBlob> hits = _memory.Lookup(query, _resultsPerLookup, visibleIds);
                if (hits.Count == 0)
                {
                    block.Append("(no stored memory matches — if that was the missing fact, " +
                                 "say memory does not contain it)\n");
                }
                foreach (MemoryBlob hit in hits)
                {
                    string line = _memory.RenderMemory(hit) + "\n";
                    if (_countTokens(block.ToString()) + _countTokens(line) > headroom) break;
                    block.Append(line);
                    served.Add(hit.Id);
                    visibleIds.Add(hit.Id);
                }
            }

            int blockTokens = _countTokens(block.ToString());
            if (blockTokens > headroom) break;      // window exhausted: current text stands
            headroom -= blockTokens;
            systemText += block.ToString();

            var round = new LookupRound(query, served);
            lookups.Add(round);
            usedIds.AddRange(served);
            OnLookup?.Invoke(round);

            result = GenerateOnce(systemText, user, maxTokens, sampling, seed, role);
        }

        // Rounds exhausted but the model still wants to search: one forced
        // answer, so a lookup-happy model cannot return scaffolding as a reply.
        if (loopEnabled && TryParseRecall(result.Text, out _))
        {
            const string NoMore = "\n(No more lookups available — answer now using only what is shown.)\n";
            if (_countTokens(NoMore) <= headroom)
            {
                systemText += NoMore;
                result = GenerateOnce(systemText, user, maxTokens, sampling, seed, role);
            }
        }

        // Only the real exchange is stored — RECALL scaffolding never becomes memory.
        _memory.Remember("user", user);
        _memory.Remember("assistant", result.Text);
        _memory.NextTurn();

        return new MemoryGenerationResult(result, usedIds, systemText, lookups);
    }

    private CnetHarnessGenerationResult GenerateOnce(
        string systemText, string user, uint maxTokens,
        CnetHarnessSamplingMode sampling, uint seed, string role) =>
        _session.Generate(new CnetHarnessGenerateOptions
        {
            System = systemText.Length > 0 ? systemText : null,
            User = user,
            Role = role,
            MaxTokens = maxTokens,
            Seed = seed,
            Sampling = sampling,
        });

    /// <summary>
    /// A reply is a lookup request iff its first non-empty line starts with
    /// "RECALL:". Anything after that line is ignored — models sometimes keep
    /// talking. Recalled blob text is data, never parsed: only the model's own
    /// output reaches this, so stored "RECALL:" strings cannot steer the loop.
    /// </summary>
    internal static bool TryParseRecall(string? text, out string query)
    {
        query = "";
        if (string.IsNullOrWhiteSpace(text)) return false;
        foreach (string rawLine in text.Split('\n'))
        {
            string line = rawLine.Trim();
            if (line.Length == 0) continue;
            if (!line.StartsWith("RECALL:", StringComparison.Ordinal)) return false;
            query = line["RECALL:".Length..].Trim().Trim('"', '\'', '`');
            if (query.Length > 200) query = query[..200];
            return query.Length > 0;
        }
        return false;
    }
}
