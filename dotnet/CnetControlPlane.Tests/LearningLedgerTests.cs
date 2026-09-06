using System.Runtime.Versioning;
using System.Text;
using System.Diagnostics;
using CnetControlPlane.Learning;
using Microsoft.Data.Sqlite;
using Xunit;

namespace CnetControlPlane.Tests;

[SupportedOSPlatform("linux")]
public sealed class LearningLedgerTests : IDisposable
{
    private readonly string root = Directory.CreateTempSubdirectory("cnet-learning-ledger-").FullName;
    private static readonly LearningPolicy Policy = LearningPolicy.Parse(Encoding.UTF8.GetBytes(LearningPolicyTests.Valid));
    private sealed class FakeClock : ILearningClock
    {
        public LearningInstant Now { get; set; } = new("00000000-0000-0000-0000-000000000001", 100_000_000_000);
        public void Advance(long seconds) => Now = Now with { Nanoseconds = Now.Nanoseconds + seconds * 1_000_000_000 };
    }
    public LearningLedgerTests() => File.SetUnixFileMode(root, UnixFileMode.UserRead | UnixFileMode.UserWrite | UnixFileMode.UserExecute);
    public void Dispose() => Directory.Delete(root, true);
    private static string Hash(int n) => n.ToString("x64");

    [Fact]
    public void DemandAndReservationsSurviveRestartWithoutRefillingBudget()
    {
        var clock = new FakeClock();
        using (var ledger = LearningLedger.Create(root, Policy, clock))
        {
            ledger.RecordDemand("calibration", 7, missed: true);
            ledger.RecordDemand("calibration", 7, missed: false);
            for (var n = 0; n < 4; n++) Assert.NotNull(ledger.Reserve("calibration", Hash(n)).Job);
            Assert.Equal("attempt_budget_exhausted", ledger.Reserve("calibration", Hash(9)).Reason);
        }
        using var reopened = LearningLedger.Open(root, Policy, clock);
        Assert.Equal((2L, 1L), reopened.Demand("calibration", 7));
        Assert.Equal(4, reopened.JobCount);
        Assert.Equal("attempt_budget_exhausted", reopened.Reserve("calibration", Hash(9)).Reason);
        clock.Advance(3600);
        Assert.NotNull(reopened.Reserve("calibration", Hash(9)).Job);
    }

    [Fact]
    public void SourceRetryCapAndPolicyIdentityAreDurable()
    {
        var clock = new FakeClock();
        using (var ledger = LearningLedger.Create(root, Policy, clock))
        {
            var first = ledger.Reserve("calibration", Hash(1)).Job!;
            Assert.Equal("source_already_pending", ledger.Reserve("calibration", Hash(1)).Reason);
            ledger.FailJob(first.Id, "source_rejected");
            var second = ledger.Reserve("calibration", Hash(1)).Job!;
            ledger.FailJob(second.Id, "source_rejected");
            Assert.Equal("source_attempts_exhausted", ledger.Reserve("calibration", Hash(1)).Reason);
            Assert.Throws<ArgumentException>(() => ledger.Reserve("unknown", Hash(1)));
            Assert.Throws<ArgumentException>(() => ledger.Reserve("calibration", "bad"));
        }
        var other = LearningPolicy.Parse(Encoding.UTF8.GetBytes(LearningPolicyTests.Valid + "\n"));
        Assert.Throws<InvalidOperationException>(() => LearningLedger.Open(root, other, clock));
        Assert.Throws<InvalidOperationException>(() => LearningLedger.Create(root, Policy, clock));
    }

