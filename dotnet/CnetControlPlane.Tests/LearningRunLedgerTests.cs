using System.Globalization;
using System.Runtime.Versioning;
using System.Text;
using CnetControlPlane.Learning;
using Microsoft.Data.Sqlite;
using Xunit;

namespace CnetControlPlane.Tests;

[SupportedOSPlatform("linux")]
public sealed class LearningRunLedgerTests : IDisposable
{
    private readonly string root = Directory.CreateTempSubdirectory("cnet-run-ledger-").FullName;
    private sealed class Clock : ILearningClock
    {
        public LearningInstant Value = new("00000000-0000-0000-0000-000000000001", 100_000_000_000);
        public bool Unavailable;
        public LearningInstant Now => Unavailable ? throw new InvalidOperationException("learning_boot_clock_unavailable") : Value;
        public void Advance(long seconds) => Value = Value with { Nanoseconds = Value.Nanoseconds + seconds * 1_000_000_000 };
    }
    private readonly Clock clock = new();
    private static readonly LearningPolicy Policy = LearningPolicy.Parse(Encoding.UTF8.GetBytes(
        LearningPolicyTests.Valid.Replace("\"max_run_seconds\":259200", "\"max_run_seconds\":120")));
    private static string H(int value) => value.ToString("x64", CultureInfo.InvariantCulture);
    public LearningRunLedgerTests() => File.SetUnixFileMode(root, UnixFileMode.UserRead | UnixFileMode.UserWrite | UnixFileMode.UserExecute);
    public void Dispose() => Directory.Delete(root, true);
    private object? Sql(string sql)
    {
        using var db = new SqliteConnection(new SqliteConnectionStringBuilder
        { DataSource = Path.Combine(root, "ledger.sqlite"), Mode = SqliteOpenMode.ReadWrite, Pooling = false }.ToString());
        db.Open(); using var command = db.CreateCommand(); command.CommandText = sql; return command.ExecuteScalar();
    }

    [Fact]
    public void CreateTimeDoesNotCountAndRepeatedStartsNeverRefreshTheOriginalBudgetOrHeartbeat()
    {
        LearningRun original;
        using (var ledger = LearningLedger.Create(root, Policy, clock))
        {
            Assert.Null(ledger.RunStatus);
            clock.Advance(60);
            original = ledger.BeginOrResumeRun();
            Assert.Equal(160_000_000_000, original.StartNanoseconds);
            Assert.Equal(original.StartNanoseconds, original.LastNanoseconds);
            Assert.Equal(0, original.TickCount);
            Assert.Equal("running", original.State);
            clock.Advance(30);
            Assert.Equal(original, ledger.BeginOrResumeRun());
            Assert.Equal(1, ledger.RecordRunTick("idle").TickCount);
        }
        clock.Advance(30);
        using var reopened = LearningLedger.Open(root, Policy, clock);
        var resumed = reopened.BeginOrResumeRun();
        Assert.Equal(original.StartNanoseconds, resumed.StartNanoseconds);
        Assert.Equal(190_000_000_000, resumed.LastNanoseconds);
        clock.Advance(60);
        var completed = reopened.FinishRun();
        Assert.Equal("budget_complete", completed.State);
        Assert.Equal(120_000_000_000, completed.LastNanoseconds - completed.StartNanoseconds);
        Assert.Equal(1, completed.TickCount); // Completion is not an invented tick.
        Assert.True(reopened.IsPaused);
        Assert.Equal(completed, reopened.BeginOrResumeRun());
        Assert.Equal(completed, reopened.FinishRun());
    }

    [Fact]
    public void PrematureFinishDoesNotConvertAHeartbeatIntoBudgetCompletion()
    {
        using var ledger = LearningLedger.Create(root, Policy, clock);
        ledger.BeginOrResumeRun(); clock.Advance(30);
        var heartbeat = ledger.RecordRunTick("evidence_unavailable");
        Assert.Equal("learning_run_budget_not_elapsed", Assert.Throws<InvalidOperationException>(() => ledger.FinishRun()).Message);
        Assert.Equal(heartbeat, ledger.RunStatus);
        Assert.False(ledger.IsPaused);
    }

