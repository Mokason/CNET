using System.Runtime.Versioning;
using System.Text;
using CnetControlPlane.Learning;
using Microsoft.Data.Sqlite;
using Xunit;

namespace CnetControlPlane.Tests;

[SupportedOSPlatform("linux")]
public sealed class LearningExperienceTests : IDisposable
{
    private readonly string root = Directory.CreateTempSubdirectory("cnet-experience-").FullName;
    private readonly Clock clock = new();
    private readonly LearningPolicy policy = LearningPolicy.Parse(Encoding.UTF8.GetBytes(LearningPolicyTests.Valid));
    private readonly LearningLedger ledger;
    private const string Id = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    private sealed class Clock : ILearningClock
    {
        public LearningInstant Now { get; set; } = new("00000000-0000-0000-0000-000000000001", 100_000_000_000);
        public void Advance() => Now = Now with { Nanoseconds = Now.Nanoseconds + 1_000_000_000 };
    }
    public LearningExperienceTests()
    {
        File.SetUnixFileMode(root, LearningCommandInstallation.Private);
        Directory.CreateDirectory(Path.Combine(root, "data"), LearningCommandInstallation.Private);
        ledger = LearningLedger.Create(root, policy, clock);
    }
    public void Dispose() { ledger.Dispose(); Directory.Delete(root, true); }
    private LocalTableReference Source(int value = 42, byte key = 7)
    {
        var bytes = Encoding.ASCII.GetBytes($"CNET_LOCAL_TABLE_V1\ndataset calibration\nauthority verified_tool\ninput_bits 8\noutput_bits 16\nrows 1\n{key}\t{value}\n");
        var path = Path.Combine(root, "data/calibration.tsv");
        File.WriteAllBytes(path, bytes); File.SetUnixFileMode(path, UnixFileMode.UserRead | UnixFileMode.UserWrite);
        return LocalTableReference.Parse(bytes, new("calibration", "verified_tool"));
    }
    private static LearningAskResult Answer(int? value = null) => LearningAskResult.Parse(Encoding.ASCII.GetBytes(value.HasValue
        ? $"{{\"ok\":true,\"verified\":true,\"miss\":false,\"teacher\":false,\"source\":\"LOCAL\",\"skill\":\"capsule_core\",\"answer\":\"{value}\"}}\n"
        : "{\"ok\":true,\"verified\":false,\"miss\":true,\"teacher\":false,\"source\":\"CNET\",\"skill\":\"capsule_refusal\",\"answer\":\"ABSTAIN: no_covered_certified_plan\"}\n"));

    [Theory]
    [InlineData(false, false, null, "awaiting_evidence")]
    [InlineData(true, false, null, "miss")]
    [InlineData(true, true, null, "abstain")]
    [InlineData(true, false, 42, "verified")]
    [InlineData(true, false, 43, "conflict")]
    [InlineData(false, false, 42, "conflict")]
    public void OutcomesKeepIndependentEvidenceSeparate(bool source, bool excludes, int? value, string state)
    {
        if (source) Source(key: excludes ? (byte)8 : (byte)7);
        var begin = ledger.BeginExperience(Id, "calibration", 7, "synthetic");
        Assert.True(begin.Created); Assert.Equal("pending", begin.Experience.State);
        clock.Advance();
        var result = ledger.FinishExperience(Id, Answer(value));
        Assert.Equal(state, result.State);
        Assert.Equal(source && !excludes ? (ushort?)42 : null, result.Expected);
        Assert.Equal((ushort?)value, result.Value);
        Assert.Equal(state == "conflict", ledger.IsPaused);
        Assert.Equal((0L, 0L), ledger.Demand("calibration", 7));
        Assert.Equal("synthetic", result.Origin);
    }