    [Fact]
    public void RebootQuarantinesOldChargesAndClockRollbackRefuses()
    {
        var clock = new FakeClock();
        using var ledger = LearningLedger.Create(root, Policy, clock);
        for (var n = 0; n < 4; n++) ledger.Reserve("calibration", Hash(n));
        clock.Now = new("00000000-0000-0000-0000-000000000002", 10_000_000_000);
        Assert.Equal("attempt_budget_exhausted", ledger.Reserve("calibration", Hash(9)).Reason);
        clock.Advance(3599);
        Assert.Equal("attempt_budget_exhausted", ledger.Reserve("calibration", Hash(9)).Reason);
        clock.Advance(1);
        Assert.NotNull(ledger.Reserve("calibration", Hash(9)).Job);
        clock.Advance(-1);
        Assert.Throws<InvalidOperationException>(() => ledger.RecordDemand("calibration", 7, true));
    }

    [Fact]
    public void MissingAndCorruptStateNeverCreateFreshBudget()
    {
        var clock = new FakeClock();
        Assert.Throws<InvalidOperationException>(() => LearningLedger.Open(root, Policy, clock));
        using (LearningLedger.Create(root, Policy, clock)) { }
        File.WriteAllBytes(Path.Combine(root, "ledger.sqlite"), [0xff, 0x55]);
        Assert.Throws<InvalidOperationException>(() => LearningLedger.Open(root, Policy, clock));
    }

    private void CorruptFixture(string sql)
    {
        using var connection = new SqliteConnection(new SqliteConnectionStringBuilder
            { DataSource = Path.Combine(root, "ledger.sqlite"), Mode = SqliteOpenMode.ReadWrite, Pooling = false }.ToString());
        connection.Open();
        using var command = connection.CreateCommand();
        // Deliberately bypass writer-side checks to simulate damaged durable rows.
        command.CommandText = "PRAGMA foreign_keys=OFF; PRAGMA ignore_check_constraints=ON; " + sql;
        command.ExecuteNonQuery();
    }

    [Fact]
    public void MissingCurrentEpochCannotReleaseRebootQuarantine()
    {
        var clock = new FakeClock();
        using (var ledger = LearningLedger.Create(root, Policy, clock))
        {
            for (var n = 0; n < 4; n++) Assert.NotNull(ledger.Reserve("calibration", Hash(n)).Job);
            clock.Now = new("00000000-0000-0000-0000-000000000002", 7200_000_000_000);
            ledger.RecordDemand("calibration", 7, true);
            Assert.Equal("attempt_budget_exhausted", ledger.Reserve("calibration", Hash(9)).Reason);
        }
        CorruptFixture("DELETE FROM epochs WHERE boot='00000000-0000-0000-0000-000000000002'");
        var refusal = Record.Exception(() => { using var reopened = LearningLedger.Open(root, Policy, clock); });
        Assert.True(refusal is InvalidOperationException,
            "LEARNING_LEDGER_RED missing epoch was interpreted as zero and reset reboot quarantine");
    }

    [Theory]
    [InlineData("UPDATE epochs SET first_ns=-1")]
    [InlineData("UPDATE epochs SET first_ns=last_ns+1")]
    [InlineData("UPDATE jobs SET reserved_ns=-1")]
    [InlineData("UPDATE jobs SET reserved_ns=reserved_ns+1")]
    [InlineData("UPDATE jobs SET boot='00000000-0000-0000-0000-000000000099'")]
    [InlineData("UPDATE demand SET misses=requests+1")]
    public void MalformedDurableRowsRefuseBeforeAnyNewReservation(string corruption)
    {
        var clock = new FakeClock();
        using (var ledger = LearningLedger.Create(root, Policy, clock))
        {
            Assert.NotNull(ledger.Reserve("calibration", Hash(1)).Job);
            ledger.RecordDemand("calibration", 7, true);
        }
        CorruptFixture(corruption);
        var refusal = Record.Exception(() => { using var reopened = LearningLedger.Open(root, Policy, clock); });
        Assert.True(refusal is InvalidOperationException,
            "LEARNING_LEDGER_RED malformed durable accounting was accepted");
    }

