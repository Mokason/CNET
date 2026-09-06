using System.Runtime.Versioning;
using System.Text;
using CnetControlPlane.Learning;
using Microsoft.Data.Sqlite;
using Xunit;

namespace CnetControlPlane.Tests;

[SupportedOSPlatform("linux")]
public sealed class LearningStorageConcurrencyTests : IDisposable
{
    private const long Limit = 256L * 1024 * 1024;
    private const UnixFileMode Private = UnixFileMode.UserRead | UnixFileMode.UserWrite | UnixFileMode.UserExecute;
    private readonly string root = Directory.CreateTempSubdirectory("cnet-storage-ledger-").FullName;
    private readonly Clock clock = new();
    private readonly LearningLedger ledger;
    private sealed class Clock : ILearningClock
    {
        public int Reads { get; private set; }
        public LearningInstant Now { get { Reads++; return new("00000000-0000-0000-0000-000000000001", 100_000_000_000); } }
    }
    public LearningStorageConcurrencyTests()
    {
        File.SetUnixFileMode(root, Private);
        ledger = LearningLedger.Create(root, LearningPolicy.Parse(Encoding.UTF8.GetBytes(LearningPolicyTests.Valid)), clock);
    }
    public void Dispose() { ledger.Dispose(); Directory.Delete(root, true); }
    private SqliteConnection Connection()
    {
        var connection = new SqliteConnection(new SqliteConnectionStringBuilder
            { DataSource = Path.Combine(root, "ledger.sqlite"), Pooling = false }.ToString());
        connection.Open(); return connection;
    }
    private long Bytes() => Directory.EnumerateFiles(root).Sum(path => new FileInfo(path).Length);

    [Fact]
    public async Task PendingDemandWriterSettlesBeforeMeasurementAndCommittedGrowthIsCounted()
    {
        var before = Bytes();
        using var connection = Connection();
        using var writer = connection.BeginTransaction(deferred: false);
        using (var command = connection.CreateCommand())
        {
            command.Transaction = writer;
            command.CommandText = """
                WITH RECURSIVE keys(n) AS (SELECT 0 UNION ALL SELECT n+1 FROM keys WHERE n<255)
                INSERT INTO demand SELECT 'calibration',n,1,1 FROM keys;
                """;
            command.ExecuteNonQuery();
        }
        var started = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var measurement = Task.Run(() => { started.SetResult(); return ledger.MeasureStorage(Limit); });
        bool completedBeforeCommit;
        try
        {
            await started.Task.WaitAsync(TimeSpan.FromSeconds(5));
            completedBeforeCommit = await Task.WhenAny(measurement, Task.Delay(200)) == measurement;
        }
        finally { writer.Commit(); }
        var usage = await measurement.WaitAsync(TimeSpan.FromSeconds(10));
        Assert.False(completedBeforeCommit, "LEARNING_STORAGE_CONCURRENCY_RED scan bypassed an active demand writer");
        Assert.True(Bytes() > before, "committed demand fixture did not grow the database");
        Assert.Equal(Bytes(), usage.Bytes);
        Assert.Equal((1L, 1L), ledger.Demand("calibration", 7));
    }

    [Fact]
    public void MeasurementDoesNotReadClockOrRewriteEpochAndKeepsExactLimits()
    {
        var reads = clock.Reads;
        var before = Bytes();
        var stamp = File.GetLastWriteTimeUtc(Path.Combine(root, "ledger.sqlite"));
        var usage = ledger.MeasureStorage(before);
        Assert.Equal(before, usage.Bytes);
        Assert.Equal(reads, clock.Reads);
        Assert.Equal(stamp, File.GetLastWriteTimeUtc(Path.Combine(root, "ledger.sqlite")));
        Assert.Equal("learning_storage_byte_limit", Assert.Throws<InvalidOperationException>(
            () => ledger.MeasureStorage(before - 1)).Message);
        ledger.RecordDemand("calibration", 7, true); // A refused scan released its lock.
        Assert.Equal((1L, 1L), ledger.Demand("calibration", 7));
    }

    [Fact]
    public void ExistingColdJournalIsIncludedWithoutAMeasurementWrite()
    {
        var path = Path.Combine(root, "ledger.sqlite-journal");
        File.WriteAllBytes(path, new byte[8192]); // Zero header: private cold journal, not recovery authority.
        File.SetUnixFileMode(path, UnixFileMode.UserRead | UnixFileMode.UserWrite);
        var before = Bytes();
        var reads = clock.Reads;
        var usage = ledger.MeasureStorage(before);
        Assert.True(File.Exists(path));
        Assert.Equal(before, usage.Bytes);
        Assert.Equal(before, Bytes());
        Assert.Equal(reads, clock.Reads);
        Assert.Equal("learning_storage_byte_limit", Assert.Throws<InvalidOperationException>(
            () => ledger.MeasureStorage(before - 1)).Message);
    }

    [Fact]
    public void LinkedArtifactsStillRefuseAndReleaseTheMeasurementLock()
    {
        File.CreateSymbolicLink(Path.Combine(root, "unexpected"), "ledger.sqlite");
        Assert.Equal("learning_storage_entry", Assert.Throws<InvalidOperationException>(
            () => ledger.MeasureStorage(Limit)).Message);
        ledger.RecordDemand("calibration", 7, true);
        Assert.Equal((1L, 1L), ledger.Demand("calibration", 7));
    }
}
