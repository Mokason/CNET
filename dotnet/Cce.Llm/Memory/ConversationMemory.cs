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
    private readonly string _sessionId;
    private int _turn;

    /// <summary>Session id grouping this run's blobs in the store.</summary>
    public string SessionId => _sessionId;

    /// <summary>Current turn number (starts at 0, advances per exchange).</summary>
    public int Turn => _turn;

    /// <param name="store">Persistent blob store; owned by the caller.</param>
    /// <param name="countTokens">
    /// Token counter of the live model (e.g. <c>CnetLlmInferenceSession.CountTokens</c>).
    /// Budgets are enforced with real token counts, not character heuristics.
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
        const string memoryHeader = "\n\n### Memory — verbatim excerpts from past sessions." +
            " Only rely on them for facts; if memory does not cover the answer, say so rather than invent one.\n";
        int remaining = promptBudgetTokens - fixedTokens - _countTokens(memoryHeader);

        // Over-fetch, then pack by score under the real token budget.
        var candidates = _store.Recall(question, _options.TopK * 2);
        var selected = new List<(MemoryBlob Blob, string Rendered)>();
        foreach (MemoryBlob blob in candidates)
        {
            if (selected.Count == _options.TopK) break;
            if (IsWithinRecentWindow(blob)) continue;   // already present verbatim

            string rendered = RenderBlob(blob);
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
                sb.Append(rendered).Append('\n');
            memoryTokens = promptBudgetTokens - fixedTokens - remaining;
        }
        else
        {
            memoryTokens = 0;
        }
        if (recentBlock.Length > 0)
            sb.Append("\n### Recent turns\n").Append(recentBlock);

        return new MemoryContext(
            sb.ToString(),
            selected.ConvertAll(s => s.Blob.Id),
            memoryTokens);
    }

    private bool IsWithinRecentWindow(MemoryBlob blob)
        => blob.SessionId == _sessionId && blob.Turn > _turn - _options.RecentTurns;

    private static string RenderBlob(MemoryBlob blob)
    {
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

    /// <summary>Splits at blank lines first, then hard-wraps any oversized paragraph.</summary>
    private IEnumerable<string> Split(string text)
    {
        if (_countTokens(text) <= _options.MaxBlobTokens)
        {
            yield return text;
            yield break;
        }

        var current = new StringBuilder();
        foreach (string paragraph in text.Split("\n\n", StringSplitOptions.RemoveEmptyEntries))
        {
            string candidate = current.Length == 0
                ? paragraph
                : current + "\n\n" + paragraph;

            if (_countTokens(candidate) <= _options.MaxBlobTokens)
            {
                current.Clear();
                current.Append(candidate);
                continue;
            }

            if (current.Length > 0)
            {
                yield return current.ToString();
                current.Clear();
            }

            // Paragraph alone exceeds the cap: hard-split by sentences.
            foreach (string piece in SplitOversized(paragraph))
                yield return piece;
        }

        if (current.Length > 0)
            yield return current.ToString();
    }

    private IEnumerable<string> SplitOversized(string paragraph)
    {
        var current = new StringBuilder();
        int from = 0;
        while (from < paragraph.Length)
        {
            int end = paragraph.IndexOfAny(['.', '!', '?'], from);
            string sentence = end < 0
                ? paragraph[from..]
                : paragraph[from..(end + 1)];
            from = end < 0 ? paragraph.Length : end + 1;

            string candidate = current.Length == 0 ? sentence : current + sentence;
            if (_countTokens(candidate) <= _options.MaxBlobTokens || current.Length == 0)
            {
                current.Clear();
                current.Append(candidate);
                continue;
            }

            yield return current.ToString().Trim();
            current.Clear();
            current.Append(sentence);
        }

        string tail = current.ToString().Trim();
        if (tail.Length > 0)
            yield return tail;
    }
}
