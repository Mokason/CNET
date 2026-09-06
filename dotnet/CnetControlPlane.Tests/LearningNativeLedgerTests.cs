using System.Globalization;
using System.Text;
using CnetControlPlane.Learning;
using Xunit;

namespace CnetControlPlane.Tests;

public sealed class LearningNativeLedgerTests : IDisposable
{
    private readonly string root = Directory.CreateTempSubdirectory("cnet-native-ledger-").FullName;
    private sealed class Clock : ILearningClock
    {
        public LearningInstant Now { get; set; } = new("00000000-0000-0000-0000-000000000001", 100_000_000_000);
        public void Advance(long seconds) => Now = Now with { Nanoseconds = Now.Nanoseconds + seconds * 1_000_000_000 };
    }
    private readonly Clock clock = new();
    private static LearningPolicy Policy(string? json = null) => LearningPolicy.Parse(Encoding.UTF8.GetBytes(json ?? LearningPolicyTests.Valid));
    private static string H(int n) => n.ToString("x64");
    private static LocalTableReference Reference(int value, string dataset = "calibration") => LocalTableReference.Parse(
        Encoding.ASCII.GetBytes("CNET_LOCAL_TABLE_V1\ndataset " + dataset +
            "\nauthority verified_tool\ninput_bits 8\noutput_bits 16\nrows 1\n7\t" +
            value.ToString(CultureInfo.InvariantCulture) + "\n"), new(dataset, "verified_tool"));
    private static string Source(int value) => Reference(value).SourceSha256;
    private static LearningTableEvaluation Evaluation(int source = 2, int candidate = 3)
    {
        var reference = Reference(source);
        // Unit fixture, not evidence of a native execution: source key 7 has
        // exactly this value; all other inputs must explicitly abstain.
        var output = new StringBuilder("CNET_TABLE_SNAPSHOT_EVAL_V1\nsnapshot_sha256 " + H(candidate) +
            "\ndataset calibration\nsource_sha256 " + reference.SourceSha256 + "\nresults 256\n");
        for (var key = 0; key < 256; key++)
            output.Append(key.ToString(CultureInfo.InvariantCulture)).Append(key == 7
                ? "\t1\t" + source.ToString(CultureInfo.InvariantCulture) + "\n" : "\t0\t-\n");
        output.Append("end\n");
        return LearningTableEvaluation.Parse(Encoding.ASCII.GetBytes(output.ToString()), reference, H(candidate));
    }
    private static ControlStatus Initial(ulong revision = 1) => new(true, revision, H(1), null, null, true, ControlReason.Ok);
    public LearningNativeLedgerTests() => File.SetUnixFileMode(root, UnixFileMode.UserRead | UnixFileMode.UserWrite | UnixFileMode.UserExecute);
    public void Dispose() => Directory.Delete(root, true);
    private (LearningJob Job, ControlStatus Staged) Stage(LearningLedger ledger, ControlStatus? before = null, int source = 2, int candidate = 3)
    {
        before ??= Initial();
        var job = ledger.Reserve("calibration", Source(source)).Job!;
        var intent = ledger.BeginStage(job.Id, "candidate_" + job.Id, H(candidate), before);
        Assert.Equal("applied", ledger.ResolveIntent(intent.Id, intent.Expected));
        return (job, intent.Expected);
    }
    private (LearningJob Job, ControlStatus Active) Activate(LearningLedger ledger, ControlStatus? before = null, int source = 2, int candidate = 3)
    {
        var (job, staged) = Stage(ledger, before, source, candidate);
        ledger.RecordEvaluation(job.Id, Evaluation(source, candidate));
        var intent = ledger.BeginActivate(job.Id, staged);
        Assert.Equal("applied", ledger.ResolveIntent(intent.Id, intent.Expected));
        return (job, intent.Expected);
    }