    [Fact]
    public void PendingAndUnknownAreNeverReplayableOrApprovable()
    {
        var source = Source();
        Assert.True(ledger.BeginExperience(Id, "calibration", 7, "unreviewed").Created);
        Assert.False(ledger.BeginExperience(Id, "calibration", 7, "unreviewed").Created);
        Assert.Throws<InvalidOperationException>(() => ledger.ApproveExperience(Id, source.SourceSha256));
        Assert.Equal("unknown", ledger.FinishExperience(Id, null).State);
        Assert.False(ledger.BeginExperience(Id, "calibration", 7, "unreviewed").Created);
        Assert.Throws<InvalidOperationException>(() => ledger.ApproveExperience(Id, source.SourceSha256));
        Assert.Throws<InvalidOperationException>(() => ledger.FinishExperience(Id, Answer()));
        Assert.Throws<InvalidOperationException>(() => ledger.BeginExperience(Id, "calibration", 8, "unreviewed"));
        Assert.Throws<InvalidOperationException>(() => ledger.BeginExperience(Id, "calibration", 7, "synthetic"));
        Assert.Equal((0L, 0L), ledger.Demand("calibration", 7));
    }

    [Theory]
    [InlineData("replacement")]
    [InlineData("corruption")]
    [InlineData("removal")]
    public void SourceDriftPersistsConflictAndPause(string change)
    {
        Source(); ledger.BeginExperience(Id, "calibration", 7, "unreviewed");
        if (change == "replacement") Source(43);
        if (change == "corruption") File.WriteAllText(Path.Combine(root, "data/calibration.tsv"), "invalid");
        if (change == "removal") File.Delete(Path.Combine(root, "data/calibration.tsv"));
        var error = Record.Exception(() => ledger.FinishExperience(Id, Answer()));
        Assert.True(error is null, "TASK_SOURCE_DRIFT_RED conflict evidence was rolled back instead of retained");
        Assert.Equal("conflict", Assert.Single(ledger.Experiences(0, 10)).State);
        Assert.True(ledger.IsPaused);
    }

    [Fact]
    public void ApprovalCannotAuthorizeAReplacementSourceAndNeverUsesTheObservedValue()
    {
        var source = Source(); ledger.BeginExperience(Id, "calibration", 7, "synthetic");
        ledger.FinishExperience(Id, Answer()); clock.Advance();
        var approved = ledger.ApproveExperience(Id, source.SourceSha256);
        Assert.Equal((ushort)42, approved.ApprovedExpected); Assert.Null(approved.Value);
        Assert.Equal(approved, ledger.ApproveExperience(Id, source.SourceSha256));
        Assert.Equal((1L, 1L), ledger.Demand("calibration", 7));
        var replacement = Source(43);
        Assert.Equal("approved_source_changed", ledger.Reserve("calibration", replacement.SourceSha256).Reason);
        Assert.True(ledger.IsPaused, "TASK_RESERVE_DRIFT_RED approved source change did not pause");
        Assert.Equal(0, ledger.JobCount);
        Assert.Throws<InvalidOperationException>(() => ledger.BeginExperience(new string('b', 32), "calibration", 7, "synthetic"));
    }

    [Fact]
    public void InboxIsReadOnlyAndCursorBounded()
    {
        ledger.BeginExperience(Id, "calibration", 7, "unreviewed");
        var before = File.ReadAllBytes(Path.Combine(root, "ledger.sqlite"));
        clock.Advance();
        using (var reader = LearningLedger.OpenReadOnly(root, policy))
        {
            var row = Assert.Single(reader.Experiences(0, 1));
            Assert.Empty(reader.Experiences(row.Sequence, 1));
            Assert.Throws<ArgumentException>(() => reader.Experiences(-1, 1));
            Assert.Throws<ArgumentException>(() => reader.Experiences(0, 101));
        }
        Assert.Equal(before, File.ReadAllBytes(Path.Combine(root, "ledger.sqlite")));
    }

    [Fact]
    public void PauseBlocksNewObservationAndApprovalButCanRetainACompletedReply()
    {
        var source = Source(); ledger.BeginExperience(Id, "calibration", 7, "synthetic");
        ledger.Pause();
        Assert.Equal("miss", ledger.FinishExperience(Id, Answer()).State);
        Assert.Throws<InvalidOperationException>(() => ledger.BeginExperience(new string('b', 32), "calibration", 7, "synthetic"));
        Assert.Throws<InvalidOperationException>(() => ledger.ApproveExperience(Id, source.SourceSha256));
    }

    [Fact]
    public void DetectableSourceDriftStillPausesWhenNativeOutcomeIsUnknown()
    {
        Source(); ledger.BeginExperience(Id, "calibration", 7, "synthetic"); Source(43);
        Assert.True(ledger.FinishExperience(Id, null).State == "conflict",
            "TASK_SOURCE_UNKNOWN_RED independent drift was hidden by transport uncertainty");
        Assert.True(ledger.IsPaused);
    }