    [Theory]
    [InlineData(false, false)]
    [InlineData(false, true)]
    [InlineData(true, false)]
    [InlineData(true, true)]
    public void RebootOrMissedHeartbeatFailsDurablyWithoutRestarting(bool restart, bool reboot)
    {
        var ledger = LearningLedger.Create(root, Policy, clock);
        var original = ledger.BeginOrResumeRun();
        if (restart) ledger.Dispose();
        if (reboot) clock.Value = new("00000000-0000-0000-0000-000000000002", 1);
        else clock.Advance(121);
        if (restart) ledger = LearningLedger.Open(root, Policy, clock);
        using (ledger)
        {
            var failed = restart ? ledger.BeginOrResumeRun() : ledger.RecordRunTick("idle");
            Assert.Equal(original with { State = "failed", LastAction = reboot ? "run_boot_changed" : "run_gap_exceeded" }, failed);
            Assert.True(ledger.IsPaused);
            Assert.Equal(failed, ledger.BeginOrResumeRun());
            Assert.Equal(failed, ledger.FinishRun());
            Assert.Equal("learning_paused", ledger.Reserve("calibration", H(3)).Reason);
        }
        using var reopened = LearningLedger.Open(root, Policy, clock);
        Assert.Equal("failed", reopened.RunStatus!.State);
        Assert.True(reopened.IsPaused);
    }

    [Theory]
    [InlineData(false, "boot")]
    [InlineData(false, "negative")]
    [InlineData(false, "rollback")]
    [InlineData(false, "unavailable")]
    [InlineData(true, "boot")]
    [InlineData(true, "negative")]
    [InlineData(true, "rollback")]
    [InlineData(true, "unavailable")]
    public void BadClockIncludingOpenPersistsFailureBeforeRefusingWithoutAdvancingEpoch(bool restart, string damage)
    {
        var ledger = LearningLedger.Create(root, Policy, clock);
        var original = ledger.BeginOrResumeRun();
        if (restart) ledger.Dispose();
        switch (damage)
        {
            case "boot": clock.Value = clock.Value with { Boot = "invalid" }; break;
            case "negative": clock.Value = clock.Value with { Nanoseconds = -1 }; break;
            case "rollback": clock.Advance(-1); break;
            case "unavailable": clock.Unavailable = true; break;
        }
        try
        {
            Assert.Throws<InvalidOperationException>(() =>
            {
                if (restart) { using var refused = LearningLedger.Open(root, Policy, clock); }
                else ledger.RecordRunTick("idle");
            });
        }
        finally { if (!restart) ledger.Dispose(); }
        Assert.Equal(100_000_000_000L, Sql("SELECT last_ns FROM epochs"));
        clock.Unavailable = false;
        clock.Value = new(original.Boot, original.LastNanoseconds);
        using var reopened = LearningLedger.Open(root, Policy, clock);
        Assert.Equal(original with { State = "failed", LastAction = damage == "rollback" ? "run_clock_rollback" : "run_clock_invalid" }, reopened.RunStatus);
        Assert.True(reopened.IsPaused);
    }

    [Fact]
    public void MissingKnownEpochRefusesBeforeAClockFailureCanCommitOverCorruptAccounting()
    {
        using var ledger = LearningLedger.Create(root, Policy, clock);
        var original = ledger.BeginOrResumeRun();
        Sql("PRAGMA foreign_keys=OFF; DELETE FROM epochs");
        clock.Value = clock.Value with { Nanoseconds = -1 };
        Assert.Equal("learning_ledger_epoch_integrity", Assert.Throws<InvalidOperationException>(() => ledger.RecordRunTick("idle")).Message);
        Assert.Equal("running", Sql("SELECT state FROM learning_run"));
        Assert.Equal(0L, Sql("SELECT paused FROM configuration"));
    }

    [Fact]
    public void FailureMarkCommitFailureThrowsAndMakesNoDurabilityClaim()
    {
        using var ledger = LearningLedger.Create(root, Policy, clock);
        var original = ledger.BeginOrResumeRun();
        // Explicit test-only failure injection: the failure UPDATE triggers a
        // deferred FK violation, so the actual COMMIT (not just UPDATE) fails.
        Sql("""
            CREATE TABLE injected_commit_guard(boot TEXT REFERENCES epochs(boot) DEFERRABLE INITIALLY DEFERRED) STRICT;
            CREATE TRIGGER injected_failed_run_commit AFTER UPDATE OF state ON learning_run
                WHEN NEW.state='failed' BEGIN INSERT INTO injected_commit_guard VALUES('missing_epoch'); END;
            """);
        clock.Value = clock.Value with { Nanoseconds = -1 };
        Assert.Throws<SqliteException>(() => ledger.RecordRunTick("idle"));
        Assert.Equal(original, ledger.RunStatus); // The failed transaction rolled back.
        Assert.False(ledger.IsPaused); // Must not claim the pause became durable.
        Assert.Equal(0L, Sql("SELECT count(*) FROM injected_commit_guard"));
        Assert.Equal(100_000_000_000L, Sql("SELECT last_ns FROM epochs"));
    }

