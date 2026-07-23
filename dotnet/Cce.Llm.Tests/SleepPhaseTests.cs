using CNET.Cce.CnetHarness;
using CNET.Cce.Llm.Judgment;
using CNET.Cce.Llm.Memory;
using CNET.Cce.Llm.Orchestration;
using Xunit;

namespace CNET.Cce.Llm.Tests;

/// <summary>
/// The rest phase: structural idleness (the writer lock IS the test), session
/// summarization with taste screening, summary provenance, and the governed
/// trigger (idle clock + slept watermark).
/// </summary>
public sealed class SleepPhaseTests : IDisposable
{
    private readonly string _dir;

    public SleepPhaseTests()
    {
        _dir = Path.Combine(Path.GetTempPath(), "cnet-sleep", Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(_dir);
    }

    public void Dispose()
    {
        try { Directory.Delete(_dir, recursive: true); } catch (IOException) { }
    }

    private string StorePath() => Path.Combine(_dir, "s.jsonl");

    private sealed class ScriptedSession(params string[] outputs) : ICnetInferenceSession
    {
        private readonly Queue<string> _script = new(outputs);
        public int Calls { get; private set; }

        public CnetHarnessGenerationResult Generate(CnetHarnessGenerateOptions options)
        {
            Calls++;
            string text = _script.Count > 0 ? _script.Dequeue() : "";
            return new CnetHarnessGenerationResult(text, 10, 5, 1, 1, 0, 0,
                CnetHarnessSamplingMode.Focused, false, 0, 1, 0, 0);
        }

        public CnetHarnessRouteInfo ProbeRoute(string role,
            CnetHarnessSamplingMode overrideMode = CnetHarnessSamplingMode.Auto) =>
            throw new NotSupportedException();

        public void Dispose() { }
    }

    private void SeedSession(string sid, int turns)
    {
        using var store = BlobStore.Open(StorePath());
        for (int i = 0; i < turns; i++)
        {
            store.Append(sid, i, "user", $"question {i} about the lighthouse expedition", 8);
            store.Append(sid, i, "assistant", $"answer {i} with substantial detail", 8);
        }
    }

    // ─────────────── selection ───────────────

    [Fact]
    public void SessionsNeedingSummary_SkipsShortDocAndAlreadySummarized()
    {
        SeedSession("sess-long", 4);      // 8 blobs: due
        SeedSession("sess-short", 1);     // 2 blobs: not worth it
        using (var store = BlobStore.Open(StorePath()))
        {
            var memory = new ConversationMemory(store, t => t.Length / 4 + 1);
            memory.IngestDocument("doc.txt", "documents are material, not sessions");
            store.Append("summary:sess-done", 0, "summary", "[summary of session sess-done]\nx", 4);
            for (int i = 0; i < 4; i++)
            {
                store.Append("sess-done", i, "user", $"old q {i}", 4);
                store.Append("sess-done", i, "assistant", $"old a {i}", 4);
            }
        }

        var sleep = new SleepPhase(StorePath(), () => new ScriptedSession());
        GhostSnapshot snap = BlobStore.Snapshot(StorePath());
        var due = sleep.SessionsNeedingSummary(snap);

        Assert.Contains("sess-long", due);
        Assert.DoesNotContain("sess-short", due);
        Assert.DoesNotContain("sess-done", due);          // already summarized
        Assert.DoesNotContain(due, sid => sid.StartsWith("doc:"));
    }

    // ─────────────── the cycle ───────────────

    [Fact]
    public void Sleep_Summarizes_StoresWithProvenance_AndScreensWithTaste()
    {
        SeedSession("sess-a", 4);
        var session = new ScriptedSession(
            "The user planned a lighthouse expedition; the beacon frequency was set to nine megahertz and the departure fixed for friday.");
        var sleep = new SleepPhase(StorePath(), () => session);

        SleepReport? report = sleep.Run(null, null, null);

        Assert.NotNull(report);
        Assert.Equal(1, report!.SessionsSummarized);
        Assert.Equal(0, report.SummariesRejected);

        using var store = BlobStore.Open(StorePath());
        MemoryBlob summary = store.All().Single(b => b.Role == "summary");
        Assert.Equal("summary:sess-a", summary.SessionId);
        Assert.StartsWith("[summary of session sess-a]", summary.Text);
        Assert.Contains("nine megahertz", summary.Text);

        // Summaries are recallable material.
        Assert.Contains(store.Recall("lighthouse expedition beacon", 5),
                        b => b.Role == "summary");
    }

    [Fact]
    public void Sleep_RejectsBadSummaries_RatherThanStoreThem()
    {
        SeedSession("sess-a", 4);
        string degenerate = string.Concat(
            Enumerable.Repeat("\"flags\": false, \"flags\": false, ", 30));
        var sleep = new SleepPhase(StorePath(), () => new ScriptedSession(degenerate))
        {
            Judge = new AdaptiveJudge(_dir),
        };

        SleepReport? report = sleep.Run(null, null, null);

        Assert.Equal(0, report!.SessionsSummarized);
        Assert.Equal(1, report.SummariesRejected);
        using var store = BlobStore.Open(StorePath());
        Assert.DoesNotContain(store.All(), b => b.Role == "summary");
    }

    [Fact]
    public void Sleep_Yields_WhenAnySessionIsAwake()
    {
        SeedSession("sess-a", 4);
        using var awake = BlobStore.Open(StorePath());    // live session holds the lock

        var sleep = new SleepPhase(StorePath(), () => new ScriptedSession("x"));
        Assert.Null(sleep.Run(null, null, null));         // yields untouched
    }

    [Fact]
    public void Sleep_IsIdempotent_OverSummaries()
    {
        SeedSession("sess-a", 4);
        var sleep = new SleepPhase(StorePath(),
            () => new ScriptedSession("A durable summary of the expedition and its dates."));
        sleep.Run(null, null, null);

        var again = new SleepPhase(StorePath(),
            () => new ScriptedSession("should never be asked"));
        SleepReport? second = again.Run(null, null, null);

        Assert.Equal(0, second!.SessionsSummarized);      // nothing due anymore
    }

    // ─────────────── governed trigger ───────────────

    [Fact]
    public void Policy_SleepsOnlyWhenIdle_AndOnlyForUnsleptMaterial()
    {
        var t0 = new DateTime(2026, 7, 23, 12, 0, 0, DateTimeKind.Utc);
        static Observations Obs(DateTime now, long maxId) =>
            new(now, maxId, 10, 0, false, 0, 0, 0, 0, true, false);
        var config = new OrchestratorConfig { SleepIdleMinutes = 30 };

        // Growing store: not idle.
        var state = new OrchestratorState { LastSeenMaxId = 50, LastGrowthUtc = t0 };
        Assert.DoesNotContain(GhostPolicies.Plan(Obs(t0.AddMinutes(5), 50), state, config),
            a => a.Name == "sleep");

        // Idle long enough, unslept material: sleep.
        Assert.Contains(GhostPolicies.Plan(Obs(t0.AddMinutes(45), 50), state, config),
            a => a.Name == "sleep");

        // Already slept for this material: rest is done.
        state.SleptForMaxId = 50;
        Assert.DoesNotContain(GhostPolicies.Plan(Obs(t0.AddMinutes(45), 50), state, config),
            a => a.Name == "sleep");
    }
}