    [Fact]
    public void LiveMissingEpochAlsoRefusesInsteadOfUpdatingZeroRows()
    {
        var clock = new FakeClock();
        using var ledger = LearningLedger.Create(root, Policy, clock);
        Assert.NotNull(ledger.Reserve("calibration", Hash(1)).Job);
        CorruptFixture("DELETE FROM epochs");
        Assert.Equal("learning_ledger_epoch_integrity", Assert.Throws<InvalidOperationException>(
            () => ledger.RecordDemand("calibration", 7, true)).Message);
        Assert.Equal((0L, 0L), ledger.Demand("calibration", 7));
        Assert.Equal(1, ledger.JobCount);
    }

    [Theory]
    [InlineData("UPDATE epochs SET first_ns=1.5")]
    [InlineData("UPDATE epochs SET first_ns=-1")]
    [InlineData("UPDATE epochs SET last_ns=first_ns-1")]
    [InlineData("UPDATE jobs SET reserved_ns=-1")]
    [InlineData("UPDATE demand SET misses=requests+1")]
    [InlineData("DELETE FROM epochs")]
    public void WriterSideTypesChecksAndEpochReferencesRejectMalformedChanges(string sql)
    {
        var clock = new FakeClock();
        using (var ledger = LearningLedger.Create(root, Policy, clock))
        {
            ledger.Reserve("calibration", Hash(1));
            ledger.RecordDemand("calibration", 7, true);
        }
        using var connection = new SqliteConnection(new SqliteConnectionStringBuilder
            { DataSource = Path.Combine(root, "ledger.sqlite"), Mode = SqliteOpenMode.ReadWrite, Pooling = false }.ToString());
        connection.Open();
        using var command = connection.CreateCommand();
        command.CommandText = "PRAGMA foreign_keys=ON; " + sql;
        Assert.Throws<SqliteException>(() => command.ExecuteNonQuery());
    }

    [Fact]
    public async Task CompetingConnectionsShareOneBudgetAndDemandCounter()
    {
        var clock = new FakeClock();
        using var first = LearningLedger.Create(root, Policy, clock);
        using var second = LearningLedger.Open(root, Policy, clock);
        var tasks = new[] { first, second }.Select((ledger, index) => Task.Run(() =>
        {
            var admitted = 0;
            for (var n = 0; n < 12; n++)
            {
                ledger.RecordDemand("calibration", 7, true);
                if (ledger.Reserve("calibration", Hash(index * 100 + n)).Job is not null) admitted++;
            }
            return admitted;
        })).ToArray();
        Assert.Equal(4, (await Task.WhenAll(tasks)).Sum());
        Assert.Equal(4, first.JobCount);
        Assert.Equal((24L, 24L), first.Demand("calibration", 7));
    }

    [Theory]
    [InlineData("ledger.sqlite")]
    [InlineData("owner.lock")]
    [InlineData("ledger.sqlite-journal")]
    [InlineData("ledger.sqlite-wal")]
    [InlineData("ledger.sqlite-shm")]
    public void DatabaseAndSidecarSymlinksRefuse(string name)
    {
        var clock = new FakeClock();
        using (LearningLedger.Create(root, Policy, clock)) { }
        var selected = Path.Combine(root, name);
        if (File.Exists(selected)) File.Move(selected, selected + ".saved");
        File.CreateSymbolicLink(selected, "missing_target");
        Assert.Throws<InvalidOperationException>(() => LearningLedger.Open(root, Policy, clock));
        Assert.False(File.Exists(Path.Combine(root, "missing_target")));
    }