    [Fact]
    public void ExactIntentSurvivesCrashAndOneUnresolvedOperationBlocksAnother()
    {
        LearningIntent intent;
        using (var ledger = LearningLedger.Create(root, Policy(), clock))
        {
            ledger.BindNative(Initial());
            var job = ledger.Reserve("calibration", Source(2)).Job!;
            intent = ledger.BeginStage(job.Id, "candidate", H(3), Initial());
            Assert.Equal("STAGE 1 1 candidate\n", intent.Request);
            Assert.Equal(intent, ledger.PendingIntent);
            Assert.Throws<InvalidOperationException>(() => ledger.BeginStage(job.Id, "other", H(3), Initial()));
        }
        using var reopened = LearningLedger.Open(root, Policy(), clock);
        Assert.Equal(intent, reopened.PendingIntent);
        Assert.Equal("applied", reopened.ResolveIntent(intent.Id, intent.Expected));
        Assert.Null(reopened.PendingIntent);
        Assert.Equal("staged", reopened.JobState(intent.JobId));
        Assert.Throws<InvalidOperationException>(() => reopened.ResolveIntent(intent.Id, intent.Expected));
    }

    [Fact]
    public void EvaluationRecordingHasOnlyTheValidatedTypedBoundary()
    {
        var method = Assert.Single(typeof(LearningLedger).GetMethods(), method => method.Name == "RecordEvaluation");
        Assert.Equal(new[] { typeof(long), typeof(LearningTableEvaluation) },
            method.GetParameters().Select(parameter => parameter.ParameterType));
    }

    [Fact]
    public void EvaluationBindsExactSourceAndCandidateBeforeActivation()
    {
        using var ledger = LearningLedger.Create(root, Policy(), clock);
        ledger.BindNative(Initial());
        var (job, staged) = Stage(ledger);
        Assert.Throws<InvalidOperationException>(() => ledger.BeginActivate(job.Id, staged));
        Assert.Throws<InvalidOperationException>(() => ledger.RecordEvaluation(job.Id, Evaluation(source: 8)));
        Assert.Throws<InvalidOperationException>(() => ledger.RecordEvaluation(job.Id, Evaluation(candidate: 8)));
        Assert.Throws<ArgumentNullException>(() => ledger.RecordEvaluation(job.Id, null!));
        Assert.Equal("staged", ledger.JobState(job.Id));
        ledger.RecordEvaluation(job.Id, Evaluation());
        var intent = ledger.BeginActivate(job.Id, staged);
        Assert.StartsWith("ACTIVATE 1 ", intent.Request);
        Assert.EndsWith(" " + H(3) + "\n", intent.Request);
        Assert.Equal(2UL, intent.Expected.Revision);
        Assert.Equal(H(3), intent.Expected.Active);
        Assert.Equal(H(1), intent.Expected.Rollback);
        Assert.Null(intent.Expected.Staged);
    }

