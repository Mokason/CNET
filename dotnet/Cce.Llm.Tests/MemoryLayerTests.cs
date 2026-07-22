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
}