    [Fact]
    public void ApprovalSourceDriftPausesButWrongOperatorHashDoesNot()
    {
        var source = Source(); ledger.BeginExperience(Id, "calibration", 7, "synthetic");
        ledger.FinishExperience(Id, Answer());
        Assert.Throws<InvalidOperationException>(() => ledger.ApproveExperience(Id, new string('0', 64)));
        Assert.False(ledger.IsPaused);
        Source(43);
        Assert.Throws<InvalidOperationException>(() => ledger.ApproveExperience(Id, source.SourceSha256));
        Assert.True(ledger.IsPaused, "TASK_APPROVAL_DRIFT_RED changed source did not persist pause");
        var retained = Assert.Single(ledger.Experiences(0, 10));
        Assert.True(retained.ReviewConflict); Assert.Equal("miss", retained.State);
        Assert.Null(retained.ApprovedSourceSha256);
        Assert.Equal((0L, 0L), ledger.Demand("calibration", 7));
    }

    [Fact]
    public void ChangedApprovedSourceIsAValidRunTickOutcome()
    {
        ledger.BeginOrResumeRun(); clock.Advance();
        var error = Record.Exception(() => ledger.RecordRunTick("approved_source_changed"));
        Assert.True(error is null, "TASK_RUN_CODE_RED source conflict lost to unknown tick code");
    }

    [Fact]
    public void AdmissionRetainsInstallationWideCompletionAndApprovalHeadroom()
    {
        var maximum = policy.MaxStorageMiB * 1024L * 1024;
        using (var retained = File.Create(Path.Combine(root, "retained.bin")))
        {
            File.SetUnixFileMode(retained.Name, UnixFileMode.UserRead | UnixFileMode.UserWrite);
            retained.SetLength(maximum - ledger.MeasureStorage(maximum).Bytes - 2 * 1024 * 1024);
        }
        var error = Record.Exception(() => ledger.BeginExperience(Id, "calibration", 7, "synthetic"));
        Assert.True(error is InvalidOperationException, "TASK_HEADROOM_RED pending admission left insufficient aggregate completion room");
        Assert.Empty(ledger.Experiences(0, 100));
    }

    [Fact]
    public void HistoricalReviewConflictCannotAdmitDemandAfterExplicitResume()
    {
        var initial = new ControlStatus(true, 1, new string('1', 64), null, null, true, ControlReason.Ok);
        ledger.BindNative(initial);
        var source = Source(); ledger.BeginExperience(Id, "calibration", 7, "synthetic");
        ledger.FinishExperience(Id, Answer()); Source(43);
        Assert.Throws<InvalidOperationException>(() => ledger.ApproveExperience(Id, source.SourceSha256));
        Source(); ledger.Resume(initial);
        Assert.Throws<InvalidOperationException>(() => ledger.ApproveExperience(Id, source.SourceSha256));
        Assert.True(ledger.Demand("calibration", 7) == (0L, 0L),
            "TASK_REVIEW_REPLAY_RED refused historical conflict still admitted training demand");
        Assert.Null(Assert.Single(ledger.Experiences(0, 100)).ApprovedSourceSha256);
    }

    [Fact]
    public void NewObservationOfApprovedSourceDriftCommitsPauseBeforeRefusal()
    {
        var source = Source(); ledger.BeginExperience(Id, "calibration", 7, "synthetic");
        ledger.FinishExperience(Id, Answer()); ledger.ApproveExperience(Id, source.SourceSha256); Source(43);
        Assert.Throws<InvalidOperationException>(() => ledger.BeginExperience(new string('b', 32), "calibration", 7, "synthetic"));
        Assert.True(ledger.IsPaused, "TASK_ADMISSION_DRIFT_RED source refusal lost its pause transaction");
        Assert.Single(ledger.Experiences(0, 100));
    }