    [Fact]
    public void EvaluationCannotBeAttachedToAnotherReservedDatasetEvenWithMatchingHashes()
    {
        const string item = "{\"id\":\"calibration\",\"authority\":\"verified_tool\"}";
        var policy = Policy(LearningPolicyTests.Valid.Replace(item,
            item + ",{\"id\":\"other_table\",\"authority\":\"verified_tool\"}"));
        using var ledger = LearningLedger.Create(root, policy, clock);
        ledger.BindNative(Initial());
        // Reserve accepts opaque source identity; the typed receipt must also
        // bind the dataset, even if a caller supplied another dataset's hash.
        var job = ledger.Reserve("other_table", Source(2)).Job!;
        var stage = ledger.BeginStage(job.Id, "candidate", H(3), Initial());
        Assert.Equal("applied", ledger.ResolveIntent(stage.Id, stage.Expected));
        Assert.Equal("learning_evaluation_identity_mismatch",
            Assert.Throws<InvalidOperationException>(() => ledger.RecordEvaluation(job.Id, Evaluation())).Message);
        Assert.Equal("staged", ledger.JobState(job.Id));
        Assert.Throws<InvalidOperationException>(() => ledger.BeginActivate(job.Id, stage.Expected));
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public void UnknownOrUncertainReplyFreezesAndKeepsExactIntent(bool uncertain)
    {
        using (var ledger = LearningLedger.Create(root, Policy(), clock))
        {
            ledger.BindNative(Initial());
            var (job, staged) = Stage(ledger);
            ledger.RecordEvaluation(job.Id, Evaluation());
            var intent = ledger.BeginActivate(job.Id, staged);
            var bad = uncertain ? intent.Expected with { Durable = false }
                : intent.Expected with { Active = H(9) };
            Assert.Equal("frozen", ledger.ResolveIntent(intent.Id, bad));
            Assert.Equal(intent, ledger.PendingIntent);
            Assert.Equal("learning_paused", ledger.Reserve("calibration", Source(8)).Reason);
        }
        using var reopened = LearningLedger.Open(root, Policy(), clock);
        Assert.NotNull(reopened.PendingIntent);
        Assert.Throws<InvalidOperationException>(() => reopened.Resume(Initial()));
    }

    [Fact]
    public void ProbationSurvivesRestartAndBlocksStageUntilSpacedProbesPass()
    {
        LearningJob job; ControlStatus active;
        using (var ledger = LearningLedger.Create(root, Policy(), clock))
        {
            ledger.BindNative(Initial());
            (job, active) = Activate(ledger);
            Assert.Equal("probation", ledger.JobState(job.Id));
            var other = ledger.Reserve("calibration", Source(8)).Job!;
            Assert.Throws<InvalidOperationException>(() => ledger.BeginStage(other.Id, "next", H(9), active));
            Assert.Equal("probe_too_soon", ledger.Probe(active, true));
            clock.Advance(30);
            Assert.Equal("probation", ledger.Probe(active, true));
        }
        using var reopened = LearningLedger.Open(root, Policy(), clock);
        Assert.Equal("probe_too_soon", reopened.Probe(active, true));
        clock.Advance(30); Assert.Equal("probation", reopened.Probe(active, true));
        clock.Advance(30); Assert.Equal("accepted", reopened.Probe(active, true));
        Assert.Equal("accepted", reopened.JobState(job.Id));
        var next = reopened.Reserve("calibration", Source(10)).Job!;
        Assert.Equal("stage", reopened.BeginStage(next.Id, "next", H(11), active).Kind);
    }

    [Fact]
    public void FailedProbeSchedulesOneDurableRollbackThatNeverTogglesOnReplay()
    {
        LearningIntent rollback; ControlStatus active; long jobId;
        using (var ledger = LearningLedger.Create(root, Policy(), clock))
        {
            ledger.BindNative(Initial());
            var result = Activate(ledger); active = result.Active; jobId = result.Job.Id;
            Assert.Equal("rollback_required", ledger.Probe(active, false));
            rollback = ledger.BeginRollback(active);
            Assert.StartsWith("ROLLBACK 2 ", rollback.Request);
            Assert.Equal(H(1), rollback.Expected.Active);
            Assert.Equal(H(3), rollback.Expected.Rollback);
        }
        using var reopened = LearningLedger.Open(root, Policy(), clock);
        Assert.Equal(rollback, reopened.PendingIntent);
        Assert.Equal("applied", reopened.ResolveIntent(rollback.Id, rollback.Expected));
        Assert.Equal("failed", reopened.JobState(jobId));
        Assert.Throws<InvalidOperationException>(() => reopened.BeginRollback(rollback.Expected));
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public void MissedProbeWindowOrRebootCannotCertifyProbation(bool reboot)
    {
        using var ledger = LearningLedger.Create(root, Policy(), clock);
        ledger.BindNative(Initial());
        var (job, active) = Activate(ledger);
        if (reboot) clock.Now = new("00000000-0000-0000-0000-000000000002", 1);
        else clock.Advance(121);
        Assert.Equal("rollback_required", ledger.Probe(active, true));
        Assert.Equal("rollback_required", ledger.Probe(active, true));
        Assert.Equal("probation", ledger.JobState(job.Id));
        Assert.Equal("rollback", ledger.BeginRollback(active).Kind);
    }

    [Fact]
    public void PausedOperatorStillCanRollBackButCannotPromoteOrResumeUnknownState()
    {
        using var ledger = LearningLedger.Create(root, Policy(), clock);
        ledger.BindNative(Initial());
        var (_, active) = Activate(ledger);
        ledger.Pause();
        Assert.Equal("learning_paused", ledger.Reserve("calibration", Source(8)).Reason);
        Assert.Throws<InvalidOperationException>(() => ledger.Resume(active with { Revision = 9 }));
        ledger.Probe(active, false);
        Assert.Equal("rollback", ledger.BeginRollback(active).Kind);
    }

    [Fact]
    public void DiscardOfRejectedCandidateIsPersistedAndCannotGrantActivation()
    {
        using var ledger = LearningLedger.Create(root, Policy(), clock);
        ledger.BindNative(Initial());
        var (job, staged) = Stage(ledger);
        var discard = ledger.BeginDiscard(job.Id, staged);
        Assert.Equal("DISCARD 1 " + H(3) + "\n", discard.Request);
        Assert.Equal("applied", ledger.ResolveIntent(discard.Id, discard.Expected));
        Assert.Equal("failed", ledger.JobState(job.Id));
        Assert.Throws<InvalidOperationException>(() => ledger.BeginActivate(job.Id, discard.Expected));
    }

    [Fact]
    public void FullUnsignedNativeRevisionIsStoredWithoutSignedTruncation()
    {
        using var ledger = LearningLedger.Create(root, Policy(), clock);
        ledger.BindNative(Initial(ulong.MaxValue - 1));
        var (job, active) = Activate(ledger, Initial(ulong.MaxValue - 1));
        Assert.Equal(ulong.MaxValue, active.Revision);
        ledger.Probe(active, false);
        Assert.Throws<ArgumentException>(() => ledger.BeginRollback(active));
        Assert.Null(ledger.PendingIntent);
        Assert.Equal("probation", ledger.JobState(job.Id));
    }

    [Fact]
    public void FailureCannotEraseAJobWithAnUnresolvedNativeSideEffect()
    {
        using var ledger = LearningLedger.Create(root, Policy(), clock);
        ledger.BindNative(Initial());
        var job = ledger.Reserve("calibration", Source(2)).Job!;
        var intent = ledger.BeginStage(job.Id, "candidate", H(3), Initial());
        Assert.Throws<InvalidOperationException>(() => ledger.FailJob(job.Id, "interrupted"));
        Assert.Equal(intent, ledger.PendingIntent);
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public void DelayedActivationReconciliationDoesNotEraseUnobservedProbation(bool reboot)
    {
        using var ledger = LearningLedger.Create(root, Policy(), clock);
        ledger.BindNative(Initial());
        var (job, staged) = Stage(ledger);
        ledger.RecordEvaluation(job.Id, Evaluation());
        var intent = ledger.BeginActivate(job.Id, staged);
        if (reboot) clock.Now = new("00000000-0000-0000-0000-000000000002", 1);
        else clock.Advance(121);
        Assert.Equal("applied", ledger.ResolveIntent(intent.Id, intent.Expected));
        Assert.Equal("rollback_required", ledger.Probe(intent.Expected, true));
    }

    [Fact]
    public void RefusedPromotionsRemainChargedAcrossRestartAndFullRebootWindow()
    {
        var policy = Policy(LearningPolicyTests.Valid.Replace("\"promotions_per_day\":8", "\"promotions_per_day\":1"));
        LearningJob second; ControlStatus secondStage;
        using (var ledger = LearningLedger.Create(root, policy, clock))
        {
            ledger.BindNative(Initial());
            var (job, staged) = Stage(ledger);
            ledger.RecordEvaluation(job.Id, Evaluation());
            var intent = ledger.BeginActivate(job.Id, staged);
            Assert.Equal("refused", ledger.ResolveIntent(intent.Id, staged with { Ok = false, Reason = ControlReason.Refused }));
            var discard = ledger.BeginDiscard(job.Id, staged);
            Assert.Equal("applied", ledger.ResolveIntent(discard.Id, discard.Expected));
            (second, secondStage) = Stage(ledger, discard.Expected, source: 8, candidate: 9);
            ledger.RecordEvaluation(second.Id, Evaluation(source: 8, candidate: 9));
            Assert.Equal("learning_promotion_budget_exhausted",
                Assert.Throws<InvalidOperationException>(() => ledger.BeginActivate(second.Id, secondStage)).Message);
        }
        clock.Now = new("00000000-0000-0000-0000-000000000002", 1);
        using var reopened = LearningLedger.Open(root, policy, clock);
        Assert.Throws<InvalidOperationException>(() => reopened.BeginActivate(second.Id, secondStage));
        clock.Advance(86399);
        Assert.Throws<InvalidOperationException>(() => reopened.BeginActivate(second.Id, secondStage));
        clock.Advance(1);
        Assert.Equal("activate", reopened.BeginActivate(second.Id, secondStage).Kind);
    }

    [Fact]
    public void UnexpectedObservationPersistsPauseEvenWhenBeginThrows()
    {
        using var ledger = LearningLedger.Create(root, Policy(), clock);
        ledger.BindNative(Initial());
        var job = ledger.Reserve("calibration", Source(2)).Job!;
        Assert.Throws<InvalidOperationException>(() => ledger.BeginStage(job.Id, "candidate", H(3), Initial(2)));
        Assert.Equal("learning_paused", ledger.Reserve("calibration", Source(8)).Reason);
        Assert.Null(ledger.PendingIntent);
    }

    [Fact]
    public void VolatileStageRecoveryEitherReissuesExactRequestOrRecognizesExactDigest()
    {
        using var ledger = LearningLedger.Create(root, Policy(), clock);
        ledger.BindNative(Initial());
        var job = ledger.Reserve("calibration", Source(2)).Job!;
        var intent = ledger.BeginStage(job.Id, "candidate", H(3), Initial());
        Assert.Equal(new LearningRecovery("reissue", intent), ledger.Recover(Initial()));
        Assert.Equal(new LearningRecovery("applied", null), ledger.Recover(intent.Expected));
        Assert.Null(ledger.PendingIntent);
        Assert.Equal("staged", ledger.JobState(job.Id));
    }

    [Fact]
    public void StatusAloneNeverResolvesTokenCheckedActivationOrRollback()
    {
        using var ledger = LearningLedger.Create(root, Policy(), clock);
        ledger.BindNative(Initial());
        var (job, staged) = Stage(ledger);
        ledger.RecordEvaluation(job.Id, Evaluation());
        var activation = ledger.BeginActivate(job.Id, staged);
        Assert.Equal(new LearningRecovery("reissue", activation), ledger.Recover(activation.Expected));
        Assert.Equal("evaluated", ledger.JobState(job.Id));
        Assert.Equal("applied", ledger.ResolveIntent(activation.Id, activation.Expected));
        Assert.Equal("rollback_required", ledger.Probe(activation.Expected, false));
        var rollback = ledger.BeginRollback(activation.Expected);
        Assert.Equal(new LearningRecovery("reissue", rollback), ledger.Recover(rollback.Expected));
        Assert.Equal("probation", ledger.JobState(job.Id));
        Assert.Equal("applied", ledger.ResolveIntent(rollback.Id, rollback.Expected));
        Assert.Equal("failed", ledger.JobState(job.Id));
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public void DaemonRestartStageLossFailsJobWithoutRebindingActiveOrLosingCharge(bool activationPending)
    {
        using var ledger = LearningLedger.Create(root, Policy(), clock);
        ledger.BindNative(Initial());
        var (job, staged) = Stage(ledger);
        ledger.RecordEvaluation(job.Id, Evaluation());
        if (activationPending) _ = ledger.BeginActivate(job.Id, staged);
        Assert.Equal(new LearningRecovery("stage_lost", null), ledger.Recover(Initial()));
        Assert.Null(ledger.PendingIntent);
        Assert.Equal("failed", ledger.JobState(job.Id));
        Assert.Equal(1, ledger.JobCount);
        Assert.Equal(new LearningRecovery("matched", null), ledger.Recover(Initial()));
        Assert.NotNull(ledger.Reserve("calibration", Source(2)).Job);
    }

    [Fact]
    public void MissingVolatileStageSatisfiesPendingDiscardButNotAnUnexpectedOwnerChange()
    {
        using var ledger = LearningLedger.Create(root, Policy(), clock);
        ledger.BindNative(Initial());
        var (job, staged) = Stage(ledger);
        ledger.BeginDiscard(job.Id, staged);
        Assert.Equal(new LearningRecovery("applied", null), ledger.Recover(Initial()));
        Assert.Equal("failed", ledger.JobState(job.Id));
        Assert.Equal(new LearningRecovery("frozen", null), ledger.Recover(Initial(2)));
        Assert.Equal("learning_paused", ledger.Reserve("calibration", Source(8)).Reason);
    }
}
