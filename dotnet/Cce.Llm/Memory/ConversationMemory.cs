using System.Text;

namespace CNET.Cce.Llm.Memory;

/// <summary>What <see cref="ConversationMemory.BuildContext"/> assembled, with provenance.</summary>
/// <param name="SystemText">System prompt with the memory and recent-turns blocks appended.</param>
/// <param name="UsedBlobIds">Ids of recalled blobs, in the order they appear in the prompt.</param>
/// <param name="MemoryTokens">Tokens the recalled block consumed, by the live tokenizer.</param>
public sealed record MemoryContext(string SystemText, IReadOnlyList<long> UsedBlobIds, int MemoryTokens);

/// <summary>
/// The conversation-facing memory layer: stores every exchange as verbatim blobs
/// in a <see cref="BlobStore"/>, and rebuilds a budgeted prompt context each turn
/// from keyword recall plus the most recent turns.
/// </summary>
/// <remarks>
/// Anti-hallucination stance, mechanically enforced rather than hoped for:
/// recalled text is quoted verbatim (never summarized), tagged with provenance
/// (<c>[#id | date | role]</c>) so a host can trace any "memory" back to a store
/// line, and injected only when the query shares a discriminative keyword with
/// the blob — otherwise the memory block is simply absent. The model cannot be
/// fed a memory that does not exist in the store file.
/// <para>
/// Prompt-ordering is deliberate: stable system text first, then recalled blobs
/// in chronological order, then recent turns, then the question. Both backends
/// reuse KV for the longest unchanged token prefix (managed
/// <c>PrefixCache</c>, native <c>cached_prompt_tokens</c>), so keeping the
/// most stable content leftmost converts directly into skipped prefill.
/// </para>
/// </remarks>
public sealed class ConversationMemory
{
    private readonly BlobStore _store;
    private readonly Func<string, int> _countTokens;
    private readonly MemoryOptions _options;
    private readonly List<(string Role, string Text)> _recent = [];

    /// <summary>
    /// Question terms that signal an ORDERING query about the conversation.
    /// Keyword recall cannot serve those — "what was the first thing I said"
    /// shares no keyword with the blob "hey" — so their presence additionally
    /// pulls the session's earliest blobs as verbatim anchors. A false positive
    /// ("the first emperor of Rome") costs a little budget, is fully visible in
    /// the receipts, and can never inject invented text.
    /// </summary>
    private static readonly HashSet<string> TemporalTerms = new(StringComparer.Ordinal)
    {
        "first", "start", "started", "starting", "begin", "began", "beginning",
        "earliest", "initially", "originally", "opening",
        // Relative-order questions ("what did I ask after that?") need the
        // session timeline just as much as absolute ones.
        "after", "before", "next", "previous", "previously", "prior",
    };
    private readonly string _sessionId;
    private int _turn;

    /// <summary>Session id grouping this run's blobs in the store.</summary>
    public string SessionId => _sessionId;

    /// <summary>Current turn number (starts at 0, advances per exchange).</summary>
    public int Turn => _turn;

    /// <param name="store">Persistent blob store; owned by the caller.</param>
    /// <param name="countTokens">
    /// Token counter of the live model — <c>CnetLlmInferenceSession.CountTokens</c>
    /// (managed) or <c>CnetHarnessSession.CountTokens</c> (native, via the
    /// <c>cnet_harness_count_tokens</c> ABI export). Budgets are enforced with
    /// real token counts, not character heuristics.
    /// </param>
    /// <param name="options">Tunables; defaults favour precision.</param>
    public ConversationMemory(BlobStore store, Func<string, int> countTokens, MemoryOptions? options = null)
    {
        _store = store ?? throw new ArgumentNullException(nameof(store));
        _countTokens = countTokens ?? throw new ArgumentNullException(nameof(countTokens));
        _options = options ?? new MemoryOptions();
        _sessionId = Guid.NewGuid().ToString("N")[..12];
    }

