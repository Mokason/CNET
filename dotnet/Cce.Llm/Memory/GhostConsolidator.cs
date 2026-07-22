using System.Runtime.InteropServices;

namespace CNET.Cce.Llm.Memory;

/// <summary>One memory selected for consolidation into the gap-lane teaching cycle.</summary>
/// <param name="SkillName">Deterministic gap-lane skill name — same memory always
/// maps to the same name, so repeated consolidation coalesces in the ledger
/// (exact-signature <c>times_hit++</c>) instead of forking duplicate gaps.
/// Teachers bind in <c>times_hit</c> order, so re-consolidating a memory
/// literally raises its teaching priority — replay strength.</param>
/// <param name="Text">Verbatim blob text (the seed/context the teacher pins).</param>
/// <param name="BlobId">Provenance: the store blob this came from.</param>
/// <param name="Reason">Which extraction rule fired, for the receipt line.</param>
public sealed record TeachableItem(string SkillName, string Text, long BlobId, string Reason);

/// <summary>Outcome of emitting one item to the gap inbox.</summary>
public sealed record ConsolidationReceipt(TeachableItem Item, bool Emitted, string? Error);

/// <summary>
/// The hippocampus→cortex seam: reads the episodic ghost store, extracts
/// memories that keep proving valuable, and notes each into the gap-lane
/// inbox as a named skill. The native lane then does what it always does —
/// mines the LM teacher, trains a fresh certified specialist per gap, and
/// seals it into the base with provenance. Nothing here invents text: every
/// teachable is a verbatim blob with its id attached.
/// </summary>
/// <remarks>
/// Extraction is precision-first, like recall itself. Four rules:
///  1. explicit imperatives — the user said "remember this";
///  2. corrections — a user turn that overrules the assistant is the single
///     most valuable thing to internalize (and the thing pure recall can only
///     paper over);
///  3. cross-session re-queries — a fact asked about again in a LATER session
///     has proven it matters beyond one conversation;
///  4. usage — blobs served into prompts at least <see cref="MinUses"/> times
///     across at least <see cref="MinSessions"/> distinct sessions.
/// Forgotten blobs never appear (the store masks tombstones) — /forget is
/// anti-teaching by construction.
/// </remarks>
public sealed class GhostConsolidator
{
    private readonly BlobStore _store;

    /// <summary>Usage rule: minimum served-into-prompt count.</summary>
    public int MinUses { get; init; } = 2;

    /// <summary>Usage rule: minimum distinct sessions that used the blob.</summary>
    public int MinSessions { get; init; } = 2;

    /// <summary>Most items per run — the lane coalesces repeats, so later runs pick up the rest.</summary>
    public int MaxItems { get; init; } = 16;

    /// <summary>Native seam, injectable for tests.</summary>
    internal Func<string, string, string, int>? NoteSkillOverride { get; set; }

    public GhostConsolidator(BlobStore store) =>
        _store = store ?? throw new ArgumentNullException(nameof(store));

    private static readonly string[] ImperativeMarkers =
        ["remember this", "remember:", "remember that", "don't forget", "do not forget"];

    private static readonly string[] CorrectionOpeners =
        ["wrong", "no,", "no.", "nope", "incorrect", "that's wrong", "thats wrong",
         "that's not", "thats not", "actually,", "i said", "i meant", "not true"];

    /// <summary>Extracts teachable items. Pure read — emits nothing.</summary>
    public List<TeachableItem> Extract()
    {
        IReadOnlyList<MemoryBlob> all = _store.All();
        var items = new List<TeachableItem>();
        var taken = new HashSet<long>();

        void Take(MemoryBlob blob, string reason)
        {
            if (items.Count >= MaxItems || !taken.Add(blob.Id)) return;
            items.Add(new TeachableItem(SkillNameFor(blob), blob.Text, blob.Id, reason));
        }

        // 1. Explicit imperatives — the user marked it themselves.
        foreach (MemoryBlob b in all)
        {
            if (b.Role != "user") continue;
            string lower = b.Text.ToLowerInvariant();
            if (ImperativeMarkers.Any(lower.Contains))
                Take(b, "explicit remember request");
        }

        // 2. Corrections — a user turn that opens by overruling the assistant,
        //    where an assistant turn immediately precedes it. Openers only:
        //    precision over recall, same stance as the memory gate.
        for (int i = 0; i < all.Count; i++)
        {
            MemoryBlob b = all[i];
            if (b.Role != "user") continue;
            string head = b.Text.TrimStart().ToLowerInvariant();
            if (!CorrectionOpeners.Any(head.StartsWith)) continue;
            bool followsAssistant = i > 0 && all[i - 1].Role == "assistant" &&
                                    all[i - 1].SessionId == b.SessionId;
            if (followsAssistant)
                Take(b, "user correction");
        }

        // 3. Cross-session re-query — a user question in a later session that
        //    shares 3+ distinctive terms with an earlier-session blob promotes
        //    that earlier blob: it mattered beyond its own conversation.
        var bySession = all.GroupBy(b => b.SessionId)
                           .OrderBy(g => g.Min(b => b.Id))
                           .ToList();
        for (int later = 1; later < bySession.Count; later++)
        {
            foreach (MemoryBlob q in bySession[later].Where(b => b.Role == "user"))
            {
                var qTerms = Terms(q.Text);
                if (qTerms.Count < 3) continue;
                for (int earlier = 0; earlier < later; earlier++)
                {
                    foreach (MemoryBlob f in bySession[earlier])
                    {
                        if (Terms(f.Text).Intersect(qTerms).Count() >= 3)
                            Take(f, $"re-queried in a later session (by #{q.Id})");
                    }
                }
            }
        }

        // 4. Usage — served into real prompts repeatedly, across sessions.
        foreach (MemoryBlob b in all)
        {
            if (_store.UsageCount(b.Id) >= MinUses && _store.UsageSessions(b.Id) >= MinSessions)
                Take(b, $"used in {_store.UsageCount(b.Id)} prompts across " +
                        $"{_store.UsageSessions(b.Id)} sessions");
        }

        return items;
    }

