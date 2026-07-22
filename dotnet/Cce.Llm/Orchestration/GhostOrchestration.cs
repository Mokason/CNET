using System.Text.Json;

namespace CNET.Cce.Llm.Orchestration;

/// <summary>
/// Everything the orchestrator can see of the world at one instant. Policies
/// decide from THIS — never from remembered events — so a restart, a missed
/// notification, or a crash mid-action changes nothing: the next tick
/// re-derives the same conclusions from the same observable state.
/// </summary>
public sealed record Observations(
    DateTime NowUtc,
    long StoreMaxSeenId,
    int StoreBlobs,
    int StoreCorrupt,
    bool InboxExists,
    int InboxLines,
    double InboxAgeSeconds,
    int LedgerGhostGaps,
    int LedgerGhostWaiting,
    bool DrainerActive,
    bool TeacherLaneActive);

/// <summary>Tunables and permissions. Actions that cost real resources are
/// opt-in — self-governance means governed, not unbounded.</summary>
public sealed class OrchestratorConfig
{
    /// <summary>Consolidate when this many new blob ids exist past the watermark.</summary>
    public int MinNewBlobs { get; init; } = 6;

    /// <summary>An inbox older than this with an active drainer is stuck.</summary>
    public double InboxStuckSeconds { get; init; } = 300;

    /// <summary>Base cooldown between runs of the same action.</summary>
    public double CooldownSeconds { get; init; } = 600;

    /// <summary>Advisories repeat much slower than actions.</summary>
    public double AdvisoryCooldownSeconds { get; init; } = 1800;

    /// <summary>Executed (non-advisory) actions per tick, at most.</summary>
    public int MaxActionsPerTick { get; init; } = 3;

    /// <summary>Permission to start the teacher lane service (loads a large model).</summary>
    public bool AllowTeacherStart { get; init; }
}

/// <summary>Planned by policy; the reconciler decides whether it may run now.</summary>
/// <param name="Name">Action identifier — the executor registry key.</param>
/// <param name="Reason">The evidence this decision was made from.</param>
/// <param name="JournalOnly">Advisory — recorded with its reason, never executed.</param>
public sealed record PlannedAction(string Name, string Reason, bool JournalOnly = false);

/// <summary>
/// Durable orchestrator memory: watermarks, last-run times, failure counts.
/// Deliberately tiny — anything re-derivable from the world must be re-derived
/// (that is the whole reconcile stance), so only irreversible knowledge lives
/// here.
/// </summary>
public sealed class OrchestratorState
{
    public long LastConsolidatedMaxId { get; set; }
    public Dictionary<string, DateTime> LastRunUtc { get; set; } = [];
    public Dictionary<string, int> ConsecutiveFailures { get; set; } = [];

    public static OrchestratorState Load(string path)
    {
        if (!File.Exists(path)) return new OrchestratorState();
        try
        {
            return JsonSerializer.Deserialize<OrchestratorState>(File.ReadAllText(path))
                   ?? new OrchestratorState();
        }
        catch (JsonException)
        {
            return new OrchestratorState();   // corrupt state = start conservative, never crash
        }
    }

    public void Save(string path)
    {
        string tmp = path + ".tmp";
        File.WriteAllText(tmp, JsonSerializer.Serialize(this,
            new JsonSerializerOptions { WriteIndented = true }));
        File.Move(tmp, path, overwrite: true);
    }
}

/// <summary>
/// The decision layer: pure function of (observations, state, config).
/// No IO, no clock reads, no side effects — every rule is unit-testable and
/// every decision it emits carries the evidence it was made from.
/// </summary>
public static class GhostPolicies
{
    public static List<PlannedAction> Plan(Observations o, OrchestratorState s, OrchestratorConfig c)
    {
        var plan = new List<PlannedAction>();

        // New episodic memory past the watermark → consolidate it toward weights.
        long newBlobs = o.StoreMaxSeenId - s.LastConsolidatedMaxId;
        if (newBlobs >= c.MinNewBlobs)
            plan.Add(new PlannedAction("consolidate",
                $"{newBlobs} new blobs since watermark #{s.LastConsolidatedMaxId}"));

        // Teachables parked with nobody to teach them.
        if (o.LedgerGhostWaiting > 0 && !o.TeacherLaneActive)
            plan.Add(c.AllowTeacherStart
                ? new PlannedAction("start-teacher-lane",
                    $"{o.LedgerGhostWaiting} ghost gaps waiting_oracle, teacher lane inactive")
                : new PlannedAction("recommend-teacher-lane",
                    $"{o.LedgerGhostWaiting} ghost gaps waiting_oracle; teacher start not permitted " +
                    "(run with --allow-teacher-start to let the orchestrator start it)",
                    JournalOnly: true));

        // Work emitted into a void, or a drainer that stopped draining.
        if (o.InboxExists && !o.DrainerActive && !o.TeacherLaneActive)
            plan.Add(new PlannedAction("warn-no-drainer",
                $"inbox holds {o.InboxLines} lines and no lane daemon is active",
                JournalOnly: true));
        else if (o.InboxExists && o.InboxAgeSeconds > c.InboxStuckSeconds)
            plan.Add(new PlannedAction("warn-inbox-stuck",
                $"inbox is {o.InboxAgeSeconds:F0}s old while a drainer is active — it should have been consumed",
                JournalOnly: true));

        if (o.StoreCorrupt > 0)
            plan.Add(new PlannedAction("warn-store-corrupt",
                $"{o.StoreCorrupt} corrupt lines in the ghost store (a past crash truncated a write)",
                JournalOnly: true));

        return plan;
    }
}