    /// <summary>
    /// Stores one message as blobs (splitting at paragraph boundaries when longer
    /// than <see cref="MemoryOptions.MaxBlobTokens"/>) and tracks it as a recent turn.
    /// </summary>
    public void Remember(string role, string text)
    {
        ArgumentException.ThrowIfNullOrEmpty(role);
        if (string.IsNullOrWhiteSpace(text)) return;

        foreach (string part in Split(text.Trim()))
            _store.Append(_sessionId, _turn, role, part, _countTokens(part));

        _recent.Add((role, text.Trim()));
        int maxRecent = _options.RecentTurns * 2;   // user + assistant per turn
        if (_recent.Count > maxRecent)
            _recent.RemoveRange(0, _recent.Count - maxRecent);
    }

    /// <summary>Advances the turn counter after a completed user/assistant exchange.</summary>
    public void NextTurn() => _turn++;

    /// <summary>
    /// Builds the system context for one generation: base system text, a memory
    /// block recalled for <paramref name="question"/>, and the recent turns —
    /// guaranteed to fit <paramref name="promptBudgetTokens"/> together with the
    /// question itself.
    /// </summary>
    /// <exception cref="InvalidOperationException">
    /// The non-negotiable parts (system text, recent turns, question) alone
    /// exceed the budget — recall cannot fix that; the caller must raise the
    /// window or shorten the inputs.
    /// </exception>
    public MemoryContext BuildContext(string? baseSystem, string question, int promptBudgetTokens)
    {
        ArgumentException.ThrowIfNullOrEmpty(question);
        ArgumentOutOfRangeException.ThrowIfLessThan(promptBudgetTokens, 1);

        string recentBlock = RenderRecent();
        if (recentBlock.Length > 0)
            recentBlock = $"\n### Recent turns (last {_options.RecentTurns} only — " +
                "earlier turns are recallable from memory, do not guess beyond them)\n"
                + recentBlock;   // header counts too
        int fixedTokens = _countTokens(baseSystem ?? string.Empty)
                        + _countTokens(recentBlock)
                        + _countTokens(question)
                        + _options.SafetyMarginTokens;

        if (fixedTokens > promptBudgetTokens)
            throw new InvalidOperationException(
                $"system + recent turns + question need {fixedTokens} tokens " +
                $"but the prompt budget is {promptBudgetTokens}; raise the context " +
                "window, shorten the system prompt, or reduce RecentTurns");

        // The memory block's own header text competes for the same budget as
        // the blobs it introduces — count it before packing anything.
        const string memoryHeader = "\n\n### Memory — verbatim excerpts from stored history." +
            " Entries marked 'this session' are earlier turns of THIS conversation;" +
            " dated entries are from past sessions. Only rely on them for facts;" +
            " if memory does not cover the answer, say so rather than invent one.\n";
        int remaining = promptBudgetTokens - fixedTokens - _countTokens(memoryHeader);

        // Over-fetch, then pack by score under the real token budget.
        // Temporal anchors first: an ordering question gets the session's
        // earliest blobs regardless of keyword overlap. They pack with priority
        // because they ARE the answer to such a question.
        var candidates = new List<MemoryBlob>();
        foreach (string term in BlobAnalyzer.Tokenize(question))
        {
            if (!TemporalTerms.Contains(term)) continue;
            candidates.AddRange(_store.SessionStart(_sessionId, _options.TemporalAnchorBlobs));
            break;
        }

        // Over-fetch beyond TopK: the recent-window exclusion below happens
        // after this cap, and recency-boosted recent blobs tend to rank first.
        foreach (MemoryBlob hit in _store.Recall(question, _options.TopK * 2 + _options.RecentTurns * 8))
            if (!candidates.Exists(c => c.Id == hit.Id))
                candidates.Add(hit);
        var selected = new List<(MemoryBlob Blob, string Rendered)>();
        foreach (MemoryBlob blob in candidates)
        {
            if (selected.Count == _options.TopK) break;
            if (IsWithinRecentWindow(blob)) continue;   // already present verbatim

            string rendered = RenderBlob(blob) + "\n";   // costed exactly as appended
            int cost = _countTokens(rendered);
            if (cost > remaining) continue;             // try the next, cheaper hit

            selected.Add((blob, rendered));
            remaining -= cost;
        }

        // Chronological in the prompt: stable ordering reads as a timeline and
        // maximizes shared token prefixes between consecutive turns.
        selected.Sort((a, b) => a.Blob.Id.CompareTo(b.Blob.Id));

        var sb = new StringBuilder(baseSystem ?? string.Empty);
        int memoryTokens = 0;
        if (selected.Count > 0)
        {
            sb.Append(memoryHeader);
            foreach ((_, string rendered) in selected)
                sb.Append(rendered);
            memoryTokens = promptBudgetTokens - fixedTokens - remaining;
        }
        else
        {
            memoryTokens = 0;
        }
        if (recentBlock.Length > 0)
            sb.Append(recentBlock);

        return new MemoryContext(
            sb.ToString(),
            selected.ConvertAll(s => s.Blob.Id),
            memoryTokens);
    }

