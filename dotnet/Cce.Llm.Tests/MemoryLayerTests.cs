using CNET.Cce.CnetHarness;
using CNET.Cce.Llm.Memory;
using Xunit;

namespace CNET.Cce.Llm.Tests;

/// <summary>
/// Unit tests for the ghost-memory layer — no model required. The properties
/// pinned here are the ones the layer's honesty depends on: persistence across
/// reopens, verbatim recall, precision (irrelevant queries recall nothing), and
/// budgets that are never exceeded.
/// </summary>
public sealed class MemoryLayerTests : IDisposable
{
    private readonly string _dir;

    public MemoryLayerTests()
    {
        _dir = Path.Combine(Path.GetTempPath(), "cnet-ghost-tests", Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(_dir);
    }

    public void Dispose()
    {
        try { Directory.Delete(_dir, recursive: true); } catch (IOException) { }
    }

    private string StorePath(string name = "ghost.jsonl") => Path.Combine(_dir, name);

    /// <summary>Crude but deterministic counter for tests that need no real tokenizer.</summary>
    private static int FakeCount(string text)
        => text.Length == 0 ? 0 : text.Split(' ', StringSplitOptions.RemoveEmptyEntries).Length;

    // ───────────────────────── store persistence ─────────────────────────

    [Fact]
    public void Blobs_SurviveReopen_WithMonotonicIds()
    {
        long id1, id2;
        using (var store = BlobStore.Open(StorePath()))
        {
            id1 = store.Append("s1", 0, "user", "the vault code is 7291", 6).Id;
        }
        using (var store = BlobStore.Open(StorePath()))
        {
            Assert.Equal(1, store.Count);
            Assert.Equal("the vault code is 7291", store.Get(id1)!.Text);
            id2 = store.Append("s2", 0, "user", "a second session speaks", 4).Id;
        }
        Assert.True(id2 > id1, "ids must stay monotonic across sessions");

        using var reopened = BlobStore.Open(StorePath());
        Assert.Equal(2, reopened.Count);
    }

    [Fact]
    public void CorruptTrailingLine_IsSkippedNotFatal()
    {
        using (var store = BlobStore.Open(StorePath()))
            store.Append("s1", 0, "user", "intact blob", 2);

        // Simulate a crash mid-write: garbage half-line at the end.
        File.AppendAllText(StorePath(), "{\"id\":99,\"sid\":\"s1\",\"tru");

        long newId;
        using (var reopened = BlobStore.Open(StorePath()))
        {
            Assert.Equal(1, reopened.Count);
            Assert.Equal(1, reopened.CorruptLinesSkipped);

            // And the store keeps working after the bad line.
            newId = reopened.Append("s1", 1, "user", "written after corruption", 3).Id;
            Assert.True(newId > 1);
        }

        // The decisive check: the blob written AFTER the corruption must survive
        // the NEXT reload. Without tail-healing, it concatenates onto the
        // truncated garbage line and silently vanishes here.
        using var again = BlobStore.Open(StorePath());
        Assert.Equal(2, again.Count);
        Assert.Equal("written after corruption", again.Get(newId)!.Text);
    }

    [Fact]
    public void SecondWriter_IsRejectedNotInterleaved()
    {
        using var first = BlobStore.Open(StorePath());
        Assert.Throws<IOException>(() => BlobStore.Open(StorePath()));
    }

    [Fact]
    public void NewlinesInText_RoundTripExactly()
    {
        string text = "line one\n\nline two with \"quotes\" and \\backslash";
        long id;
        using (var store = BlobStore.Open(StorePath()))
            id = store.Append("s1", 0, "assistant", text, 10).Id;
        using var reopened = BlobStore.Open(StorePath());
        Assert.Equal(text, reopened.Get(id)!.Text);   // verbatim or nothing
    }

    // ───────────────────────── recall precision ─────────────────────────

    private BlobStore SeededStore()
    {
        var store = BlobStore.Open(StorePath());
        store.Append("s1", 0, "user", "the vault code is 7291, write it down", 8);
        store.Append("s1", 1, "assistant", "GEMM kernels dequantize Q8_0 tiles to f32 above sixteen tokens", 11);
        store.Append("s1", 2, "user", "my dog is called Biscuit and likes the park", 9);
        store.Append("s2", 0, "assistant", "decode is bandwidth bound at forty gigabytes per second", 9);
        return store;
    }

    [Fact]
    public void Keyword_RecallsTheRightBlob_First()
    {
        using var store = SeededStore();
        var hits = store.Recall("what was the vault code?", 4);
        Assert.NotEmpty(hits);
        Assert.Contains("7291", hits[0].Text);
    }

    [Fact]
    public void NumericKeyword_Recalls()
    {
        using var store = SeededStore();
        var hits = store.Recall("remind me about 7291", 4);
        Assert.NotEmpty(hits);
        Assert.Contains("7291", hits[0].Text);
    }

    [Fact]
    public void IdentifierWithUnderscore_IsOneTerm()
    {
        using var store = SeededStore();
        var hits = store.Recall("how do Q8_0 tiles work", 4);
        Assert.NotEmpty(hits);
        Assert.Contains("Q8_0", hits[0].Text);
    }

    /// <summary>The anti-hallucination gate: no shared keyword, no recall.</summary>
    [Fact]
    public void IrrelevantQuery_RecallsNothing()
    {
        using var store = SeededStore();
        Assert.Empty(store.Recall("banana smoothie recipes", 4));
    }

    [Fact]
    public void StopwordOnlyQuery_RecallsNothing()
    {
        using var store = SeededStore();
        Assert.Empty(store.Recall("what is the and of it", 4));
    }

    [Fact]
    public void EmptyStore_RecallsNothing()
    {
        using var store = BlobStore.Open(StorePath());
        Assert.Empty(store.Recall("anything at all", 4));
    }

    [Fact]
    public void Recall_IsCaseInsensitive()
    {
        using var store = SeededStore();
        var hits = store.Recall("BISCUIT", 4);
        Assert.NotEmpty(hits);
        Assert.Contains("Biscuit", hits[0].Text);
    }

    [Fact]
    public void Recall_IsDeterministic()
    {
        using var store = SeededStore();
        var a = store.Recall("vault code", 4).Select(b => b.Id);
        var b2 = store.Recall("vault code", 4).Select(b => b.Id);
        Assert.Equal(a, b2);
    }

    [Fact]
    public void RecalledText_IsVerbatim()
    {
        using var store = SeededStore();
        var hit = store.Recall("vault code", 1).Single();
        Assert.Equal("the vault code is 7291, write it down", hit.Text);
    }

    // ───────────────────────── context building ─────────────────────────

    private static ConversationMemory NewMemory(BlobStore store, MemoryOptions? opts = null)
        => new(store, FakeCount, opts ?? new MemoryOptions { SafetyMarginTokens = 4 });

    [Fact]
    public void BuildContext_InjectsRecalledBlob_WithProvenanceTag()
    {
        using var store = SeededStore();
        var memory = NewMemory(store);

        var ctx = memory.BuildContext("You are terse.", "what was the vault code?", 200);

        Assert.Single(ctx.UsedBlobIds);
        Assert.Contains("[#", ctx.SystemText);
        Assert.Contains("7291", ctx.SystemText);
        Assert.Contains("verbatim excerpts", ctx.SystemText);
    }

    [Fact]
    public void BuildContext_IrrelevantQuestion_HasNoMemoryBlock()
    {
        using var store = SeededStore();
        var memory = NewMemory(store);

        var ctx = memory.BuildContext("You are terse.", "compose a haiku about rain", 200);

        Assert.Empty(ctx.UsedBlobIds);
        Assert.DoesNotContain("### Memory", ctx.SystemText);
    }

    [Fact]
    public void BuildContext_NeverExceedsBudget()
    {
        using var store = BlobStore.Open(StorePath());
        // Many recallable blobs, all matching the query keyword.
        for (int i = 0; i < 40; i++)
            store.Append("s1", i, "user",
                $"telemetry shard {i} recorded anomaly cluster epsilon-{i} in the vault sector", 12);

        var memory = NewMemory(store);
        int budget = 120;
        var ctx = memory.BuildContext("sys", "report on the vault telemetry anomaly", budget);

        int total = FakeCount(ctx.SystemText) + FakeCount("report on the vault telemetry anomaly");
        Assert.True(total <= budget,
            $"context used {total} of {budget} budget");
        Assert.True(ctx.UsedBlobIds.Count > 0, "some memory should have fit");
    }

    [Fact]
    public void BuildContext_FixedPartsExceedBudget_ThrowsClearly()
    {
        using var store = SeededStore();
        var memory = NewMemory(store);
        var ex = Assert.Throws<InvalidOperationException>(
            () => memory.BuildContext(string.Join(' ', Enumerable.Repeat("word", 300)), "q", 50));
        Assert.Contains("budget", ex.Message);
    }

    [Fact]
    public void BuildContext_RecalledBlobs_AppearChronologically()
    {
        using var store = BlobStore.Open(StorePath());
        store.Append("s1", 0, "user", "first fact about the ranger protocol alpha", 7);
        store.Append("s1", 5, "user", "second fact about the ranger protocol beta", 7);
        var memory = NewMemory(store);

        var ctx = memory.BuildContext(null, "ranger protocol", 200);

        Assert.Equal(2, ctx.UsedBlobIds.Count);
        Assert.True(ctx.UsedBlobIds[0] < ctx.UsedBlobIds[1]);
        Assert.True(ctx.SystemText.IndexOf("alpha", StringComparison.Ordinal)
                  < ctx.SystemText.IndexOf("beta", StringComparison.Ordinal));
    }

    [Fact]
    public void Remember_SplitsOversizedMessages_AtParagraphs()
    {
        using var store = BlobStore.Open(StorePath());
        var memory = new ConversationMemory(store, FakeCount,
            new MemoryOptions { MaxBlobTokens = 10, SafetyMarginTokens = 4 });

        string p1 = string.Join(' ', Enumerable.Repeat("alpha", 8));
        string p2 = string.Join(' ', Enumerable.Repeat("bravo", 8));
        memory.Remember("assistant", p1 + "\n\n" + p2);

        Assert.Equal(2, store.Count);
        var hits = store.Recall("bravo", 2);
        Assert.Single(hits);
        Assert.DoesNotContain("alpha", hits[0].Text);   // split kept blobs coherent
    }

    [Fact]
    public void RecentTurns_AreIncludedVerbatim_WithoutRecall()
    {
        using var store = BlobStore.Open(StorePath());
        var memory = NewMemory(store);
        memory.Remember("user", "my access phrase is zephyr nine");
        memory.Remember("assistant", "noted");
        memory.NextTurn();

        // Question shares no keyword with the phrase — recall would miss it, but
        // recency must carry it.
        var ctx = memory.BuildContext(null, "please continue", 200);
        Assert.Contains("zephyr nine", ctx.SystemText);
        Assert.Contains("### Recent turns", ctx.SystemText);
    }

    [Fact]
    public void GhostMemory_SpansSessions_EndToEnd()
    {
        // Session A stores a fact and dies.
        using (var storeA = BlobStore.Open(StorePath()))
        {
            var memA = NewMemory(storeA);
            memA.Remember("user", "the deploy password is quartz-owl-42");
            memA.Remember("assistant", "understood, stored");
            memA.NextTurn();
        }

        // Session B — new process, new ConversationMemory, same file.
        using var storeB = BlobStore.Open(StorePath());
        var memB = NewMemory(storeB);
        var ctx = memB.BuildContext("You are terse.", "what is the deploy password?", 300);

        Assert.NotEmpty(ctx.UsedBlobIds);
        Assert.Contains("quartz-owl-42", ctx.SystemText);
        Assert.Contains("[#", ctx.SystemText);   // provenance survives the grave
    }

    // ───────────────────── review-driven regressions ─────────────────────

    /// <summary>Off-by-one fix: the oldest recent turn must not appear twice.</summary>
    [Fact]
    public void OldestRecentTurn_IsNotDuplicatedByRecall()
    {
        using var store = BlobStore.Open(StorePath());
        var memory = new ConversationMemory(store, FakeCount,
            new MemoryOptions { RecentTurns = 1, SafetyMarginTokens = 4 });

        memory.Remember("user", "the beacon frequency is 121.5 megahertz");
        memory.Remember("assistant", "beacon frequency noted");
        memory.NextTurn();

        var ctx = memory.BuildContext(null, "what beacon frequency did we set", 300);

        // Still within the recent window: present in the recent block, and must
        // NOT also be recalled as a memory blob.
        Assert.Contains("121.5", ctx.SystemText);
        Assert.Empty(ctx.UsedBlobIds);
    }

    /// <summary>Verbatim splitting: stored parts concatenate to the original message.</summary>
    [Fact]
    public void OversizedMessage_PartsConcatenateExactly()
    {
        using var store = BlobStore.Open(StorePath());
        var memory = new ConversationMemory(store, FakeCount,
            new MemoryOptions { MaxBlobTokens = 8, SafetyMarginTokens = 4 });

        // Multi-newline runs and irregular spacing — the shapes the old splitter mangled.
        string text = "alpha one two three four five six seven\n\n\n\nbravo eight nine ten"
                    + " eleven twelve thirteen\n\ncharlie  double  spaced tail";
        memory.Remember("user", text);

        var parts = new List<string>();
        for (long id = 1; id <= store.Count; id++)
            parts.Add(store.Get(id)!.Text);

        Assert.True(parts.Count > 1, "message should have split");
        Assert.Equal(text, string.Concat(parts));   // byte-exact reconstruction
    }

    /// <summary>Punctuation-free runs (logs, code) must still split under the cap.</summary>
    [Fact]
    public void PunctuationFreeOversizedRun_StillSplits()
    {
        using var store = BlobStore.Open(StorePath());
        var memory = new ConversationMemory(store, FakeCount,
            new MemoryOptions { MaxBlobTokens = 10, SafetyMarginTokens = 4 });

        string run = string.Join(' ', Enumerable.Range(0, 60).Select(i => $"logline{i}"));
        memory.Remember("assistant", run);

        Assert.True(store.Count >= 5, $"expected several parts, got {store.Count}");
        for (long id = 1; id <= store.Count; id++)
            Assert.True(FakeCount(store.Get(id)!.Text) <= 10,
                $"blob {id} exceeds the cap");

        var parts = new List<string>();
        for (long id = 1; id <= store.Count; id++)
            parts.Add(store.Get(id)!.Text);
        Assert.Equal(run, string.Concat(parts));
    }

    /// <summary>A failed Open must release the sidecar lock or the store wedges in-process.</summary>
    [Fact]
    public void FailedOpen_ReleasesTheLock()
    {
        string path = StorePath("wedge.jsonl");
        Directory.CreateDirectory(path);   // data path IS a directory -> open fails

        Assert.ThrowsAny<Exception>(() => BlobStore.Open(path));
        Directory.Delete(path);

        // If the lock leaked, this reports "already open in another session".
        using var store = BlobStore.Open(path);
        store.Append("s1", 0, "user", "recovered after failed open", 4);
        Assert.Equal(1, store.Count);
    }

    /// <summary>Lone surrogates: in-session recall must equal post-reopen recall.</summary>
    [Fact]
    public void LoneSurrogate_IsConsistentAcrossReopen()
    {
        string text = "prefix \uD83D suffix";   // unpaired high surrogate
        string inSession;
        using (var store = BlobStore.Open(StorePath()))
        {
            long id = store.Append("s1", 0, "user", text, 3).Id;
            inSession = store.Get(id)!.Text;
        }
        using var reopened = BlobStore.Open(StorePath());
        Assert.Equal(inSession, reopened.Get(1)!.Text);
        Assert.DoesNotContain('\uD83D', reopened.Get(1)!.Text);   // sanitized, both sides
    }

    private sealed class FakeSession : ICnetInferenceSession
    {
        public CnetHarnessGenerateOptions? LastOptions;
        public CnetHarnessGenerationResult Generate(CnetHarnessGenerateOptions options)
        {
            LastOptions = options;
            return new CnetHarnessGenerationResult("ok", 10, 5, 1, 1, 0, 0f,
                CnetHarnessSamplingMode.Deterministic, false, 0f, 1f, 0, 0f);
        }
        public CnetHarnessRouteInfo ProbeRoute(string role,
            CnetHarnessSamplingMode overrideMode = CnetHarnessSamplingMode.Auto)
            => new(0, 0f, CnetHarnessSamplingMode.Deterministic, 0f, 1f, 0, 0f);
        public void Dispose() { }
    }

    /// <summary>Clamp fix: an oversized answer budget is clamped, not a crash.</summary>
    [Theory]
    [InlineData(2048u)]          // == window
    [InlineData(3000u)]          // > window
    [InlineData(3_000_000_000u)] // > int.MaxValue: the old (int) cast wrapped
    public void OversizedMaxTokens_IsClamped_NotACrash(uint maxTokens)
    {
        using var store = BlobStore.Open(StorePath());
        var session = new FakeSession();
        var ghost = new MemorySession(session,
            new ConversationMemory(store, FakeCount,
                new MemoryOptions { SafetyMarginTokens = 4 }), 2048);

        var r = ghost.Generate("sys", "hello there", maxTokens);

        Assert.NotNull(session.LastOptions);
        Assert.True(session.LastOptions!.MaxTokens <= 2048 - 128,
            $"maxTokens {session.LastOptions.MaxTokens} not clamped under window - reserve");
        Assert.Equal("ok", r.Result.Text);
    }

    /// <summary>At the minimum window (256), the clamp still leaves the prompt reserve.</summary>
    [Fact]
    public void MinimumWindow_ClampsToTheReserveGap()
    {
        using var store = BlobStore.Open(StorePath());
        var session = new FakeSession();
        var ghost = new MemorySession(session,
            new ConversationMemory(store, FakeCount,
                new MemoryOptions { SafetyMarginTokens = 4 }), 256);

        ghost.Generate("sys", "hi", maxTokens: 4000);

        Assert.Equal(128u, session.LastOptions!.MaxTokens);   // 256 - 128 reserve
    }

    // ───────────────────── temporal anchors ─────────────────────

    /// <summary>
    /// The live failure this exists for: "what was the first thing i said"
    /// shares NO keyword with the stored "hey", so keyword recall alone
    /// returns nothing and the model guesses from its recent window.
    /// Ordering questions must pull the session's earliest blobs.
    /// </summary>
    [Fact]
    public void TemporalQuestion_RecallsSessionStart_DespiteZeroKeywordOverlap()
    {
        using var store = BlobStore.Open(StorePath());
        var memory = NewMemory(store);

        // Reproduce the user's transcript shape: several turns of small talk.
        memory.Remember("user", "hey");
        memory.Remember("assistant", "Hey! How can I help you today?");
        memory.NextTurn();
        memory.Remember("user", "what can you do?");
        memory.Remember("assistant", "many things: writing, coding, analysis");
        memory.NextTurn();
        memory.Remember("user", "how many languages do you know");
        memory.Remember("assistant", "dozens of languages");
        memory.NextTurn();
        memory.Remember("user", "can you look stuff up online?");
        memory.Remember("assistant", "no, I cannot browse");
        memory.NextTurn();

        var ctx = memory.BuildContext(null, "what was the first thing i said in this chat?", 400);

        Assert.Contains("hey", ctx.SystemText);
        Assert.Contains(1L, ctx.UsedBlobIds);          // blob #1 IS "hey"
        Assert.Contains("### Memory", ctx.SystemText);
    }

    [Fact]
    public void TemporalAnchors_DoNotDuplicate_RecentTurns()
    {
        using var store = BlobStore.Open(StorePath());
        var memory = new ConversationMemory(store, FakeCount,
            new MemoryOptions { RecentTurns = 4, SafetyMarginTokens = 4 });

        memory.Remember("user", "opening line about quasar drift");
        memory.Remember("assistant", "noted");
        memory.NextTurn();

        // Only one turn exists and it is inside the recent window: the anchor
        // path must not re-inject it as a memory blob.
        var ctx = memory.BuildContext(null, "what did i say first?", 400);

        Assert.Empty(ctx.UsedBlobIds);
        Assert.Contains("quasar", ctx.SystemText);      // present via recent turns
        Assert.DoesNotContain("### Memory", ctx.SystemText);
    }

    [Fact]
    public void TemporalAnchors_AreSessionScoped()
    {
        using (var storeA = BlobStore.Open(StorePath()))
        {
            var memA = NewMemory(storeA);
            memA.Remember("user", "previous-session opener about falcon telemetry");
            memA.NextTurn();
        }

        using var storeB = BlobStore.Open(StorePath());
        var memB = NewMemory(storeB);
        memB.Remember("user", "current session begins here");
        memB.Remember("assistant", "ok");
        memB.NextTurn();
        memB.Remember("user", "filler turn");
        memB.Remember("assistant", "ok");
        memB.NextTurn();
        memB.Remember("user", "more filler");
        memB.Remember("assistant", "ok");
        memB.NextTurn();

        var ctx = memB.BuildContext(null, "what was the first thing i said?", 400);

        // The anchor is THIS session's opener, not the previous session's.
        Assert.Contains("current session begins here", ctx.SystemText);
        Assert.DoesNotContain("falcon telemetry", ctx.SystemText);
    }

    [Fact]
    public void NonTemporalIrrelevantQuery_StillRecallsNothing()
    {
        using var store = SeededStore();
        var memory = NewMemory(store);
        var ctx = memory.BuildContext(null, "compose a haiku about rain", 300);
        Assert.Empty(ctx.UsedBlobIds);
    }

    [Fact]
    public void TemporalAnchors_RespectTheBudget()
    {
        using var store = BlobStore.Open(StorePath());
        var memory = new ConversationMemory(store, FakeCount,
            new MemoryOptions { SafetyMarginTokens = 4 });

        memory.Remember("user", string.Join(' ', Enumerable.Repeat("opening", 50)));
        memory.Remember("assistant", string.Join(' ', Enumerable.Repeat("reply", 50)));
        memory.NextTurn();
        for (int t = 0; t < 3; t++)
        {
            memory.Remember("user", $"filler turn {t}");
            memory.Remember("assistant", "ok");
            memory.NextTurn();
        }

        int budget = 60;   // too small for the 50-word opener
        var ctx = memory.BuildContext(null, "what came first?", budget);

        int total = FakeCount(ctx.SystemText) + FakeCount("what came first?");
        Assert.True(total <= budget, $"context used {total} of {budget}");
    }

    // ───────────────────── forgetting (tombstones) ─────────────────────

    [Fact]
    public void Forget_RemovesFromRecall_Immediately()
    {
        using var store = SeededStore();
        long id = store.Recall("vault code", 1).Single().Id;

        Assert.True(store.Forget(id));

        Assert.Empty(store.Recall("vault code", 4));
        Assert.Null(store.Get(id));
        Assert.Equal(3, store.Count);
    }

    [Fact]
    public void Forget_PersistsAcrossReopen()
    {
        long id;
        using (var store = SeededStore())
        {
            id = store.Recall("vault code", 1).Single().Id;
            Assert.True(store.Forget(id));
        }

        using var reopened = BlobStore.Open(StorePath());
        Assert.Empty(reopened.Recall("vault code", 4));
        Assert.Null(reopened.Get(id));
        Assert.Equal(3, reopened.Count);
        Assert.Equal(0, reopened.CorruptLinesSkipped);   // tombstones are not corruption
    }

    /// <summary>Deletion is an event, not an erasure: the file keeps history.</summary>
    [Fact]
    public void Forget_KeepsTheOriginalLine_InTheFile()
    {
        using (var store = SeededStore())
            store.Forget(1);

        string file = File.ReadAllText(StorePath());
        Assert.Contains("7291", file);          // the forgotten text is still history
        Assert.Contains("\"del\":1", file);     // masked by a tombstone
    }

    /// <summary>
    /// Ids are never reused after a forget — receipts from any point in history
    /// must stay unambiguous forever.
    /// </summary>
    [Fact]
    public void ForgottenIds_AreNeverReused()
    {
        long newId;
        using (var store = SeededStore())
        {
            long last = store.Recall("bandwidth", 1).Single().Id;   // the newest blob
            store.Forget(last);
            newId = store.Append("s3", 0, "user", "a brand new memory", 4).Id;
            Assert.True(newId > last, $"id {newId} reused the forgotten range (last was {last})");
        }

        using var reopened = BlobStore.Open(StorePath());
        Assert.Equal("a brand new memory", reopened.Get(newId)!.Text);
        long nextAfterReopen = reopened.Append("s4", 0, "user", "and another", 2).Id;
        Assert.True(nextAfterReopen > newId);
    }

    [Fact]
    public void Forget_UnknownOrAlreadyForgotten_ReturnsFalse()
    {
        using var store = SeededStore();
        Assert.False(store.Forget(9999));
        Assert.True(store.Forget(1));
        Assert.False(store.Forget(1));
    }

    /// <summary>Forgetting must shrink document frequency, or idf drifts and
    /// scoring degrades as the store ages.</summary>
    [Fact]
    public void Forget_UpdatesIndexStatistics()
    {
        using var store = BlobStore.Open(StorePath());
        long a = store.Append("s1", 0, "user", "zephyr protocol details alpha", 4).Id;
        store.Append("s1", 1, "user", "zephyr protocol details beta", 4);

        store.Forget(a);

        var hits = store.Recall("zephyr protocol", 4);
        Assert.Single(hits);
        Assert.Contains("beta", hits[0].Text);
    }

    [Fact]
    public void ForgottenBlob_NoLongerServesTemporalAnchors()
    {
        using var store = BlobStore.Open(StorePath());
        var memory = NewMemory(store);
        memory.Remember("user", "embarrassing opener about llamas");
        memory.Remember("assistant", "noted");
        memory.NextTurn();
        for (int t = 0; t < 3; t++)
        {
            memory.Remember("user", $"filler {t}");
            memory.Remember("assistant", "ok");
            memory.NextTurn();
        }

        store.Forget(1);   // the opener

        var ctx = memory.BuildContext(null, "what was the first thing i said?", 400);
        Assert.DoesNotContain("llamas", ctx.SystemText);
        Assert.DoesNotContain(1L, ctx.UsedBlobIds);
    }
}