    [Fact]
    public void FuturePersistedHeartbeatCannotBeHealedByAdvancingTheEpoch()
    {
        using var ledger = LearningLedger.Create(root, Policy, clock);
        ledger.BeginOrResumeRun();
        Sql("UPDATE learning_run SET last_ns=101000000000");
        clock.Advance(2);
        Assert.Equal("learning_run_integrity", Assert.Throws<InvalidOperationException>(() => ledger.BeginOrResumeRun()).Message);
        Assert.Equal(100_000_000_000L, Sql("SELECT last_ns FROM epochs"));
    }

    [Fact]
    public void BadClockBeforeAnyRunDoesNotInventAFailedRunOrDurablePause()
    {
        using var ledger = LearningLedger.Create(root, Policy, clock);
        clock.Value = clock.Value with { Nanoseconds = -1 };
        Assert.Equal("learning_clock_invalid", Assert.Throws<InvalidOperationException>(
            () => ledger.Reserve("calibration", H(2))).Message);
        Assert.Null(ledger.RunStatus);
        Assert.False(ledger.IsPaused);
        Assert.Equal(100_000_000_000L, Sql("SELECT last_ns FROM epochs"));
    }

    [Fact]
    public void MissingPromotionChargeRefusesBeforeClockFailureCanBeCommitted()
    {
        using var ledger = LearningLedger.Create(root, Policy, clock);
        var original = ledger.BeginOrResumeRun();
        Activation(ledger, false);
        Sql("DELETE FROM promotion_charges");
        clock.Value = clock.Value with { Nanoseconds = -1 };
        Assert.Equal("learning_ledger_promotion_integrity", Assert.Throws<InvalidOperationException>(
            () => ledger.RecordRunTick("idle")).Message);
        Assert.Equal(original, ledger.RunStatus);
        Assert.False(ledger.IsPaused);
    }

    [Theory]
    [InlineData("")]
    [InlineData("unregistered_code")]
    [InlineData("idle\nsource contents")]
    public void OnlyFixedTickAndStopCodesAreAcceptedBeforeAnyMutation(string code)
    {
        using var ledger = LearningLedger.Create(root, Policy, clock);
        var original = ledger.BeginOrResumeRun(); clock.Advance(1);
        Assert.Equal("learning_run_tick_code", Assert.Throws<ArgumentException>(() => ledger.RecordRunTick(code)).Message);
        Assert.Equal("learning_run_stop_code", Assert.Throws<ArgumentException>(() => ledger.StopRun(code)).Message);
        Assert.Equal(original, ledger.RunStatus);
        Assert.False(ledger.IsPaused);
    }

    [Theory]
    [InlineData("idle")]
    [InlineData("evidence_unavailable")]
    [InlineData("learning_paused")]
    [InlineData("job_capacity_exhausted")]
    [InlineData("source_attempts_exhausted")]
    [InlineData("source_already_pending")]
    [InlineData("attempt_budget_exhausted")]
    public void RefusedAndUnavailableTicksAreCountedAsObservationsNotLearningSuccess(string code)
    {
        using var ledger = LearningLedger.Create(root, Policy, clock);
        ledger.BeginOrResumeRun(); clock.Advance(30);
        var tick = ledger.RecordRunTick(code);
        Assert.Equal(1, tick.TickCount);
        Assert.Equal(130_000_000_000, tick.LastNanoseconds);
        Assert.Equal("running", tick.State);
        Assert.Equal(code, tick.LastAction);
    }

    [Theory]
    [InlineData("operator_stop")]
    [InlineData("cancelled")]
    [InlineData("tick_failed")]
    public void ExplicitStopCannotBeAutomaticallyRestarted(string code)
    {
        using var ledger = LearningLedger.Create(root, Policy, clock);
        var original = ledger.BeginOrResumeRun();
        var failed = ledger.StopRun(code);
        Assert.Equal(original with { State = "failed", LastAction = code }, failed);
        Assert.True(ledger.IsPaused);
        Assert.Equal(failed, ledger.BeginOrResumeRun());
        Assert.Equal(failed, ledger.FinishRun());
        Assert.Throws<InvalidOperationException>(() => ledger.RecordRunTick("idle"));
    }

