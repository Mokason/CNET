using CNET.Cce.CnetHarness;

namespace CNET.Cce.Llm.Memory;

/// <summary>One model-directed action performed during a generation.</summary>
/// <param name="Query">The keywords or expression the model asked for.</param>
/// <param name="BlobIds">What the store served — empty when nothing matched.</param>
/// <param name="Kind">"recall" (memory search), "read" (document sections),
/// or "calc" (exact computation).</param>
/// <param name="Output">The computed result, for calc actions.</param>
public sealed record LookupRound(string Query, IReadOnlyList<long> BlobIds,
                                 string Kind = "recall", string? Output = null);

/// <summary>One memory-augmented generation: the inner result plus provenance.</summary>
/// <param name="Result">The final result from the inner session.</param>
/// <param name="UsedBlobIds">Store ids of every memory that reached the prompt —
/// gate-recalled and model-looked-up alike; resolve via <see cref="BlobStore.Get"/>.</param>
/// <param name="PromptSystemText">The final system text sent, for audit.</param>
/// <param name="Lookups">Model-directed recall rounds, in order.</param>
/// <param name="Truncated">The answer stopped because it hit the token budget,
/// not because it finished — a "continue" next turn will resume at the cut.
/// With auto-continue enabled this is only true once the round cap is also
/// exhausted.</param>
/// <param name="AutoContinues">Automatic resume rounds this answer needed;
/// <see cref="CnetHarnessGenerationResult.Text"/> is the stitched whole.</param>
/// <param name="Exact">Answered by the exact-arithmetic lane — computed, not
/// generated; no model was invoked and no memory context was built.</param>
/// <param name="CertifiedUnit">Answered from certified knowledge: the sealed
/// specialist whose record was served verbatim; null for model answers.</param>
/// <param name="Reflections">Output-reflection rounds: the judge flagged a
/// draft and one clean regeneration was attempted. Bounded at 1 by design —
/// awareness without a cap is a system stuck with its own thoughts.</param>
public sealed record MemoryGenerationResult(
    CnetHarnessGenerationResult Result,
    IReadOnlyList<long> UsedBlobIds,
    string PromptSystemText,
    IReadOnlyList<LookupRound> Lookups,
    bool Truncated,
    int AutoContinues,
    bool Exact = false,
    string? CertifiedUnit = null,
    int Reflections = 0);

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
        "\n### Actions\n" +
        "Memory and documents shown above are only what keyword-matched this " +
        "question — the store may hold the answer under different words. You can " +
        "act before answering: reply with EXACTLY one line and nothing else.\n" +
        "RECALL: <two to five keywords>   — search stored conversation memory\n" +
        "READ: <document keywords>        — pull matching document sections\n" +
        "CALC: <arithmetic expression>    — exact computation, never wrong;\n" +
        "     USE IT for ANY non-trivial arithmetic — large products, powers, roots\n" +
        "     (isqrt/sqrt/floor/abs supported) — instead of computing digits yourself\n" +
        "Results will be added and you will be asked again. Use the words the " +
        "original material would use. Only after a search finds nothing, say the " +
        "memory or documents do not cover it — never invent.\n";

    /// <summary>User-slot instruction for structural resumes.</summary>
    private const string ResumeInstruction =
        "Continue your message exactly where it was cut off. Output only the " +
        "remaining text — do not repeat anything already written, do not restart, no preamble.";

    /// <summary>Appended on the salvage retry when a resume round emits nothing.</summary>
    private const string NoDeliberationNudge =
        " Do not spend tokens deliberating — output the continuation text immediately.";

    private readonly ICnetInferenceSession _session;
    private readonly ConversationMemory _memory;
    private readonly int _contextWindowTokens;
    private readonly Func<string, int> _countTokens;
    private readonly int _maxLookupRounds;
    private readonly int _resultsPerLookup;
    private readonly int _maxAutoContinues;

    /// <summary>
    /// The full text of the immediately previous answer iff it was cut off by
    /// the token budget; null once a turn completes normally. "Continue" only
    /// ever refers to the turn just before it — an intervening completed turn
    /// clears this, so a stale resume is impossible.
    /// </summary>
    private string? _pendingContinuation;

    /// <summary>Invoked after each model-directed lookup — lets a UI narrate the search.</summary>
    public Action<LookupRound>? OnLookup { get; set; }

    /// <summary>Fired before each inner model generation, so a streaming UI can
    /// reset its filter per generation (action scaffolding vs the real answer).</summary>
    public Action? OnInnerGenerationStart { get; set; }

    /// <summary>Fired after each inner model generation completes.</summary>
    public Action? OnInnerGenerationEnd { get; set; }

    /// <summary>
    /// The exact-arithmetic lane (ported from AICIMO): arithmetic questions
    /// are answered by computation before any model runs — 0.2 ms of decimal
    /// parsing instead of seconds of decode, and never wrong. Declines fall
    /// through to the model untouched. Disable for A/B comparison.
    /// </summary>
    public bool ExactLane { get; set; } = true;

    /// <summary>
    /// Rung 4's routing layer: when set, questions grounded in a certified
    /// record are answered from that record — the sealed specialist's exact
    /// content — before the model is consulted. Declines route onward.
    /// </summary>
    public Routing.RecordRouter? Router { get; set; }

    /// <summary>
    /// Optional adaptive judge: live conversation labels it — a
    /// correction-shaped turn marks the previous answer bad, a
    /// confirmation-shaped turn marks it good. The judge never gates
    /// anything here; chat answers are the user's to judge, not taste's.
    /// </summary>
    public Judgment.AdaptiveJudge? Judge { get; set; }

    /// <summary>
    /// Optional certified-tool registry: the TOOL: action invokes declarative
    /// tools the model proposed and the verifier certified. Sandboxed by
    /// construction — no code runs, only the trusted interpreter.
    /// </summary>
    public Tools.IToolProvider? Tools { get; set; }

    private string? _lastAssistantText;

    private static readonly string[] CorrectionOpeners =
        ["wrong", "no,", "no.", "nope", "incorrect", "that's wrong", "thats wrong",
         "that's not", "thats not", "actually,", "i said", "i meant", "not true"];

    private static readonly string[] ConfirmationOpeners =
        ["thanks", "thank you", "perfect", "correct", "exactly", "great",
         "nice", "awesome", "it works", "that works", "good job", "well done"];

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
    /// <param name="maxAutoContinues">
    /// Automatic resume rounds when an answer hits the token budget: the layer
    /// re-prompts with the answer-so-far and stitches the chunks into one text,
    /// no user intervention. 0 restores manual-only continuation. Each round is
    /// one extra inner generation, so this bounds worst-case cost per turn.
    /// </param>
    public MemorySession(ICnetInferenceSession session, ConversationMemory memory,
                         int contextWindowTokens, Func<string, int>? countTokens = null,
                         int maxLookupRounds = 2, int resultsPerLookup = 5,
                         int maxAutoContinues = 3)
    {
        _session = session ?? throw new ArgumentNullException(nameof(session));
        _memory = memory ?? throw new ArgumentNullException(nameof(memory));
        ArgumentOutOfRangeException.ThrowIfLessThan(contextWindowTokens, 256);
        ArgumentOutOfRangeException.ThrowIfNegative(maxLookupRounds);
        ArgumentOutOfRangeException.ThrowIfLessThan(resultsPerLookup, 1);
        ArgumentOutOfRangeException.ThrowIfNegative(maxAutoContinues);
        _contextWindowTokens = contextWindowTokens;
        _countTokens = countTokens ?? (text => text.Length / 3 + 1);
        _maxLookupRounds = maxLookupRounds;
        _resultsPerLookup = resultsPerLookup;
        _maxAutoContinues = maxAutoContinues;
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

        // Outcome feedback for the PREVIOUS turn's certified serve (if any):
        // a correction-shaped turn demotes the unit that earned it.
        Router?.ObserveUserTurn(user);

        // Consequence-labeled judge evidence from live conversation.
        if (Judge is not null && _lastAssistantText is not null)
        {
            string head = user.TrimStart().ToLowerInvariant();
            if (CorrectionOpeners.Any(head.StartsWith))
                Judge.Learn(_lastAssistantText, good: false, source: "chat-correction");
            else if (ConfirmationOpeners.Any(head.StartsWith))
                Judge.Learn(_lastAssistantText, good: true, source: "chat-confirmation");
            _lastAssistantText = null;   // feedback window is one turn
        }

        // ── exact lane: computed truth beats generated truth ──
        // Runs before context building because no context can improve an
        // exact answer, and a model's opinion can only degrade one. The
        // exchange still becomes memory — exact answers are conversation too.
        if (ExactLane && Verify.ExactArithmetic.TryAnswer(user, out string exact))
        {
            _memory.Remember("user", user);
            _memory.Remember("assistant", exact);
            _memory.NextTurn();
            _pendingContinuation = null;
            _lastAssistantText = exact;
            var exactResult = new CnetHarnessGenerationResult(
                exact, 0, 0, 0, 0, 0, 0, CnetHarnessSamplingMode.Deterministic,
                false, 0, 1, 0, 0);
            return new MemoryGenerationResult(exactResult, [], "", [],
                Truncated: false, AutoContinues: 0, Exact: true);
        }

        // ── certified knowledge before model opinion ──
        if (Router?.TryRoute(user) is { } serve)
        {
            _memory.Remember("user", user);
            _memory.Remember("assistant", serve.RecordText);
            _memory.NextTurn();
            _pendingContinuation = null;
            _lastAssistantText = serve.RecordText;
            var servedResult = new CnetHarnessGenerationResult(
                serve.RecordText, 0, 0, 0, 0, 0, 0,
                CnetHarnessSamplingMode.Deterministic, false, 0, 1, 0, 0);
            return new MemoryGenerationResult(servedResult, [], "", [],
                Truncated: false, AutoContinues: 0, Exact: false,
                CertifiedUnit: serve.UnitName);
        }

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

        // ── continuation ──
        // The previous answer hit the token budget and the user asked to go on:
        // anchor the model at the exact cut so it resumes instead of restarting
        // from the top (the observed failure: a truncated JSON reply + "continue"
        // regenerated the same opening stages and truncated again). Preferred
        // anchor is structural — the partial reply as a real assistant turn,
        // the shape chat models are trained to continue. Backends without that
        // get the partial quoted into the system prompt instead.
        bool continuing = _pendingContinuation is not null && IsContinueRequest(user);
        bool structural = _session.SupportsContinuation;
        string innerUser = user;
        string? continueFrom = null;
        if (continuing)
        {
            if (structural)
            {
                continueFrom = ClipForResume(_pendingContinuation!,
                    headroom - _countTokens(ResumeInstruction) - 32);
                if (continueFrom is not null)
                {
                    innerUser = user + "\n(" + ResumeInstruction + ")";
                    headroom -= _countTokens(continueFrom) + _countTokens(ResumeInstruction);
                }
            }
            if (continueFrom is null)
            {
                string tail = _pendingContinuation!;
                if (tail.Length > 600) tail = tail[^600..];
                string block =
                    "\n### Continuation\n" +
                    "Your previous reply was cut off mid-output by the token limit. " +
                    "It ended with:\n…" + tail + "\n" +
                    "Resume EXACTLY at that cut — output only the remaining text. " +
                    "Do not repeat anything already written, do not restart, no preamble.\n";
                if (_countTokens(block) <= headroom)
                {
                    systemText += block;
                    headroom -= _countTokens(block);
                }
            }
        }

        // A resume turn needs no lookups — and the two instruction blocks
        // ("reply with one line" vs "output only the remaining text") conflict.
        bool loopEnabled = !continuing && _maxLookupRounds > 0 &&
                           headroom > _countTokens(LookupProtocol) + 32;
        if (loopEnabled)
        {
            string protocol = LookupProtocol;
            if (Tools is { } reg && reg.List().Any(t => !t.Retired))
            {
                string names = string.Join(", ", reg.List()
                    .Where(t => !t.Retired).Select(t => t.Name));
                protocol +=
                    "TOOL: <name> <input>             — run a certified tool (" + names + ")\n";
            }
            systemText += protocol;
            headroom -= _countTokens(protocol);
        }

        // A resume turn (manual or auto) suppresses hidden reasoning: the anchor
        // makes continuation mechanical, and a thinking model otherwise spends
        // the whole budget deliberating and emits nothing (observed live).
        CnetHarnessGenerationResult result = GenerateOnce(
            systemText, innerUser, maxTokens,
            continuing ? ResumeSampling(sampling) : sampling, seed, role,
            think: continuing ? false : null, continueFrom: continueFrom);

        int rounds = 0;
        while (loopEnabled && rounds < _maxLookupRounds &&
               TryParseAction(result.Text, out string kind, out string query))
        {
            rounds++;
            var served = new List<long>();
            string? output = null;
            var block = new System.Text.StringBuilder();

            if (!seenQueries.Add(kind + ":" + query))
            {
                block.Append($"\n### {kind.ToUpperInvariant()} \"{query}\"\n" +
                             "(already done — answer now with what is shown)\n");
            }
            else if (kind == "calc")
            {
                // The exact lane as a callable thought-step: computed truth
                // mid-deliberation, with the same decline discipline.
                block.Append($"\n### Calculation\n");
                block.Append(Verify.ExactArithmetic.TryAnswer(query, out string answer)
                    ? $"{query} = {answer}\n"
                    : $"{query} — declined (not pure arithmetic; do not guess a value)\n");
                output = answer.Length > 0 ? answer : null;
            }
            else if (kind == "tool")
            {
                // A certified declarative tool as a thought-step. First token
                // is the tool name; the remainder is its input.
                string name = query.Split(' ', 2)[0];
                string toolInput = query.Length > name.Length
                    ? query[(name.Length + 1)..].Trim() : "";
                block.Append($"\n### Tool {name}\n");
                if (Tools is { } reg && reg.TryInvoke(name, toolInput, out string toolOut))
                {
                    block.Append($"{name}({toolInput}) = {toolOut}\n");
                    output = toolOut;
                }
                else
                {
                    block.Append($"{name} — no such certified tool, or it declined this " +
                                 "input (do not guess a value)\n");
                }
            }
            else
            {
                // recall (conversation memory) or read (document sections).
                string? roleFilter = kind == "read" ? "doc" : null;
                block.Append(kind == "read"
                    ? $"\n### Document sections \"{query}\"\n"
                    : $"\n### Lookup \"{query}\"\n");
                List<MemoryBlob> hits = _memory.Lookup(query, _resultsPerLookup,
                                                       visibleIds, roleFilter);
                if (hits.Count == 0)
                {
                    block.Append(kind == "read"
                        ? "(no document section matches — say the documents do not cover it)\n"
                        : "(no stored memory matches — if that was the missing fact, " +
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

            var round = new LookupRound(query, served, kind, output);
            lookups.Add(round);
            usedIds.AddRange(served);
            OnLookup?.Invoke(round);

            result = GenerateOnce(systemText, user, maxTokens, sampling, seed, role);
        }

        // Rounds exhausted but the model still wants to act: one forced
        // answer, so an action-happy model cannot return scaffolding as a reply.
        if (loopEnabled && TryParseAction(result.Text, out _, out _))
        {
            const string NoMore = "\n(No more actions available — answer now using only what is shown.)\n";
            if (_countTokens(NoMore) <= headroom)
            {
                systemText += NoMore;
                result = GenerateOnce(systemText, user, maxTokens, sampling, seed, role);
            }
        }

        // ── all-thinking burnout salvage ──
        // A capped answer with no visible text means the model spent the whole
        // budget on hidden reasoning (bit a live agent-to-agent test twice in a
        // row). One retry: double budget, think:false. Same prompt and seed —
        // the deterministic thinking prefix simply gets room to finish.
        uint mainBudget = maxTokens;
        uint burnedGenerated = 0; double burnedPromptMs = 0, burnedGenMs = 0;
        if (string.IsNullOrWhiteSpace(result.Text) && result.GeneratedTokens >= maxTokens)
        {
            burnedGenerated = result.GeneratedTokens;
            burnedPromptMs = result.PromptMs;
            burnedGenMs = result.GenerationMs;
            mainBudget = Math.Min(maxTokens * 2, 65536u);
            result = GenerateOnce(systemText, innerUser, mainBudget, sampling, seed, role,
                think: false, continueFrom: continueFrom);
        }

        // ── auto-continue ──
        // A budget-capped chunk is unfinished; instead of handing the seam to
        // the user, resume automatically and stitch. Unlike a manual
        // "continue" (whose previous chunk is already stored and visible in
        // recent turns), the answer-in-progress exists nowhere else — so each
        // round quotes the whole answer so far, clipped from the head when the
        // budget demands, since the cut end is what anchors the resume.
        var chunks = new List<string> { result.Text };
        uint totalGenerated = result.GeneratedTokens + burnedGenerated;
        double totalPromptMs = result.PromptMs + burnedPromptMs;
        double totalGenMs = result.GenerationMs + burnedGenMs;
        int autoRounds = 0;
        // Two unfinished signals: the token budget cut the answer, or the
        // answer ends inside an open ``` fence — a model emitting a natural
        // stop mid-code-block believes it is done but structurally is not
        // (observed live twice: JSON with 11 unclosed braces, fence never
        // closed). Both resume identically; the round cap bounds either.
        bool capped = (result.GeneratedTokens >= mainBudget || HasUnclosedFence(result.Text)) &&
                      !string.IsNullOrWhiteSpace(result.Text);

        void Tally(CnetHarnessGenerationResult r)
        {
            totalGenerated += r.GeneratedTokens;
            totalPromptMs += r.PromptMs;
            totalGenMs += r.GenerationMs;
        }

        while (capped && autoRounds < _maxAutoContinues)
        {
            string soFar = string.Concat(chunks);
            int resumeHeadroom = promptBudget - _countTokens(context.SystemText);
            uint roundBudget = maxTokens;
            CnetHarnessGenerationResult next;

            if (structural)
            {
                // The partial answer rides as a real assistant turn. The
                // quote-in-system alternative degrades as the partial grows —
                // observed live: by round ~5 the model abandoned the resume
                // and started a fresh answer mid-string.
                string? anchor = ClipForResume(soFar,
                    resumeHeadroom - _countTokens(ResumeInstruction) - 32);
                if (anchor is null) break;                // window too tight to anchor

                next = GenerateOnce(context.SystemText, ResumeInstruction, roundBudget,
                    ResumeSampling(sampling), seed, role, think: false, continueFrom: anchor);
                autoRounds++;
                Tally(next);
                systemText = context.SystemText;

                if (string.IsNullOrWhiteSpace(next.Text))
                {
                    // think:false is advisory — minimax-m3:cloud thinks
                    // regardless and can spend the whole round deliberating.
                    // Salvage once: explicit nudge + double budget so thinking
                    // AND content both fit (the changed prompt also lets a
                    // greedy same-seed retry take a different path).
                    roundBudget = Math.Min(maxTokens * 2, 65536u);
                    next = GenerateOnce(context.SystemText,
                        ResumeInstruction + NoDeliberationNudge, roundBudget,
                        ResumeSampling(sampling), seed, role, think: false, continueFrom: anchor);
                    Tally(next);
                    if (string.IsNullOrWhiteSpace(next.Text)) break;  // truly stuck
                }
            }
            else
            {
                string? block = BuildResumeBlock(soFar, resumeHeadroom - _countTokens(user));
                if (block is null) break;                 // window too tight to anchor

                next = GenerateOnce(context.SystemText + block, user, roundBudget,
                    ResumeSampling(sampling), seed, role, think: false);
                autoRounds++;
                Tally(next);
                systemText = context.SystemText + block;

                if (string.IsNullOrWhiteSpace(next.Text))
                {
                    roundBudget = Math.Min(maxTokens * 2, 65536u);
                    string retrySystem = context.SystemText + block +
                        "Do not spend tokens deliberating — output the continuation text immediately.\n";
                    next = GenerateOnce(retrySystem, user, roundBudget,
                        ResumeSampling(sampling), seed, role, think: false);
                    Tally(next);
                    systemText = retrySystem;
                    if (string.IsNullOrWhiteSpace(next.Text)) break;  // truly stuck
                }
            }

            chunks.Add(CleanResumeChunk(soFar, next.Text));
            result = next;
            capped = next.GeneratedTokens >= roundBudget ||
                     HasUnclosedFence(string.Concat(chunks));
        }

        string fullText = string.Concat(chunks);
        result = result with
        {
            Text = fullText,
            GeneratedTokens = totalGenerated,
            PromptMs = totalPromptMs,
            GenerationMs = totalGenMs,
        };

        // ── reflection: taste inspects the draft before the user sees it ──
        // Multi-layer thought, bounded: if the judge flags the assembled
        // answer as Bad (degenerate repetition, tangle, junk), regenerate
        // ONCE with the critique in context and keep whichever draft scores
        // better. One round only — a reflection loop without a cap is a
        // system stuck with its own thoughts. The judge still certifies
        // nothing: both drafts are model output; taste just picks.
        int reflections = 0;
        if (Judge is not null && !continuing &&
            !string.IsNullOrWhiteSpace(fullText))
        {
            Judgment.Judgment draftJudgment = Judge.Judge(fullText);
            if (draftJudgment.Value == Judgment.Verdict.Bad)
            {
                string critique =
                    "\n(Your previous draft was rejected by an output-quality filter: " +
                    string.Join(", ", draftJudgment.TopFeatures) +
                    ". Answer the user's message directly and cleanly — no unrelated " +
                    "content, no repetition.)\n";
                CnetHarnessGenerationResult retry = GenerateOnce(
                    context.SystemText + critique, user, mainBudget, sampling,
                    seed + 1, role);
                reflections = 1;
                totalGenerated += retry.GeneratedTokens;
                if (!string.IsNullOrWhiteSpace(retry.Text) &&
                    Judge.Judge(retry.Text).Score > draftJudgment.Score)
                {
                    fullText = retry.Text;
                    result = retry with
                    {
                        Text = fullText,
                        GeneratedTokens = totalGenerated,
                    };
                    capped = false;   // the reflected answer stands as-is
                }
            }
        }

        // Durable usage signal for consolidation: which memories actually
        // earned a slot in this prompt (gate-recalled and looked-up alike).
        _memory.RecordUsage(usedIds);

        // Only the real exchange is stored — RECALL scaffolding, resume
        // prompts, and REJECTED reflection drafts never become memory: the
        // store keeps exactly what the user saw.
        _memory.Remember("user", user);
        _memory.Remember("assistant", fullText);
        _memory.NextTurn();

        _lastAssistantText = fullText;

        // Still capped after every allowed round: arm the manual path. A
        // whitespace answer has nothing to resume.
        bool truncated = capped && !string.IsNullOrWhiteSpace(fullText);
        _pendingContinuation = truncated ? fullText : null;

        return new MemoryGenerationResult(result, usedIds, systemText, lookups,
                                          truncated, autoRounds,
                                          Reflections: reflections);
    }

    /// <summary>
    /// Removes the two seam artifacts models produce when resuming a cut
    /// (both observed live): markdown fence churn — a resume inside an open
    /// code block re-emits ``` markers before continuing — and short overlap,
    /// re-typing a few characters from the cut point. The content either side
    /// of the artifacts is untouched.
    /// </summary>
    internal static string CleanResumeChunk(string soFar, string next)
    {
        // Fence churn only makes sense inside an unclosed fence.
        if (CountFences(soFar) % 2 == 1)
        {
            for (int guard = 0; guard < 3; guard++)
            {
                string probe = next.TrimStart();
                if (!probe.StartsWith("```", StringComparison.Ordinal)) break;
                int pos = next.IndexOf("```", StringComparison.Ordinal) + 3;
                while (pos < next.Length && char.IsLetter(next[pos])) pos++;   // language tag
                if (pos < next.Length && next[pos] == '\n') pos++;
                next = next[pos..];
            }
        }
        return TrimSeamOverlap(soFar, next);
    }

    /// <summary>
    /// Sampling for resume rounds: continuation of existing text wants a much
    /// colder distribution than open-ended generation — at temperature 0.7 the
    /// model occasionally drops or re-types tokens at the seam (observed live:
    /// a missing "}," between objects), while greedy resumes were byte-exact.
    /// Focused (0.3) keeps seams faithful without greedy's repetition traps.
    /// An explicit Deterministic request is respected.
    /// </summary>
    private static CnetHarnessSamplingMode ResumeSampling(CnetHarnessSamplingMode requested) =>
        requested == CnetHarnessSamplingMode.Deterministic
            ? requested
            : CnetHarnessSamplingMode.Focused;

    /// <summary>An odd number of ``` markers means the text ends inside a code block.</summary>
    internal static bool HasUnclosedFence(string text) => CountFences(text) % 2 == 1;

    private static int CountFences(string text)
    {
        int count = 0;
        for (int i = text.IndexOf("```", StringComparison.Ordinal); i >= 0;
             i = text.IndexOf("```", i + 3, StringComparison.Ordinal))
            count++;
        return count;
    }

    /// <summary>
    /// If the new chunk starts with a suffix of the text so far, drop the
    /// duplicated prefix (observed live: an 8-char token re-typed, corrupting
    /// JSON). Six-char minimum so common short sequences ("the ") are never
    /// mistaken for a seam.
    /// </summary>
    internal static string TrimSeamOverlap(string soFar, string next)
    {
        string? trimmed = TryTrimOverlap(soFar, next);
        if (trimmed is not null) return trimmed;

        // Whitespace-tolerant pass: models re-typing from the cut often start
        // at the beginning of the line, with fresh indentation the text so far
        // already contains (observed live: '\"id\": \"' re-typed behind
        // 12 spaces of indent). The indentation is part of the artifact.
        string nextTrim = next.TrimStart();
        if (nextTrim.Length != next.Length)
        {
            trimmed = TryTrimOverlap(soFar, nextTrim);
            if (trimmed is not null) return trimmed;
        }
        return next;
    }

    private static string? TryTrimOverlap(string soFar, string next)
    {
        int max = Math.Min(120, Math.Min(soFar.Length, next.Length));
        for (int len = max; len >= 6; len--)
            if (string.CompareOrdinal(soFar, soFar.Length - len, next, 0, len) == 0)
                return next[len..];
        return null;
    }

    /// <summary>
    /// Head-clips a partial answer to fit a structural-resume token budget —
    /// the cut end is what anchors the resume, so the head is expendable.
    /// Null when even a 300-char tail cannot fit.
    /// </summary>
    private string? ClipForResume(string soFar, int budgetTokens)
    {
        foreach (int keep in new[] { soFar.Length, 4800, 2400, 1200, 600, 300 })
        {
            if (keep > soFar.Length) continue;
            string tail = keep == soFar.Length ? soFar : soFar[^keep..];
            if (_countTokens(tail) <= budgetTokens) return tail;
        }
        return null;
    }

    /// <summary>
    /// The resume prompt for an unfinished answer: quote it (head-clipped to
    /// fit <paramref name="headroom"/>) and demand exact continuation. Null
    /// when even a 300-char anchor cannot fit.
    /// </summary>
    private string? BuildResumeBlock(string soFar, int headroom)
    {
        foreach (int keep in new[] { soFar.Length, 4800, 2400, 1200, 600, 300 })
        {
            if (keep > soFar.Length) continue;
            string quoted = keep == soFar.Length ? soFar : "…" + soFar[^keep..];
            string block =
                "\n### Continuation\n" +
                "Your reply below was cut off mid-output by the token limit:\n" +
                quoted + "\n" +
                "Resume EXACTLY at that cut — output only the remaining text. " +
                "Do not repeat anything already written, do not restart, no preamble.\n";
            if (_countTokens(block) <= headroom) return block;
        }
        return null;
    }

    /// <summary>
    /// True when the message is a bare resume request. Deliberately narrow:
    /// "continue …" with any suffix still counts ("continue the json"), but a
    /// message that merely mentions continuing does not.
    /// </summary>
    internal static bool IsContinueRequest(string user)
    {
        string t = user.Trim().TrimEnd('.', '!', '?', '…').Trim().ToLowerInvariant();
        return t.StartsWith("continue", StringComparison.Ordinal) ||
               t is "go on" or "keep going" or "carry on" or "resume" or "finish"
                 or "keep writing" or "finish it" or "more please";
    }

    private CnetHarnessGenerationResult GenerateOnce(
        string systemText, string user, uint maxTokens,
        CnetHarnessSamplingMode sampling, uint seed, string role,
        bool? think = null, string? continueFrom = null)
    {
        OnInnerGenerationStart?.Invoke();
        try
        {
            return _session.Generate(new CnetHarnessGenerateOptions
            {
                System = systemText.Length > 0 ? systemText : null,
                User = user,
                Role = role,
                MaxTokens = maxTokens,
                Seed = seed,
                Sampling = sampling,
                Think = think,
                ContinueFrom = continueFrom,
            });
        }
        finally { OnInnerGenerationEnd?.Invoke(); }
    }

    /// <summary>
    /// A reply is a lookup request iff its first non-empty line starts with
    /// "RECALL:". Anything after that line is ignored — models sometimes keep
    /// talking. Recalled blob text is data, never parsed: only the model's own
    /// output reaches this, so stored "RECALL:" strings cannot steer the loop.
    /// </summary>
    internal static bool TryParseRecall(string? text, out string query)
    {
        bool ok = TryParseAction(text, out string kind, out query) && kind == "recall";
        if (!ok) query = "";
        return ok;
    }

    /// <summary>
    /// A reply is an action iff its first non-empty line starts with RECALL:,
    /// READ:, or CALC:. Anything after that line is ignored. Only the model's
    /// own output reaches this — stored text is data and cannot steer the loop.
    /// </summary>
    internal static bool TryParseAction(string? text, out string kind, out string arg)
    {
        kind = "";
        arg = "";
        if (string.IsNullOrWhiteSpace(text)) return false;
        foreach (string rawLine in text.Split('\n'))
        {
            string line = rawLine.Trim();
            if (line.Length == 0) continue;
            (string Prefix, string Kind)[] actions =
                [("RECALL:", "recall"), ("READ:", "read"), ("CALC:", "calc"),
                 ("TOOL:", "tool")];
            foreach ((string prefix, string k) in actions)
            {
                if (!line.StartsWith(prefix, StringComparison.Ordinal)) continue;
                arg = line[prefix.Length..].Trim().Trim('"', '\'', '`');
                if (arg.Length > 200) arg = arg[..200];
                if (arg.Length == 0) return false;
                kind = k;
                return true;
            }
            return false;   // first non-empty line is not an action: it is the answer
        }
        return false;
    }
}