    [Fact]
    public void NumericObservationOfSymbolicDatasetRefusesWithoutPausing()
    {
        var symbolic = LearningPolicy.Parse(Encoding.UTF8.GetBytes(LearningPolicyTests.Valid.Replace(
            "\"authority\":\"verified_tool\"", "\"authority\":\"verified_tool\",\"symbol_vocabulary_sha256\":\"" + new string('1', 64) + "\"")));
        var otherRoot = Directory.CreateTempSubdirectory("cnet-symbol-observe-").FullName;
        try
        {
            File.SetUnixFileMode(otherRoot, LearningCommandInstallation.Private);
            using var other = LearningLedger.Create(otherRoot, symbolic, clock);
            Assert.ThrowsAny<Exception>(() => other.BeginExperience(Id, "calibration", 7, "synthetic"));
            Assert.False(other.IsPaused, "TASK_SYMBOL_INPUT_RED unsupported numeric input paused healthy learning");
            Assert.Empty(other.Experiences(0, 10));
        }
        finally { Directory.Delete(otherRoot, true); }
    }

    [Theory]
    [InlineData("expiry")]
    [InlineData("heartbeat")]
    [InlineData("stop")]
    [InlineData("reboot")]
    public void OriginalRunAdmissionCannotBeRenewedByObservationOrApproval(string condition)
    {
        var source = Source(); ledger.BeginExperience(Id, "calibration", 7, "synthetic");
        ledger.FinishExperience(Id, Answer()); ledger.BeginOrResumeRun();
        if (condition == "expiry") clock.Now = clock.Now with { Nanoseconds = clock.Now.Nanoseconds + policy.MaxRunSeconds * 1_000_000_000L };
        if (condition == "heartbeat") clock.Now = clock.Now with { Nanoseconds = clock.Now.Nanoseconds + 121_000_000_000L };
        if (condition == "stop") ledger.StopRun("operator_stop");
        if (condition == "reboot") clock.Now = new("00000000-0000-0000-0000-000000000002", 1);
        Assert.Throws<InvalidOperationException>(() => ledger.BeginExperience(new string('b', 32), "calibration", 7, "synthetic"));
        Assert.Throws<InvalidOperationException>(() => ledger.ApproveExperience(Id, source.SourceSha256));
        Assert.Equal((0L, 0L), ledger.Demand("calibration", 7)); Assert.Single(ledger.Experiences(0, 100));
    }

    [Fact]
    public void CrossBootReplyIsUnknownDespiteMatchingExpectedValue()
    {
        Source(); ledger.BeginExperience(Id, "calibration", 7, "synthetic");
        clock.Now = new("00000000-0000-0000-0000-000000000002", 1);
        var experience = ledger.FinishExperience(Id, Answer(42));
        Assert.Equal("unknown", experience.State); Assert.Null(experience.Value);
        Assert.NotEqual(experience.Boot, experience.FinishedBoot);
    }

    [Fact]
    public void ApprovalRequiresPresentCoveringEvidenceAndCanonicalIdentities()
    {
        Assert.Throws<ArgumentException>(() => ledger.BeginExperience(new string('A', 32), "calibration", 7, "synthetic"));
        Assert.Throws<ArgumentException>(() => ledger.BeginExperience(Id, "calibration", 7, "human"));
        Assert.Throws<ArgumentException>(() => ledger.FinishExperience("invalid", null));
        Assert.Throws<ArgumentException>(() => ledger.ApproveExperience(Id, "invalid"));
        Assert.Throws<InvalidOperationException>(() => ledger.ApproveExperience(Id, new string('1', 64)));
        ledger.BeginExperience(Id, "calibration", 7, "synthetic"); ledger.FinishExperience(Id, Answer());
        Assert.Throws<InvalidOperationException>(() => ledger.ApproveExperience(Id, new string('1', 64)));
        var excluding = Source(key: 8);
        Assert.Throws<InvalidOperationException>(() => ledger.ApproveExperience(Id, excluding.SourceSha256));
        Assert.False(ledger.IsPaused); Assert.Equal((0L, 0L), ledger.Demand("calibration", 7));
    }

    [Fact]
    public async Task ConcurrentDuplicateAdmissionCreatesOnlyOneNativeAttemptAuthorization()
    {
        var attempts = await Task.WhenAll(Enumerable.Range(0, 8).Select(_ => Task.Run(() =>
        {
            using var other = LearningLedger.Open(root, policy, clock);
            return other.BeginExperience(Id, "calibration", 7, "synthetic").Created;
        })));
        Assert.Equal(1, attempts.Count(created => created)); Assert.Single(ledger.Experiences(0, 100));
    }

