using CNET.Cce.Llm.Memory;
using Xunit;

namespace CNET.Cce.Llm.Tests;

/// <summary>
/// The hippocampus→cortex seam: usage events persisted in the store, the four
/// extraction rules, deterministic skill naming, and emission receipts (native
/// seam faked; the real cnet.so path is covered by NativeConsolidationTests).
/// </summary>
public sealed class GhostConsolidationTests : IDisposable
{
    private readonly string _dir;

    public GhostConsolidationTests()
    {
        _dir = Path.Combine(Path.GetTempPath(), "cnet-consolidate", Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(_dir);
    }

    public void Dispose()
    {
        try { Directory.Delete(_dir, recursive: true); } catch (IOException) { }
    }

    private string StorePath() => Path.Combine(_dir, "c.jsonl");

    // ─────────────── usage events ───────────────

    [Fact]
    public void UsageEvents_PersistAcrossReopen()
    {
        long id;
        using (var store = BlobStore.Open(StorePath()))
        {
            id = store.Append("s1", 0, "user", "the reactor code is 4-8-15", 8).Id;
            store.RecordUsage([id], "s2");
            store.RecordUsage([id], "s3");
            Assert.Equal(2, store.UsageCount(id));
            Assert.Equal(2, store.UsageSessions(id));
        }

        using var reopened = BlobStore.Open(StorePath());
        Assert.Equal(2, reopened.UsageCount(id));
        Assert.Equal(2, reopened.UsageSessions(id));
        Assert.Equal(0, reopened.CorruptLinesSkipped);   // usage lines are a known kind
    }

    [Fact]
    public void UsageEvents_SameSessionRepeats_CountOnceForSessions()
    {
        using var store = BlobStore.Open(StorePath());
        long id = store.Append("s1", 0, "user", "fact", 2).Id;
        store.RecordUsage([id], "s2");
        store.RecordUsage([id], "s2");
        Assert.Equal(2, store.UsageCount(id));
        Assert.Equal(1, store.UsageSessions(id));
    }

    [Fact]
    public void All_ReturnsLiveBlobsOnly_OldestFirst()
    {
        using var store = BlobStore.Open(StorePath());
        long a = store.Append("s", 0, "user", "first", 2).Id;
        long b = store.Append("s", 1, "user", "second", 2).Id;
        store.Forget(a);
        var all = store.All();
        Assert.Single(all);
        Assert.Equal(b, all[0].Id);
    }

    // ─────────────── extraction rules ───────────────

    [Fact]
    public void Extract_ExplicitRememberRequest()
    {
        using var store = BlobStore.Open(StorePath());
        store.Append("s1", 0, "user", "remember this: the deploy key lives in vault slot 9", 12);
        store.Append("s1", 0, "assistant", "noted", 2);

        var items = new GhostConsolidator(store).Extract();

        var item = Assert.Single(items);
        Assert.Contains("vault slot 9", item.Text);
        Assert.Equal("explicit remember request", item.Reason);
        Assert.StartsWith("gh_", item.SkillName);
    }

    [Fact]
    public void Extract_UserCorrection_RequiresAdjacentAssistantTurn()
    {
        using var store = BlobStore.Open(StorePath());
        store.Append("s1", 0, "user", "what is the port?", 5);
        store.Append("s1", 0, "assistant", "the port is 8080", 5);
        store.Append("s1", 1, "user", "wrong, the port is 9119", 6);
        // correction-shaped text NOT after an assistant turn: not taken
        store.Append("s2", 0, "user", "no, let's talk about lunch", 6);

        var items = new GhostConsolidator(store).Extract();

        var item = Assert.Single(items);
        Assert.Equal("user correction", item.Reason);
        Assert.Contains("9119", item.Text);
    }

    [Fact]
    public void Extract_CrossSessionRequery_PromotesTheEarlierFact()
    {
        using var store = BlobStore.Open(StorePath());
        store.Append("sessA", 0, "user", "the kraken migration finishes friday at the harbor", 10);
        store.Append("sessA", 0, "assistant", "understood", 2);
        store.Append("sessB", 0, "user", "when does the kraken migration reach the harbor?", 10);

        var items = new GhostConsolidator(store).Extract();

        Assert.Contains(items, i => i.Reason.StartsWith("re-queried in a later session") &&
                                    i.Text.Contains("finishes friday"));
    }

    [Fact]
    public void Extract_UsageRule_NeedsBothThresholds()
    {
        using var store = BlobStore.Open(StorePath());
        long hot = store.Append("s1", 0, "user", "hot fact", 3).Id;
        long warm = store.Append("s1", 0, "user", "warm fact", 3).Id;
        store.RecordUsage([hot], "s2");
        store.RecordUsage([hot], "s3");
        store.RecordUsage([warm], "s2");
        store.RecordUsage([warm], "s2");   // 2 uses but 1 session

        var items = new GhostConsolidator(store).Extract();

        var item = Assert.Single(items);
        Assert.Equal(hot, item.BlobId);
        Assert.StartsWith("used in 2 prompts across 2 sessions", item.Reason);
    }

    [Fact]
    public void Extract_ForgottenBlobs_AreAntiTeaching()
    {
        using var store = BlobStore.Open(StorePath());
        long id = store.Append("s1", 0, "user", "remember this: the old wrong password", 8).Id;
        store.Forget(id);

        Assert.Empty(new GhostConsolidator(store).Extract());
    }

    [Fact]
    public void Extract_DedupsAcrossRules_AndHonorsMaxItems()
    {
        using var store = BlobStore.Open(StorePath());
        long id = store.Append("s1", 0, "user", "remember this: alpha", 4).Id;
        store.RecordUsage([id], "s2");
        store.RecordUsage([id], "s3");     // qualifies under two rules
        for (int i = 0; i < 5; i++)
            store.Append("s1", i + 1, "user", $"remember this: item {i}", 4);

        var consolidator = new GhostConsolidator(store) { MaxItems = 3 };
        var items = consolidator.Extract();

        Assert.Equal(3, items.Count);
        Assert.Equal(items.Count, items.Select(i => i.BlobId).Distinct().Count());
    }

    // ─────────────── skill naming ───────────────

    [Fact]
    public void SkillNames_AreDeterministic_AndFitThePortTag()
    {
        using var store = BlobStore.Open(StorePath());
        var blob = store.Append("s1", 0, "user", "remember this: the wifi password is grendel-999", 10);

        string name1 = GhostConsolidator.SkillNameFor(blob);
        string name2 = GhostConsolidator.SkillNameFor(blob);

        Assert.Equal(name1, name2);                       // coalescing depends on this
        Assert.True(("skill_" + name1).Length <= 32,      // native PORT_TAG_MAX
            $"goal tag too long: skill_{name1}");
        Assert.Matches("^gh_[0-9a-f]{8}_", name1);
    }

    // ─────────────── emission ───────────────

    [Fact]
    public void Emit_CallsNoteSkill_PerItem_WithReceipts()
    {
        using var store = BlobStore.Open(StorePath());
        store.Append("s1", 0, "user", "remember this: alpha beacon", 5);
        store.Append("s1", 1, "user", "remember this: beta beacon", 5);

        var calls = new List<(string Inbox, string Skill, string Text)>();
        var consolidator = new GhostConsolidator(store)
        {
            NoteSkillOverride = (inbox, skill, text) => { calls.Add((inbox, skill, text)); return 0; },
        };

        var receipts = consolidator.Emit(consolidator.Extract(), "/tmp/fake.inbox");

        Assert.Equal(2, receipts.Count);
        Assert.All(receipts, r => Assert.True(r.Emitted));
        Assert.Equal(2, calls.Count);
        Assert.All(calls, c => Assert.Equal("/tmp/fake.inbox", c.Inbox));
        Assert.Contains(calls, c => c.Text.Contains("alpha beacon"));
    }

    [Fact]
    public void Emit_NativeFailure_ReportsHonestReceipt()
    {
        using var store = BlobStore.Open(StorePath());
        store.Append("s1", 0, "user", "remember this: gamma", 4);

        var consolidator = new GhostConsolidator(store)
        {
            NoteSkillOverride = (_, _, _) => -1,
        };
        var receipts = consolidator.Emit(consolidator.Extract(), "/tmp/fake.inbox");

        var r = Assert.Single(receipts);
        Assert.False(r.Emitted);
        Assert.Contains("-1", r.Error);
    }
}

/// <summary>
/// Real-native integration: the actual cnet.so writes the actual inbox line.
/// Skipped when the library is absent (CI without a native build).
/// </summary>
public sealed class NativeConsolidationTests
{
    private static string? FindCnetSo()
    {
        string? dir = Directory.GetCurrentDirectory();
        for (int i = 0; i < 8 && dir is not null; i++)
        {
            string p = Path.Combine(dir, "cnet.so");
            if (File.Exists(p)) return p;
            dir = Path.GetDirectoryName(dir);
        }
        return null;
    }