    private bool IsWithinRecentWindow(MemoryBlob blob)
        // The recent list holds turns [_turn - RecentTurns, _turn - 1]; the
        // boundary turn must be INCLUDED in the exclusion or its blobs appear
        // twice (memory block + recent block).
        => blob.SessionId == _sessionId && blob.Turn >= _turn - _options.RecentTurns;

    /// <summary>Durably records that these blobs were served into a prompt
    /// this session — the consolidation pass's usage signal.</summary>
    public void RecordUsage(IReadOnlyCollection<long> ids)
    {
        if (ids.Count > 0) _store.RecordUsage(ids, _sessionId);
    }

    /// <summary>
    /// Renders one blob exactly as the prompt does — provenance tag included.
    /// Public so the model-directed recall loop injects lookups in the same
    /// format the model already understands.
    /// </summary>
    public string RenderMemory(MemoryBlob blob) => RenderBlob(blob);

    /// <summary>
    /// A model-requested lookup: same store, same precision gate, minus blobs
    /// already visible (recent window or <paramref name="exclude"/>).
    /// </summary>
    public List<MemoryBlob> Lookup(string query, int maxResults, IReadOnlySet<long> exclude,
                                   string? role = null)
    {
        ArgumentException.ThrowIfNullOrEmpty(query);
        var hits = new List<MemoryBlob>();
        // Role-filtered lookups (READ: wants documents only) must over-fetch:
        // the store ranks across all roles.
        int fetch = role is null ? maxResults + exclude.Count
                                 : (maxResults + exclude.Count) * 4;
        foreach (MemoryBlob blob in _store.Recall(query, fetch))
        {
            if (hits.Count == maxResults) break;
            if (role is not null && blob.Role != role) continue;
            if (exclude.Contains(blob.Id) || IsWithinRecentWindow(blob)) continue;
            hits.Add(blob);
        }
        return hits;
    }

    /// <summary>
    /// Ingests a document into the store: chunked by the same splitter as
    /// conversation, each chunk headed with its source and position, stored
    /// under a document session id (never the live session — documents are
    /// material, not turns, so they can never flood the recent window or the
    /// temporal anchors). Recall and the READ: action find them by keyword;
    /// quotes carry [#id] receipts like any memory.
    /// </summary>
    /// <returns>The appended blob ids, in document order.</returns>
    public List<long> IngestDocument(string sourceName, string content)
    {
        ArgumentException.ThrowIfNullOrEmpty(sourceName);
        ArgumentException.ThrowIfNullOrEmpty(content);

        var parts = Split(content.Trim()).ToList();
        string docSession = "doc:" + sourceName;
        var ids = new List<long>(parts.Count);
        for (int i = 0; i < parts.Count; i++)
        {
            string text = $"[doc:{sourceName} §{i + 1}/{parts.Count}]\n{parts[i]}";
            ids.Add(_store.Append(docSession, i, "doc", text, _countTokens(text)).Id);
        }
        return ids;
    }

    private string RenderBlob(MemoryBlob blob)
    {
        // Current-session blobs must say so, with their turn number: without
        // the marker a model reading "excerpts from past sessions" classified
        // this session's own opening turn as history and answered ordering
        // questions from its 2-turn window instead.
        if (blob.SessionId == _sessionId)
            return $"[#{blob.Id} | this session, turn {blob.Turn} | {blob.Role}] {blob.Text}";

        // Hand-edited or foreign store lines may carry short timestamps; render
        // what exists rather than throwing at prompt-build time.
        string date = blob.TimestampUtc.Length >= 10 ? blob.TimestampUtc[..10] : blob.TimestampUtc;
        return $"[#{blob.Id} | {date} | {blob.Role}] {blob.Text}";
    }