    /// <summary>
    /// Notes each item into the gap-lane inbox via the native
    /// <c>cnet_auto_learn_note_skill</c> — the same canonical tagging every
    /// other producer uses; no managed reimplementation to drift.
    /// </summary>
    public List<ConsolidationReceipt> Emit(IEnumerable<TeachableItem> items, string inboxPath)
    {
        ArgumentException.ThrowIfNullOrEmpty(inboxPath);
        var receipts = new List<ConsolidationReceipt>();
        foreach (TeachableItem item in items)
        {
            try
            {
                int rc = NoteSkillOverride is not null
                    ? NoteSkillOverride(inboxPath, item.SkillName, item.Text)
                    : CnetAutoLearnNative.NoteSkill(inboxPath, item.SkillName, item.Text);
                receipts.Add(new ConsolidationReceipt(item, rc == 0,
                    rc == 0 ? null : $"native note_skill returned {rc}"));
            }
            catch (Exception ex) when (ex is DllNotFoundException or EntryPointNotFoundException)
            {
                receipts.Add(new ConsolidationReceipt(item, false,
                    "cnet.so not found or too old — build with `make cnet_dll` " +
                    $"({ex.GetType().Name})"));
            }
        }
        return receipts;
    }

    /// <summary>
    /// Deterministic skill name: gh_&lt;8-hex FNV-1a of the text&gt;_&lt;first
    /// distinctive term&gt;. Stable across runs and stores, short enough that
    /// the native goal tag ("skill_" + this) stays within PORT_TAG_MAX (32).
    /// </summary>
    internal static string SkillNameFor(MemoryBlob blob)
    {
        ulong h = 14695981039346656037UL;
        foreach (byte c in System.Text.Encoding.UTF8.GetBytes(blob.Text))
            h = (h ^ c) * 1099511628211UL;
        string term = Terms(blob.Text).FirstOrDefault() ?? "note";
        if (term.Length > 12) term = term[..12];
        return $"gh_{(uint)h:x8}_{term}";
    }

    private static List<string> Terms(string text) =>
        BlobAnalyzer.Tokenize(text).Where(t => t.Length >= 4).Distinct().ToList();
}

/// <summary>
/// Binding to the auto-learn note functions exported by the unified cnet.so
/// (built by <c>make cnet_dll</c>). Resolution order: CNET_LIBRARY env
/// (absolute path, authoritative), then cnet.so beside the app, the current
/// directory, and each parent — the repo-root convention every CNET host uses.
/// </summary>
internal static partial class CnetAutoLearnNative
{
    private const string LibraryName = "cnet";
    private static int s_registered;

    [LibraryImport(LibraryName, EntryPoint = "cnet_auto_learn_note_skill",
        StringMarshalling = StringMarshalling.Utf8)]
    private static partial int NoteSkillImport(string inboxPath, string skillName,
                                               string text, nuint kOverride);

    /// <summary>Notes a named teachable skill into the inbox. 0 on success.</summary>
    public static int NoteSkill(string inboxPath, string skillName, string text)
    {
        EnsureResolver();
        return NoteSkillImport(inboxPath, skillName, text, 0);   // 0 = default top-k
    }

    private static void EnsureResolver()
    {
        if (Interlocked.CompareExchange(ref s_registered, 1, 0) != 0) return;
        NativeLibrary.SetDllImportResolver(typeof(CnetAutoLearnNative).Assembly,
            (name, _, _) =>
            {
                if (name != LibraryName) return IntPtr.Zero;

                string? env = Environment.GetEnvironmentVariable("CNET_LIBRARY");
                if (!string.IsNullOrEmpty(env))
                    return NativeLibrary.Load(env);   // authoritative; throws if bad

                foreach (string dir in Candidates())
                {
                    string candidate = System.IO.Path.Combine(dir, "cnet.so");
                    if (File.Exists(candidate) && NativeLibrary.TryLoad(candidate, out IntPtr h))
                        return h;
                }
                return IntPtr.Zero;   // fall through to default probing
            });

        static IEnumerable<string> Candidates()
        {
            yield return AppContext.BaseDirectory;
            string? dir = Directory.GetCurrentDirectory();
            for (int i = 0; i < 6 && dir is not null; i++)
            {
                yield return dir;
                dir = System.IO.Path.GetDirectoryName(dir);
            }
        }
    }
}