    [Fact]
    public void CapacityRefusesNewIdentityWithoutEvictingPendingTombstones()
    {
        // Bulk synthetic fixture setup, not a claim of 4096 native executions.
        using var db = new SqliteConnection(new SqliteConnectionStringBuilder
        { DataSource = Path.Combine(root, "ledger.sqlite"), Mode = SqliteOpenMode.ReadWrite, Pooling = false }.ToString());
        db.Open(); using var insert = db.CreateCommand();
        insert.CommandText = """
            WITH RECURSIVE slots(i) AS (SELECT 0 UNION ALL SELECT i+1 FROM slots WHERE i+1<$limit)
            INSERT INTO experiences(request_id,dataset,key,origin,boot,started_ns,state)
            SELECT printf('%032x',i),'calibration',7,'synthetic',$boot,$ns,'pending' FROM slots
            """;
        insert.Parameters.AddWithValue("$limit", LearningLedger.MaximumExperiences);
        insert.Parameters.AddWithValue("$boot", clock.Now.Boot); insert.Parameters.AddWithValue("$ns", clock.Now.Nanoseconds);
        Assert.Equal(LearningLedger.MaximumExperiences, insert.ExecuteNonQuery());
        Assert.Equal("learning_task_capacity", Assert.Throws<InvalidOperationException>(() =>
            ledger.BeginExperience(Id, "calibration", 7, "synthetic")).Message);
        var duplicate = ledger.BeginExperience(new string('0', 32), "calibration", 7, "synthetic");
        Assert.False(duplicate.Created); Assert.Equal("pending", duplicate.Experience.State);
        Assert.Equal(100, ledger.Experiences(0, 100).Count); Assert.Empty(ledger.Experiences(4096, 1));
        var gaps = ledger.Gaps(1);
        Assert.Equal(4096, gaps.Observations); Assert.Equal(4096, Assert.Single(gaps.Groups).Requests);
        Assert.Null(gaps.Groups[0].MeanRequestMilliseconds);
        insert.CommandText = "INSERT INTO experiences(request_id,dataset,key,origin,boot,started_ns,state) VALUES($id,'calibration',7,'synthetic',$boot,$ns,'pending')";
        insert.Parameters.AddWithValue("$id", Id); insert.ExecuteNonQuery();
        Assert.Throws<InvalidOperationException>(() => ledger.Gaps(1));
    }

    [Fact]
    public void GapReportKeepsSourceVersionsApprovalsAndUncertaintySeparate()
    {
        Assert.Empty(ledger.Gaps(32).Groups);
        Assert.Throws<ArgumentException>(() => ledger.Gaps(0));
        Assert.Throws<ArgumentException>(() => ledger.Gaps(33));
        var first = Source(); ledger.BeginExperience(Id, "calibration", 7, "synthetic");
        clock.Advance(); ledger.FinishExperience(Id, Answer(42));
        var second = Source(43); ledger.BeginExperience(new string('b', 32), "calibration", 7, "synthetic");
        clock.Advance(); ledger.FinishExperience(new string('b', 32), Answer());
        ledger.ApproveExperience(new string('b', 32), second.SourceSha256);
        ledger.BeginExperience(new string('c', 32), "calibration", 7, "unreviewed");
        ledger.FinishExperience(new string('c', 32), null);
        var report = ledger.Gaps(32);
        Assert.Equal(3, report.TotalGroups); Assert.False(report.Truncated);
        var original = Assert.Single(report.Groups, group => group.SourceSha256 == first.SourceSha256);
        Assert.Equal("verified", original.LatestState); Assert.Equal(1000, original.MeanRequestMilliseconds);
        var approved = Assert.Single(report.Groups, group => group.ApprovedRequests == 1);
        Assert.Equal(second.SourceSha256, approved.SourceSha256); Assert.Equal(0, approved.PendingApprovals);
        var unknown = Assert.Single(report.Groups, group => group.Origin == "unreviewed");
        Assert.Equal(1, unknown.States["unknown"]); Assert.Equal(0, unknown.PendingApprovals);
        Assert.False(report.TrainingEligible); Assert.Null(report.EstimatedLearningSeconds);
    }
}