    [Fact]
    public void NoteSkill_WritesCanonicalInboxLine()
    {
        string? so = FindCnetSo();
        if (so is null) return;   // no native build present — nothing to verify
        Environment.SetEnvironmentVariable("CNET_LIBRARY", so);

        string dir = Path.Combine(Path.GetTempPath(), "cnet-native-consolidate",
            Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(dir);
        string storePath = Path.Combine(dir, "s.jsonl");
        string inbox = Path.Combine(dir, "gap.inbox");
        try
        {
            using var store = BlobStore.Open(storePath);
            store.Append("s1", 0, "user", "remember this: the beacon frequency is 121.5 MHz", 12);

            var consolidator = new GhostConsolidator(store);
            var receipts = consolidator.Emit(consolidator.Extract(), inbox);

            var r = Assert.Single(receipts);
            Assert.True(r.Emitted, r.Error);

            string line = File.ReadAllText(inbox).Trim();
            Assert.StartsWith("NO_PLAN ", line);           // the gap-lane grammar
            Assert.Contains("skill_", line);               // named-skill goal tag
            // Same memory emitted again coalesces: identical signature line.
            consolidator.Emit(consolidator.Extract(), inbox);
            string[] lines = File.ReadAllLines(inbox);
            Assert.Equal(2, lines.Length);
            Assert.Equal(lines[0], lines[1]);
        }
        finally
        {
            try { Directory.Delete(dir, recursive: true); } catch (IOException) { }
        }
    }
}