    [Fact]
    public void RenamingRootRefusesWithoutRedirectingAnOpenLedger()
    {
        var clock = new FakeClock();
        var moved = root + "-moved";
        try
        {
            using (var ledger = LearningLedger.Create(root, Policy, clock))
            {
                ledger.RecordDemand("calibration", 7, true);
                Assert.NotNull(ledger.Reserve("calibration", Hash(1)).Job);
                Directory.Move(root, moved);
                Directory.CreateDirectory(root, UnixFileMode.UserRead | UnixFileMode.UserWrite | UnixFileMode.UserExecute);
                Assert.Equal("learning_root_path_identity_changed", Assert.Throws<InvalidOperationException>(
                    () => ledger.RecordDemand("calibration", 7, true)).Message);
                Assert.Empty(Directory.EnumerateFileSystemEntries(root));
            }
            using var reopened = LearningLedger.Open(moved, Policy, clock);
            Assert.Equal((1L, 1L), reopened.Demand("calibration", 7));
            Assert.Equal(1, reopened.JobCount);
        }
        finally { if (Directory.Exists(moved)) Directory.Delete(moved, true); }
    }

    [Fact]
    public void CapacityDisabledPolicyAndReusedBootRefuse()
    {
        var clock = new FakeClock();
        var small = LearningPolicy.Parse(Encoding.UTF8.GetBytes(LearningPolicyTests.Valid.Replace("\"max_jobs\":256", "\"max_jobs\":16")));
        using var ledger = LearningLedger.Create(root, small, clock);
        for (var n = 0; n < 16; n++)
        {
            Assert.NotNull(ledger.Reserve("calibration", Hash(n)).Job);
            clock.Advance(3600);
        }
        Assert.Equal("job_capacity_exhausted", ledger.Reserve("calibration", Hash(17)).Reason);
        var original = clock.Now;
        clock.Now = new("00000000-0000-0000-0000-000000000002", 1);
        ledger.RecordDemand("calibration", 7, true);
        clock.Now = original;
        Assert.Throws<InvalidOperationException>(() => ledger.RecordDemand("calibration", 7, true));
    }

    [Fact]
    public void DisabledPolicyCanInspectButCannotReserve()
    {
        var disabled = LearningPolicy.Parse(Encoding.UTF8.GetBytes(LearningPolicyTests.Valid.Replace("\"enabled\":true", "\"enabled\":false")));
        using var ledger = LearningLedger.Create(root, disabled, new FakeClock());
        Assert.Equal(0, ledger.JobCount);
        Assert.Throws<InvalidOperationException>(() => ledger.Reserve("calibration", Hash(1)));
    }

    [Fact]
    public void FailedOpenCannotReleaseAnotherConnectionsOperatingSystemLock()
    {
        var clock = new FakeClock();
        using var ledger = LearningLedger.Create(root, Policy, clock);
        var path = Path.Combine(root, "ledger.sqlite");
        using var connection = new SqliteConnection(new SqliteConnectionStringBuilder
            { DataSource = path, Mode = SqliteOpenMode.ReadWrite, Pooling = false }.ToString());
        connection.Open();
        using var transaction = connection.BeginTransaction(deferred: false);
        Assert.Throws<InvalidOperationException>(() => LearningLedger.Open(root, Policy, clock));
        // A separate process must remain excluded after the failed second open.
        // Closing a non-SQLite descriptor for this inode would drop POSIX locks.
        var start = new ProcessStartInfo("/usr/bin/sqlite3")
            { UseShellExecute = false, RedirectStandardError = true, RedirectStandardOutput = true };
        start.Environment.Clear();
        start.ArgumentList.Add(path);
        start.ArgumentList.Add("PRAGMA busy_timeout=0; BEGIN IMMEDIATE; ROLLBACK;");
        using var competitor = Process.Start(start)!;
        if (!competitor.WaitForExit(5000)) { competitor.Kill(); competitor.WaitForExit(); Assert.Fail("lock probe timed out"); }
        Assert.True(competitor.ExitCode != 0,
            "LEARNING_LEDGER_RED unrelated descriptor close released active SQLite lock");
        Assert.Contains("locked", competitor.StandardError.ReadToEnd(), StringComparison.OrdinalIgnoreCase);
    }
}