/// <summary>One journaled decision — the orchestrator's receipts.</summary>
public sealed record Decision(
    string TimestampUtc, string Action, string Reason,
    bool Executed, bool DryRun, string Outcome);

/// <summary>Append-only JSONL journal of every decision, with reasons.</summary>
public sealed class DecisionJournal(string path)
{
    private static readonly JsonSerializerOptions Options = new()
    {
        Encoder = System.Text.Encodings.Web.JavaScriptEncoder.UnsafeRelaxedJsonEscaping,
    };

    public void Record(Decision decision)
    {
        string? dir = System.IO.Path.GetDirectoryName(System.IO.Path.GetFullPath(path));
        if (!string.IsNullOrEmpty(dir)) Directory.CreateDirectory(dir);
        File.AppendAllText(path, JsonSerializer.Serialize(decision, Options) + "\n");
    }
}

/// <summary>
/// Plans, gates, executes, journals — one tick of self-governance. Executors
/// are injected by name so the loop's discipline (cooldowns, exponential
/// backoff on failure, per-tick budget, dry-run, receipts) is identical for
/// every action and testable without touching the real world.
/// </summary>
public sealed class Reconciler(
    OrchestratorConfig config,
    OrchestratorState state,
    DecisionJournal journal,
    IReadOnlyDictionary<string, Func<PlannedAction, OrchestratorState, string>> executors)
{
    /// <summary>Runs one reconcile pass. Returns the decisions taken.</summary>
    public List<Decision> Tick(Observations obs, bool dryRun = false)
    {
        var decisions = new List<Decision>();
        int executed = 0;

        foreach (PlannedAction action in GhostPolicies.Plan(obs, state, config))
        {
            double baseCooldown = action.JournalOnly
                ? config.AdvisoryCooldownSeconds : config.CooldownSeconds;
            int failures = state.ConsecutiveFailures.GetValueOrDefault(action.Name);
            // Exponential backoff: a repeatedly failing action must not flap.
            double cooldown = baseCooldown * Math.Pow(2, Math.Min(failures, 6));

            if (state.LastRunUtc.TryGetValue(action.Name, out DateTime last) &&
                (obs.NowUtc - last).TotalSeconds < cooldown)
                continue;   // within cooldown — silence, not a journal entry per tick

            if (action.JournalOnly)
            {
                state.LastRunUtc[action.Name] = obs.NowUtc;
                decisions.Add(Record(obs, action, executed: false, dryRun, "advisory"));
                continue;
            }

            if (executed >= config.MaxActionsPerTick)
            {
                decisions.Add(Record(obs, action, executed: false, dryRun,
                    "deferred: per-tick action budget exhausted"));
                continue;
            }

            if (dryRun)
            {
                decisions.Add(Record(obs, action, executed: false, dryRun, "dry run — would execute"));
                continue;
            }

            if (!executors.TryGetValue(action.Name, out var executor))
            {
                decisions.Add(Record(obs, action, executed: false, dryRun,
                    "no executor registered for this action"));
                continue;
            }

            state.LastRunUtc[action.Name] = obs.NowUtc;
            try
            {
                string outcome = executor(action, state);
                state.ConsecutiveFailures.Remove(action.Name);
                executed++;
                decisions.Add(Record(obs, action, executed: true, dryRun, outcome));
            }
            catch (Exception ex)
            {
                state.ConsecutiveFailures[action.Name] = failures + 1;
                decisions.Add(Record(obs, action, executed: false, dryRun,
                    $"FAILED ({failures + 1} consecutive): {ex.Message}"));
            }
        }

        return decisions;
    }

    private Decision Record(Observations obs, PlannedAction action,
                            bool executed, bool dryRun, string outcome)
    {
        var decision = new Decision(obs.NowUtc.ToString("o"), action.Name, action.Reason,
                                    executed, dryRun, outcome);
        journal.Record(decision);
        return decision;
    }
}
