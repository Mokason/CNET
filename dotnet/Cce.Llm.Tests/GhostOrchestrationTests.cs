using CNET.Cce.Llm.Memory;
using CNET.Cce.Llm.Orchestration;
using Xunit;

namespace CNET.Cce.Llm.Tests;

/// <summary>
/// The reconcile loop's discipline: state-derived planning, cooldowns,
/// exponential backoff, per-tick budgets, dry-run, journal receipts, and the
/// lock-free snapshot the orchestrator observes through.
/// </summary>
public sealed class GhostOrchestrationTests : IDisposable
{
    private readonly string _dir;

    public GhostOrchestrationTests()
    {
        _dir = Path.Combine(Path.GetTempPath(), "cnet-orchestrate", Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(_dir);
    }

    public void Dispose()
    {
        try { Directory.Delete(_dir, recursive: true); } catch (IOException) { }
    }

    private static Observations Obs(
        long maxId = 0, int blobs = 0, int corrupt = 0,
        bool inboxExists = false, int inboxLines = 0, double inboxAge = 0,
        int ghostGaps = 0, int ghostWaiting = 0,
        bool drainer = true, bool teacher = false,
        DateTime? now = null) =>
        new(now ?? new DateTime(2026, 7, 23, 12, 0, 0, DateTimeKind.Utc),
            maxId, blobs, corrupt, inboxExists, inboxLines, inboxAge,
            ghostGaps, ghostWaiting, drainer, teacher);

    private (Reconciler Reconciler, OrchestratorState State,
             List<(string Name, int Calls)> Log,
             Dictionary<string, Func<PlannedAction, OrchestratorState, string>> Executors)
        NewReconciler(OrchestratorConfig? config = null)
    {
        var state = new OrchestratorState();
        var journal = new DecisionJournal(Path.Combine(_dir, "journal.jsonl"));
        var log = new List<(string, int)>();
        var executors = new Dictionary<string, Func<PlannedAction, OrchestratorState, string>>();
        var reconciler = new Reconciler(config ?? new OrchestratorConfig(), state, journal, executors);
        return (reconciler, state, log, executors);
    }

    // ─────────────── policies are pure state functions ───────────────

    [Fact]
    public void Plan_NewBlobsPastWatermark_TriggersConsolidate()
    {
        var plan = GhostPolicies.Plan(Obs(maxId: 10), new OrchestratorState { LastConsolidatedMaxId = 3 },
                                      new OrchestratorConfig { MinNewBlobs = 6 });
        Assert.Contains(plan, a => a.Name == "consolidate" && a.Reason.Contains("7 new blobs"));

        var quiet = GhostPolicies.Plan(Obs(maxId: 8), new OrchestratorState { LastConsolidatedMaxId = 3 },
                                       new OrchestratorConfig { MinNewBlobs = 6 });
        Assert.DoesNotContain(quiet, a => a.Name == "consolidate");
    }

    [Fact]
    public void Plan_WaitingGaps_NoTeacher_RespectsPermissionGate()
    {
        var obs = Obs(ghostGaps: 9, ghostWaiting: 9, teacher: false);

        var blocked = GhostPolicies.Plan(obs, new OrchestratorState(), new OrchestratorConfig());
        var rec = Assert.Single(blocked, a => a.Name == "recommend-teacher-lane");
        Assert.True(rec.JournalOnly);

        var allowed = GhostPolicies.Plan(obs, new OrchestratorState(),
                                         new OrchestratorConfig { AllowTeacherStart = true });
        var start = Assert.Single(allowed, a => a.Name == "start-teacher-lane");
        Assert.False(start.JournalOnly);
    }

    [Fact]
    public void Plan_TeacherAlreadyActive_NoAction()
    {
        var plan = GhostPolicies.Plan(Obs(ghostGaps: 9, ghostWaiting: 9, teacher: true),
                                      new OrchestratorState(),
                                      new OrchestratorConfig { AllowTeacherStart = true });
        Assert.DoesNotContain(plan, a => a.Name.Contains("teacher"));
    }

    [Fact]
    public void Plan_InboxWithNoDrainer_And_StuckInbox_AreDistinct()
    {
        var noDrainer = GhostPolicies.Plan(
            Obs(inboxExists: true, inboxLines: 4, drainer: false, teacher: false),
            new OrchestratorState(), new OrchestratorConfig());
        Assert.Contains(noDrainer, a => a.Name == "warn-no-drainer");

        var stuck = GhostPolicies.Plan(
            Obs(inboxExists: true, inboxLines: 4, inboxAge: 999, drainer: true),
            new OrchestratorState(), new OrchestratorConfig { InboxStuckSeconds = 300 });
        Assert.Contains(stuck, a => a.Name == "warn-inbox-stuck");

        var fresh = GhostPolicies.Plan(
            Obs(inboxExists: true, inboxLines: 4, inboxAge: 10, drainer: true),
            new OrchestratorState(), new OrchestratorConfig { InboxStuckSeconds = 300 });
        Assert.DoesNotContain(fresh, a => a.Name.StartsWith("warn-inbox"));
    }

    // ─────────────── reconciler discipline ───────────────

    [Fact]
    public void Tick_ExecutesAndJournals_WithReceipts()
    {
        var (reconciler, state, _, executors) = NewReconciler();
        int calls = 0;
        executors["consolidate"] = (_, s) => { calls++; s.LastConsolidatedMaxId = 10; return "noted 3/3"; };

        var decisions = reconciler.Tick(Obs(maxId: 10));

        var d = Assert.Single(decisions, x => x.Action == "consolidate");
        Assert.True(d.Executed);
        Assert.Equal("noted 3/3", d.Outcome);
        Assert.Equal(1, calls);
        Assert.Equal(10, state.LastConsolidatedMaxId);
    }

    [Fact]
    public void Tick_Cooldown_SuppressesRepeats_UntilElapsed()
    {
        var (reconciler, _, _, executors) = NewReconciler(new OrchestratorConfig { CooldownSeconds = 600 });
        int calls = 0;
        executors["consolidate"] = (_, _) => { calls++; return "ok"; };   // watermark NOT advanced

        var t0 = new DateTime(2026, 7, 23, 12, 0, 0, DateTimeKind.Utc);
        reconciler.Tick(Obs(maxId: 10, now: t0));
        reconciler.Tick(Obs(maxId: 10, now: t0.AddSeconds(30)));          // within cooldown
        Assert.Equal(1, calls);

        reconciler.Tick(Obs(maxId: 10, now: t0.AddSeconds(700)));         // cooldown elapsed
        Assert.Equal(2, calls);
    }

    [Fact]
    public void Tick_FailureBackoff_IsExponential_AndResetsOnSuccess()
    {
        var (reconciler, state, _, executors) = NewReconciler(new OrchestratorConfig { CooldownSeconds = 100 });
        bool fail = true;
        int calls = 0;
        executors["consolidate"] = (_, _) =>
        {
            calls++;
            if (fail) throw new InvalidOperationException("boom");
            return "ok";
        };

        var t0 = new DateTime(2026, 7, 23, 12, 0, 0, DateTimeKind.Utc);
        var d1 = reconciler.Tick(Obs(maxId: 10, now: t0));
        Assert.Contains("FAILED (1 consecutive)", Assert.Single(d1).Outcome);
        Assert.Equal(1, state.ConsecutiveFailures["consolidate"]);

        // Backoff doubled: 100 * 2^1 = 200s. At +150s: still suppressed.
        reconciler.Tick(Obs(maxId: 10, now: t0.AddSeconds(150)));
        Assert.Equal(1, calls);

        // At +250s it retries; succeed now and failures reset.
        fail = false;
        var d3 = reconciler.Tick(Obs(maxId: 10, now: t0.AddSeconds(250)));
        Assert.True(Assert.Single(d3).Executed);
        Assert.False(state.ConsecutiveFailures.ContainsKey("consolidate"));
    }

    [Fact]
    public void Tick_PerTickBudget_DefersOverflow()
    {
        var (reconciler, _, _, executors) = NewReconciler(new OrchestratorConfig
        {
            MaxActionsPerTick = 0,     // everything defers
        });
        executors["consolidate"] = (_, _) => "should not run";

        var decisions = reconciler.Tick(Obs(maxId: 100));

        var d = Assert.Single(decisions, x => x.Action == "consolidate");
        Assert.False(d.Executed);
        Assert.Contains("budget exhausted", d.Outcome);
    }

    [Fact]
    public void Tick_DryRun_ExecutesNothing_JournalsIntent()
    {
        var (reconciler, state, _, executors) = NewReconciler();
        executors["consolidate"] = (_, _) => throw new Exception("must not be called");

        var decisions = reconciler.Tick(Obs(maxId: 100), dryRun: true);

        var d = Assert.Single(decisions, x => x.Action == "consolidate");
        Assert.False(d.Executed);
        Assert.True(d.DryRun);
        Assert.Contains("would execute", d.Outcome);
        Assert.Equal(0, state.LastConsolidatedMaxId);
    }

    [Fact]
    public void Tick_Advisories_JournalOnce_ThenCooldown()
    {
        var (reconciler, _, _, _) = NewReconciler(new OrchestratorConfig { AdvisoryCooldownSeconds = 1800 });
        var t0 = new DateTime(2026, 7, 23, 12, 0, 0, DateTimeKind.Utc);

        var d1 = reconciler.Tick(Obs(ghostWaiting: 9, ghostGaps: 9, now: t0));
        Assert.Single(d1, x => x.Action == "recommend-teacher-lane");

        var d2 = reconciler.Tick(Obs(ghostWaiting: 9, ghostGaps: 9, now: t0.AddSeconds(60)));
        Assert.Empty(d2);   // advisory suppressed, journal not spammed
    }

    [Fact]
    public void State_RoundTrips_AndSurvivesCorruption()
    {
        string path = Path.Combine(_dir, "state.json");
        var state = new OrchestratorState { LastConsolidatedMaxId = 42 };
        state.LastRunUtc["consolidate"] = new DateTime(2026, 7, 23, 0, 0, 0, DateTimeKind.Utc);
        state.Save(path);

        var loaded = OrchestratorState.Load(path);
        Assert.Equal(42, loaded.LastConsolidatedMaxId);
        Assert.True(loaded.LastRunUtc.ContainsKey("consolidate"));

        File.WriteAllText(path, "{ not json");
        var recovered = OrchestratorState.Load(path);
        Assert.Equal(0, recovered.LastConsolidatedMaxId);   // conservative restart, no crash
    }

    // ─────────────── the lock-free observation path ───────────────

    [Fact]
    public void Snapshot_WorksWhileALiveSessionHoldsTheWriterLock()
    {
        string path = Path.Combine(_dir, "live.jsonl");
        using var live = BlobStore.Open(path);       // writer lock held
        long id = live.Append("s1", 0, "user", "remember this: the drawbridge code is 7777", 10).Id;
        live.RecordUsage([id], "s2");

        GhostSnapshot snap = BlobStore.Snapshot(path);   // no lock contention

        Assert.Equal(1, snap.All().Count);
        Assert.Equal(id, snap.MaxSeenId);
        Assert.Equal(1, snap.UsageCount(id));
        var items = new GhostConsolidator(snap).Extract();
        Assert.Single(items);                         // consolidation works off the snapshot
    }

    [Fact]
    public void Snapshot_MasksTombstones_SameAsTheLiveLoader()
    {
        string path = Path.Combine(_dir, "tomb.jsonl");
        long dead;
        using (var store = BlobStore.Open(path))
        {
            dead = store.Append("s1", 0, "user", "remember this: obsolete fact", 6).Id;
            store.Append("s1", 1, "user", "keep me", 3);
            store.Forget(dead);
        }

        GhostSnapshot snap = BlobStore.Snapshot(path);
        Assert.Single(snap.All());
        Assert.Equal(dead + 1, snap.MaxSeenId);       // watermark still covers the tombstoned id
    }
}
