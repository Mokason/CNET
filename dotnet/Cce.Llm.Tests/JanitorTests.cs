using CNET.Cce.Llm.Memory;
using CNET.Cce.Llm.Orchestration;
using Xunit;

namespace CNET.Cce.Llm.Tests;

/// <summary>
/// The janitor: compaction archives dead weight without destroying history,
/// preserves every live guarantee (recall view, usage counts, never-reuse
/// ids), yields to live sessions, and triggers through the same governed
/// policy loop as everything else.
/// </summary>
public sealed class JanitorTests : IDisposable
{
    private readonly string _dir;

    public JanitorTests()
    {
        _dir = Path.Combine(Path.GetTempPath(), "cnet-janitor", Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(_dir);
    }

    public void Dispose()
    {
        try { Directory.Delete(_dir, recursive: true); } catch (IOException) { }
    }

    private string StorePath() => Path.Combine(_dir, "s.jsonl");

    private (long Kept, long Dead) SeedClutteredStore()
    {
        using var store = BlobStore.Open(StorePath());
        long kept = store.Append("s1", 0, "user", "remember this: the keeper survives", 8).Id;
        long dead = store.Append("s1", 1, "assistant", "garbage that will be forgotten", 8).Id;
        store.RecordUsage([kept], "s2");
        store.RecordUsage([dead], "s2");        // usage of a soon-dead id
        store.Forget(dead);                      // blob line + tombstone = dead weight
        return (kept, dead);
    }

    [Fact]
    public void Compact_ArchivesDeadWeight_PreservesTheLiveView()
    {
        (long kept, long dead) = SeedClutteredStore();

        int archived = BlobStore.Compact(StorePath());

        // dead blob line + its usage line + tombstone = 3 archived lines
        Assert.Equal(3, archived);
        string archive = File.ReadAllText(StorePath() + ".archive.jsonl");
        Assert.Contains("garbage that will be forgotten", archive);   // history preserved
        Assert.Contains($"\"del\":{dead}", archive);

        using var reopened = BlobStore.Open(StorePath());
        Assert.Equal(1, reopened.Count);
        Assert.Equal("remember this: the keeper survives", reopened.Get(kept)!.Text);
        Assert.Equal(1, reopened.UsageCount(kept));                   // live usage kept
        Assert.Null(reopened.Get(dead));
    }

    /// <summary>The guarantee compaction must not break: forgotten ids are
    /// never reused, even after their lines leave the working file.</summary>
    [Fact]
    public void Compact_PreservesNeverReuseIds_ViaTheSentinel()
    {
        (_, long dead) = SeedClutteredStore();
        BlobStore.Compact(StorePath());

        using var reopened = BlobStore.Open(StorePath());
        long next = reopened.Append("s3", 0, "user", "post-compaction blob", 4).Id;

        Assert.True(next > dead, $"id {next} reused space at or below forgotten id {dead}");
    }

    [Fact]
    public void Compact_YieldsToALiveSession()
    {
        SeedClutteredStore();
        using var live = BlobStore.Open(StorePath());   // writer lock held

        Assert.Equal(-1, BlobStore.Compact(StorePath()));
        Assert.False(File.Exists(StorePath() + ".archive.jsonl"));   // untouched
    }

    [Fact]
    public void Compact_CleanStore_IsANoOp()
    {
        using (var store = BlobStore.Open(StorePath()))
            store.Append("s1", 0, "user", "nothing dead here", 4);

        Assert.Equal(0, BlobStore.Compact(StorePath()));
        Assert.False(File.Exists(StorePath() + ".archive.jsonl"));
    }

    [Fact]
    public void Compact_IsIdempotent()
    {
        SeedClutteredStore();
        Assert.True(BlobStore.Compact(StorePath()) > 0);
        Assert.Equal(0, BlobStore.Compact(StorePath()));   // second pass: nothing left
    }

    [Fact]
    public void Snapshot_CountsDeadWeight()
    {
        SeedClutteredStore();
        GhostSnapshot snap = BlobStore.Snapshot(StorePath());
        Assert.Equal(3, snap.DeadLines);       // dead blob + its usage + tombstone
        Assert.Equal(5, snap.TotalLines);

        BlobStore.Compact(StorePath());
        GhostSnapshot after = BlobStore.Snapshot(StorePath());
        Assert.Equal(0, after.DeadLines);
    }

    // ─────────────── governed like everything else ───────────────

    [Fact]
    public void Policy_TriggersJanitor_OnDeadWeightThresholds()
    {
        static Observations Obs(int dead, int total) =>
            new(new DateTime(2026, 7, 23, 12, 0, 0, DateTimeKind.Utc),
                0, 0, 0, false, 0, 0, 0, 0, true, false, dead, total);

        var config = new OrchestratorConfig();

        Assert.Contains(GhostPolicies.Plan(Obs(40, 400), new OrchestratorState(), config),
            a => a.Name == "janitor");                       // absolute threshold
        Assert.Contains(GhostPolicies.Plan(Obs(12, 40), new OrchestratorState(), config),
            a => a.Name == "janitor");                       // fraction threshold
        Assert.DoesNotContain(GhostPolicies.Plan(Obs(5, 400), new OrchestratorState(), config),
            a => a.Name == "janitor");                       // clean enough
    }

    /// <summary>Foreign-written stores (different JSON spacing) must not get
    /// their live lines archived as spurious copies — identity, not bytes.</summary>
    [Fact]
    public void Compact_ForeignStore_ArchivesOnlyTrueDeadWeight()
    {
        string path = StorePath();
        // Hand-written with spaces after colons — parseable, not byte-canonical.
        File.WriteAllLines(path,
        [
            """{"id": 1, "sid": "s1", "turn": 0, "role": "user", "ts": "2026-07-23T00:00:00Z", "text": "live foreign blob", "tok": 4, "v": 1}""",
            """{"id": 2, "sid": "s1", "turn": 1, "role": "user", "ts": "2026-07-23T00:00:00Z", "text": "dead foreign blob", "tok": 4, "v": 1}""",
            """{"del": 2, "ts": "2026-07-23T01:00:00Z"}""",
        ]);

        int archived = BlobStore.Compact(path);

        Assert.Equal(2, archived);   // dead blob + tombstone; live line NOT archived
        string archive = File.ReadAllText(path + ".archive.jsonl");
        Assert.DoesNotContain("live foreign blob", archive);

        using var reopened = BlobStore.Open(path);
        Assert.Equal("live foreign blob", reopened.Get(1)!.Text);
    }
}
