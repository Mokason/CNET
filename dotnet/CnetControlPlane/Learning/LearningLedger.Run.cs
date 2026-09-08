using Microsoft.Data.Sqlite;

namespace CnetControlPlane.Learning;

internal sealed record LearningRun(string Boot, long StartNanoseconds, long LastNanoseconds,
    long TickCount, string State, string LastAction);

internal sealed partial class LearningLedger
{
    private static readonly string[] RunTickCodes = ["idle", "activated", "probation", "probe_too_soon", "accepted",
        "rolled_back", "discarded", "interrupted", "paused", "frozen", "stage_refused", "refused", "outcome_unknown",
        "acquisition_refused", "evidence_unavailable", "verified_mismatch", "learning_paused", "job_capacity_exhausted",
        "source_attempts_exhausted", "source_already_pending", "attempt_budget_exhausted", "approved_source_changed"];
    private static readonly string[] RunStopCodes = ["operator_stop", "cancelled", "tick_failed"];
    private static readonly string[] RunFailureCodes = ["run_boot_changed", "run_clock_invalid", "run_clock_rollback",
        "run_gap_exceeded", "run_paused", "run_tick_count_exhausted"];

    private LearningRun? ReadRun(SqliteTransaction tx)
    {
        using var command = Command("SELECT boot,start_ns,last_ns,tick_count,state,last_action FROM learning_run WHERE id=1", tx);
        using var reader = command.ExecuteReader();
        if (!reader.Read()) throw new InvalidOperationException("learning_run_integrity");
        var boot = reader.GetValue(0); var start = reader.GetValue(1); var last = reader.GetValue(2);
        var ticks = reader.GetValue(3); var state = reader.GetValue(4); var action = reader.GetValue(5);
        if (reader.Read()) throw new InvalidOperationException("learning_run_integrity");
        reader.Close();
        if (Equals(state, "not_started"))
        {
            if (boot is not DBNull || start is not DBNull || last is not DBNull || !Equals(ticks, 0L) || !Equals(action, "not_started"))
                throw new InvalidOperationException("learning_run_integrity");
            return null;
        }
        if (boot is not string bootId || !Guid.TryParseExact(bootId, "D", out _) || start is not long startNs
            || last is not long lastNs || startNs < 0 || lastNs < startNs || ticks is not long tickCount || tickCount < 0
            || state is not string runState || action is not string lastAction
            || !(runState == "running" && (lastAction == "run_started" || RunTickCodes.Contains(lastAction))
                || runState == "failed" && (RunStopCodes.Contains(lastAction) || RunFailureCodes.Contains(lastAction))
                || runState == "budget_complete" && lastAction == "budget_complete"))
            throw new InvalidOperationException("learning_run_integrity");
        var epoch = Epoch(bootId, tx);
        if (startNs < epoch.First || lastNs > epoch.Last
            || runState == "budget_complete" && lastNs - startNs < policy.MaxRunSeconds * 1_000_000_000L)
            throw new InvalidOperationException("learning_run_integrity");
        return new(bootId, startNs, lastNs, tickCount, runState, lastAction);
    }

    public LearningRun? RunStatus
    {
        get
        {
            files.AssertPathIdentity();
            using var tx = db.BeginTransaction(deferred: true);
            RequireSchemaVersion(tx);
            var run = ReadRun(tx); tx.Commit(); return run;
        }
    }

    private LearningRun FailRun(LearningRun run, string code, SqliteTransaction tx)
    {
        if (Execute("UPDATE learning_run SET state='failed',last_action=$code WHERE id=1 AND state='running'", tx,
            ("$code", code)) != 1) throw new InvalidOperationException("learning_run_integrity");
        SetPaused(tx);
        return run with { State = "failed", LastAction = code };
    }

    // Transaction validated schema, epochs, charges and this row before the
    // clock read. Persist failure without ever advancing time from that clock.
    // Commit errors propagate: a failed commit is NOT evidence of a durable pause.
    private void CommitRunClockFailure(LearningRun? run, string code, SqliteTransaction tx)
    {
        if (run?.State != "running") return;
        _ = FailRun(run, code, tx);
        tx.Commit();
    }

    private LearningRun ObserveRunTime(LearningRun run, LearningInstant now, SqliteTransaction tx)
    {
        if (now.Boot != run.Boot) return FailRun(run, "run_boot_changed", tx);
        if (now.Nanoseconds < run.LastNanoseconds) return FailRun(run, "run_clock_rollback", tx);
        if (now.Nanoseconds - run.LastNanoseconds > policy.MaxProbeGapSeconds * 1_000_000_000L)
            return FailRun(run, "run_gap_exceeded", tx);
        // A durable pause wins over a live heartbeat or natural completion,
        // just as it does over restart. Never silently clear it to finish.
        if (Convert.ToInt64(Scalar("SELECT paused FROM configuration WHERE id=1", tx)) != 0)
            return FailRun(run, "run_paused", tx);
        return run;
    }