    private static LearningIntent Activation(LearningLedger ledger, bool apply)
    {
        // Exact finite parser fixture only, not native-execution evidence.
        var source = LocalTableReference.Parse(Encoding.ASCII.GetBytes(
            "CNET_LOCAL_TABLE_V1\ndataset calibration\nauthority verified_tool\ninput_bits 8\noutput_bits 16\nrows 1\n7\t2\n"), new("calibration", "verified_tool"));
        var before = new ControlStatus(true, 1, H(1), null, null, true, ControlReason.Ok);
        ledger.BindNative(before);
        var job = ledger.Reserve("calibration", source.SourceSha256).Job!;
        var stage = ledger.BeginStage(job.Id, "candidate", H(3), before);
        Assert.Equal("applied", ledger.ResolveIntent(stage.Id, stage.Expected));
        var output = new StringBuilder("CNET_TABLE_SNAPSHOT_EVAL_V1\nsnapshot_sha256 " + H(3) +
            "\ndataset calibration\nsource_sha256 " + source.SourceSha256 + "\nresults 256\n");
        for (var key = 0; key < 256; key++) output.Append(key.ToString(CultureInfo.InvariantCulture)).Append(key == 7 ? "\t1\t2\n" : "\t0\t-\n");
        ledger.RecordEvaluation(job.Id, LearningTableEvaluation.Parse(Encoding.ASCII.GetBytes(output.Append("end\n").ToString()), source, H(3)));
        var activation = ledger.BeginActivate(job.Id, stage.Expected);
        if (apply) Assert.Equal("applied", ledger.ResolveIntent(activation.Id, activation.Expected));
        return activation;
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public void ElapsedBudgetCannotFinishWithPendingIntentOrActiveProbation(bool probation)
    {
        using var ledger = LearningLedger.Create(root, Policy, clock);
        var original = ledger.BeginOrResumeRun();
        Activation(ledger, probation); clock.Advance(120);
        Assert.Equal("learning_run_native_unsettled", Assert.Throws<InvalidOperationException>(() => ledger.FinishRun()).Message);
        Assert.Equal(original, ledger.RunStatus);
        Assert.Equal(1L, Sql("SELECT count(*) FROM promotion_charges"));
    }

    [Fact]
    public void StopPreservesChargesAndUnresolvedNativeIntent()
    {
        using var ledger = LearningLedger.Create(root, Policy, clock);
        ledger.BeginOrResumeRun();
        var intent = Activation(ledger, false);
        Assert.Equal("failed", ledger.StopRun("cancelled")!.State);
        Assert.Equal(intent, ledger.PendingIntent);
        Assert.Equal(1L, Sql("SELECT count(*) FROM promotion_charges"));
    }

    [Fact]
    public void TickCounterCannotWrapAndADeletedSingletonCannotResetTheRunBudget()
    {
        using var ledger = LearningLedger.Create(root, Policy, clock);
        ledger.BeginOrResumeRun();
        Sql("UPDATE learning_run SET tick_count=9223372036854775807");
        var failed = ledger.RecordRunTick("idle");
        Assert.Equal("failed", failed.State);
        Assert.Equal("run_tick_count_exhausted", failed.LastAction);
        Assert.Equal(long.MaxValue, failed.TickCount);
        Sql("DELETE FROM learning_run");
        Assert.Equal("learning_run_integrity", Assert.Throws<InvalidOperationException>(() => ledger.BeginOrResumeRun()).Message);
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public void DurablePauseCannotBecomeAHeartbeatOrBudgetCompletion(bool finish)
    {
        using (var ledger = LearningLedger.Create(root, Policy, clock))
        {
            var original = ledger.BeginOrResumeRun();
            clock.Advance(120);
            ledger.Pause();
            var result = finish ? ledger.FinishRun() : ledger.RecordRunTick("frozen");
            Assert.True(result.State == "failed",
                "LEARNING_RUN_PAUSE_RED: durable pause became a running heartbeat or completed budget");
            Assert.Equal(original with { State = "failed", LastAction = "run_paused" }, result);
            Assert.True(ledger.IsPaused);
        }
        using var reopened = LearningLedger.Open(root, Policy, clock);
        Assert.Equal("failed", reopened.BeginOrResumeRun().State);
        Assert.Equal("run_paused", reopened.RunStatus!.LastAction);
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public void PrematureCompletedRowRefusesBeforeLaterClockCanHealIt(bool restart)
    {
        var ledger = LearningLedger.Create(root, Policy, clock);
        ledger.BeginOrResumeRun();
        if (restart) ledger.Dispose();
        Sql("UPDATE learning_run SET state='budget_complete',last_action='budget_complete'; UPDATE configuration SET paused=1");
        clock.Advance(120);
        try
        {
            var failure = Record.Exception(() =>
            {
                if (restart) { using var refused = LearningLedger.Open(root, Policy, clock); }
                else ledger.BeginOrResumeRun();
            });
            Assert.True(failure is InvalidOperationException,
                "LEARNING_RUN_COMPLETION_RED: premature persisted completion was accepted");
            Assert.Equal("learning_run_integrity", failure.Message);
            Assert.Equal(100_000_000_000L, Sql("SELECT last_ns FROM epochs"));
        }
        finally { if (!restart) ledger.Dispose(); }
    }

}