    private string RenderRecent()
    {
        if (_recent.Count == 0) return string.Empty;
        var sb = new StringBuilder();
        foreach ((string role, string text) in _recent)
            sb.Append(role).Append(": ").Append(text).Append('\n');
        return sb.ToString();
    }

    /// <summary>
    /// Splits into parts that are exact substrings of the input — concatenating
    /// the parts reproduces the message byte-for-byte, so blobs stay verbatim
    /// even when split. Paragraph boundaries first, then sentence enders, then
    /// word boundaries for punctuation-free runs (logs, code), then a hard cut
    /// for a single pathological token.
    /// </summary>
    private IEnumerable<string> Split(string text)
    {
        if (_countTokens(text) <= _options.MaxBlobTokens)
        {
            yield return text;
            yield break;
        }

        foreach (string part in MergeToBudget(SliceAt(text, FindBoundaries(text, "\n\n"))))
        {
            if (_countTokens(part) <= _options.MaxBlobTokens)
            {
                yield return part;
                continue;
            }
            foreach (string sub in MergeToBudget(SliceAt(part, FindSentenceBoundaries(part))))
            {
                if (_countTokens(sub) <= _options.MaxBlobTokens)
                {
                    yield return sub;
                    continue;
                }
                // Punctuation-free run: fall back to word boundaries, then to a
                // hard character cut for a single oversized token.
                foreach (string w in MergeToBudget(SliceAt(sub, FindBoundaries(sub, " "))))
                {
                    if (_countTokens(w) <= _options.MaxBlobTokens)
                    {
                        yield return w;
                        continue;
                    }
                    int hard = Math.Max(16, _options.MaxBlobTokens * 3);
                    for (int i = 0; i < w.Length; i += hard)
                        yield return w[i..Math.Min(w.Length, i + hard)];
                }
            }
        }
    }

    /// <summary>End indices of slices cut after each occurrence of a separator run.</summary>
    private static List<int> FindBoundaries(string text, string separator)
    {
        var cuts = new List<int>();
        int idx = 0;
        while ((idx = text.IndexOf(separator, idx, StringComparison.Ordinal)) >= 0)
        {
            int end = idx + separator.Length;
            // Extend across a run of the separator's characters so a cut never
            // lands inside "\n\n\n" and splits it between two parts oddly.
            while (end < text.Length && separator.Contains(text[end])) end++;
            cuts.Add(end);
            idx = end;
        }
        return cuts;
    }

    /// <summary>End indices of slices cut after sentence-ending punctuation.</summary>
    private static List<int> FindSentenceBoundaries(string text)
    {
        var cuts = new List<int>();
        for (int i = 0; i < text.Length; i++)
            if (text[i] is '.' or '!' or '?')
                cuts.Add(i + 1);
        return cuts;
    }

    /// <summary>Exact slices between consecutive cut points; concatenation == input.</summary>
    private static IEnumerable<string> SliceAt(string text, List<int> cuts)
    {
        int from = 0;
        foreach (int cut in cuts)
        {
            if (cut <= from || cut >= text.Length) continue;
            yield return text[from..cut];
            from = cut;
        }
        if (from < text.Length)
            yield return text[from..];
    }

    /// <summary>Greedily merges consecutive slices while they fit the blob cap.</summary>
    private IEnumerable<string> MergeToBudget(IEnumerable<string> slices)
    {
        var current = new StringBuilder();
        foreach (string slice in slices)
        {
            if (current.Length > 0 && _countTokens(current.ToString() + slice) <= _options.MaxBlobTokens)
            {
                current.Append(slice);
                continue;
            }
            if (current.Length > 0)
                yield return current.ToString();
            current.Clear();
            current.Append(slice);
        }
        if (current.Length > 0)
            yield return current.ToString();
    }
}