    public LearningRun BeginOrResumeRun()
    {
        policy.RequireEnabled();
        return Transaction((now, tx) =>
        {
            var run = ReadRun(tx);
            if (run is not null)
            {
                if (run.State != "running") return run; // Terminal is not success or a new run.
                // A restart is not a heartbeat and cannot refresh last_ns.
                return ObserveRunTime(run, now, tx);
            }
            MayPromote(tx);
            if (Execute("""
                UPDATE learning_run SET boot=$boot,start_ns=$now,last_ns=$now,state='running',last_action='run_started'
                WHERE id=1 AND state='not_started'
                """, tx, ("$boot", now.Boot), ("$now", now.Nanoseconds)) != 1)
                throw new InvalidOperationException("learning_run_integrity");
            return new LearningRun(now.Boot, now.Nanoseconds, now.Nanoseconds, 0, "running", "run_started");
        });
    }

    // Trusted supervisor boundary: called after an actual tick. This records an
    // observation, including refusal/idle, never an assertion of learning gain.
    public LearningRun RecordRunTick(string code)
    {
        if (!RunTickCodes.Contains(code)) throw new ArgumentException("learning_run_tick_code");
        return Transaction((now, tx) =>
        {
            var run = ReadRun(tx) ?? throw new InvalidOperationException("learning_run_not_started");
            if (run.State != "running") throw new InvalidOperationException("learning_run_not_running");
            run = ObserveRunTime(run, now, tx);
            if (run.State != "running") return run;
            if (run.TickCount == long.MaxValue) return FailRun(run, "run_tick_count_exhausted", tx);
            var ticks = checked(run.TickCount + 1);
            if (Execute("UPDATE learning_run SET last_ns=$now,tick_count=$ticks,last_action=$code WHERE id=1 AND state='running'", tx,
                ("$now", now.Nanoseconds), ("$ticks", ticks), ("$code", code)) != 1)
                throw new InvalidOperationException("learning_run_integrity");
            return run with { LastNanoseconds = now.Nanoseconds, TickCount = ticks, LastAction = code };
        });
    }

    // This is elapsed run-budget accounting, NOT a 72-hour acceptance result.
    public LearningRun FinishRun() => Transaction((now, tx) =>
    {
        var run = ReadRun(tx) ?? throw new InvalidOperationException("learning_run_not_started");
        if (run.State != "running") return run;
        run = ObserveRunTime(run, now, tx);
        if (run.State != "running") return run;
        if (now.Nanoseconds - run.StartNanoseconds < policy.MaxRunSeconds * 1_000_000_000L)
            throw new InvalidOperationException("learning_run_budget_not_elapsed");
        if (Convert.ToInt64(Scalar("SELECT EXISTS(SELECT 1 FROM intents WHERE state='pending') OR EXISTS(SELECT 1 FROM probation)", tx)) != 0)
            throw new InvalidOperationException("learning_run_native_unsettled");
        if (Execute("UPDATE learning_run SET last_ns=$now,state='budget_complete',last_action='budget_complete' WHERE id=1 AND state='running'", tx,
            ("$now", now.Nanoseconds)) != 1) throw new InvalidOperationException("learning_run_integrity");
        SetPaused(tx);
        return run with { LastNanoseconds = now.Nanoseconds, State = "budget_complete", LastAction = "budget_complete" };
    });

    // Stopping needs no new clock authority and cannot invent elapsed time.
    public LearningRun? StopRun(string code)
    {
        if (!RunStopCodes.Contains(code)) throw new ArgumentException("learning_run_stop_code");
        files.AssertPathIdentity();
        using var tx = db.BeginTransaction(deferred: false);
        RequireSchemaVersion(tx);
        if (Scalar("SELECT boot FROM configuration WHERE id=1", tx) is not string known || !Guid.TryParseExact(known, "D", out _))
            throw new InvalidOperationException("learning_ledger_epoch_integrity");
        _ = Epoch(known, tx); RequirePromotionIntegrity(tx);
        var run = ReadRun(tx);
        if (run is null) SetPaused(tx);
        else if (run.State == "running") run = FailRun(run, code, tx);
        tx.Commit(); return run;
    }
}
