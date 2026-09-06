using System.Globalization;
using System.Runtime.Versioning;
using System.Text;
using CnetControlPlane.Learning;
using Microsoft.Data.Sqlite;
using Xunit;

namespace CnetControlPlane.Tests;

[SupportedOSPlatform("linux")]
public sealed class LearningLedgerIntegrityTests : IDisposable
{
    private readonly string root = Directory.CreateTempSubdirectory("cnet-ledger-integrity-").FullName;
    private sealed class Clock : ILearningClock
    {
        public LearningInstant Now { get; set; } = new("00000000-0000-0000-0000-000000000001", 100_000_000_000);
        public void Advance(long seconds) => Now = Now with { Nanoseconds = Now.Nanoseconds + seconds * 1_000_000_000 };
    }
    private readonly Clock clock = new();
    private static LearningPolicy Policy(string? text = null) => LearningPolicy.Parse(Encoding.UTF8.GetBytes(text ?? LearningPolicyTests.Valid));
    private static string H(int n) => n.ToString("x64", CultureInfo.InvariantCulture);
    private static ControlStatus Initial => new(true, 1, H(1), null, null, true, ControlReason.Ok);
    public LearningLedgerIntegrityTests() => File.SetUnixFileMode(root, UnixFileMode.UserRead | UnixFileMode.UserWrite | UnixFileMode.UserExecute);
    public void Dispose() => Directory.Delete(root, recursive: true);

    // Separate real SQLite connections deliberately damage task-owned fixtures.
    // No production fault-injection hook or weakened writer checks are needed.
    private object? Sql(string sql)
    {
        using var connection = new SqliteConnection(new SqliteConnectionStringBuilder
        { DataSource = Path.Combine(root, "ledger.sqlite"), Mode = SqliteOpenMode.ReadWrite, Pooling = false }.ToString());
        connection.Open();
        using var command = connection.CreateCommand();
        command.CommandText = sql;
        return command.ExecuteScalar();
    }
    private static LocalTableReference Reference(int value) => LocalTableReference.Parse(Encoding.ASCII.GetBytes(
        "CNET_LOCAL_TABLE_V1\ndataset calibration\nauthority verified_tool\ninput_bits 8\noutput_bits 16\nrows 1\n7\t" +
        value.ToString(CultureInfo.InvariantCulture) + "\n"), new("calibration", "verified_tool"));
    private static LearningTableEvaluation Evaluation(int value, int candidate)
    {
        var reference = Reference(value);
        // Canonical unit-test parser fixture, not a claim of native execution.
        var text = new StringBuilder("CNET_TABLE_SNAPSHOT_EVAL_V1\nsnapshot_sha256 " + H(candidate) +
            "\ndataset calibration\nsource_sha256 " + reference.SourceSha256 + "\nresults 256\n");
        for (var key = 0; key < 256; key++)
            text.Append(key.ToString(CultureInfo.InvariantCulture)).Append(key == 7
                ? "\t1\t" + value.ToString(CultureInfo.InvariantCulture) + "\n" : "\t0\t-\n");
        return LearningTableEvaluation.Parse(Encoding.ASCII.GetBytes(text.Append("end\n").ToString()), reference, H(candidate));
    }
    private static (LearningJob Job, ControlStatus Staged) Evaluated(LearningLedger ledger, int value = 2, int candidate = 3)
    {
        var job = ledger.Reserve("calibration", Reference(value).SourceSha256).Job!;
        var stage = ledger.BeginStage(job.Id, "candidate_" + job.Id, H(candidate), Initial);
        Assert.Equal("applied", ledger.ResolveIntent(stage.Id, stage.Expected));
        ledger.RecordEvaluation(job.Id, Evaluation(value, candidate));
        return (job, stage.Expected);
    }
    private static LearningIntent PendingActivation(LearningLedger ledger)
    {
        ledger.BindNative(Initial);
        var (job, staged) = Evaluated(ledger);
        return ledger.BeginActivate(job.Id, staged);
    }
    private static void IntegrityRefusal(Action action, string reason)
    {
        var error = Record.Exception(action);
        Assert.True(error is InvalidOperationException,
            "LEARNING_LEDGER_INTEGRITY_RED: damaged accounting or unsupported identity was accepted");
        Assert.Equal(reason, error.Message);
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public void ExistingWalIsRefusedWithoutJournalConversionEvenForWrongPolicy(bool wrongPolicy)
    {
        using (LearningLedger.Create(root, Policy(), clock)) { }
        Assert.Equal("wal", Sql("PRAGMA journal_mode=WAL"));
        var policy = Policy(wrongPolicy ? LearningPolicyTests.Valid + "\n" : null);
        var error = Record.Exception(() => { using var reopened = LearningLedger.Open(root, policy, clock); });
        Assert.True(Equals("wal", Sql("PRAGMA journal_mode")),
            "LEARNING_LEDGER_INTEGRITY_RED: Open converted existing WAL before refusing identity");
        Assert.Equal("learning_durable_sqlite_required", Assert.IsType<InvalidOperationException>(error).Message);
    }

    [Fact]
    public void CreatePinsTheFreshSchemaVersionAndDeleteJournal()
    {
        using (LearningLedger.Create(root, Policy(), clock)) { }
        Assert.Equal("delete", Sql("PRAGMA journal_mode"));
        Assert.True(Equals(1L, Sql("PRAGMA user_version")),
            "LEARNING_LEDGER_INTEGRITY_RED: the new schema has no explicit user_version identity");
    }

    [Theory]
    [InlineData(0)]
    [InlineData(2)]
    [InlineData(int.MaxValue)]
    public void UnsupportedVersionRefusesWithoutMigrationResetOrTimeAdvance(int version)
    {
        using (var ledger = LearningLedger.Create(root, Policy(), clock))
            Assert.NotNull(ledger.Reserve("calibration", H(2)).Job);
        Sql("PRAGMA user_version=" + version.ToString(CultureInfo.InvariantCulture));
        clock.Advance(1);
        IntegrityRefusal(() => { using var reopened = LearningLedger.Open(root, Policy(), clock); },
            "learning_ledger_schema_version");
        Assert.Equal((long)version, Sql("PRAGMA user_version"));
        Assert.Equal(1L, Sql("SELECT count(*) FROM jobs"));
        Assert.Equal(100_000_000_000L, Sql("SELECT last_ns FROM epochs"));
    }

    [Fact]
    public void LiveUnsupportedVersionRefusesBeforeBudgetOrEpochMutation()
    {
        using var ledger = LearningLedger.Create(root, Policy(), clock);
        Sql("PRAGMA user_version=2");
        clock.Advance(1);
        IntegrityRefusal(() => ledger.Reserve("calibration", H(2)), "learning_ledger_schema_version");
        Assert.Equal(0, ledger.JobCount);
        Assert.Equal(2L, Sql("PRAGMA user_version"));
        Assert.Equal(100_000_000_000L, Sql("SELECT last_ns FROM epochs"));
    }

    [Theory]
    [InlineData("DELETE FROM promotion_charges")]
    [InlineData("UPDATE promotion_charges SET ns=99999999999")]
    [InlineData("UPDATE promotion_charges SET ns=100000000001")]
    [InlineData("INSERT INTO promotion_charges SELECT id,'00000000-0000-0000-0000-000000000001',100000000000 FROM intents WHERE kind='stage'")]
    public void OpenRefusesAccountingDamageNotDetectedByForeignKeys(string damage)
    {
        using (var ledger = LearningLedger.Create(root, Policy(), clock)) PendingActivation(ledger);
        Sql(damage);
        Assert.Null(Sql("PRAGMA foreign_key_check"));
        Assert.Equal("ok", Sql("PRAGMA quick_check"));
        IntegrityRefusal(() => { using var reopened = LearningLedger.Open(root, Policy(), clock); },
            "learning_ledger_promotion_integrity");
    }

    [Theory]
    [InlineData("DELETE FROM promotion_charges")]
    [InlineData("UPDATE promotion_charges SET ns=99999999999")]
    [InlineData("UPDATE promotion_charges SET ns=100000000001")]
    [InlineData("INSERT INTO promotion_charges SELECT id,'00000000-0000-0000-0000-000000000001',100000000000 FROM intents WHERE kind='stage'")]
    public void LiveAccountingDamageRefusesBeforeAdvancingEpochOrReserving(string damage)
    {
        using var ledger = LearningLedger.Create(root, Policy(), clock);
        PendingActivation(ledger);
        Sql(damage);
        // In particular, this later clock must not heal a future charge by
        // moving last_ns forward before the damaged row is checked.
        clock.Advance(2);
        IntegrityRefusal(() => ledger.Reserve("calibration", H(9)), "learning_ledger_promotion_integrity");
        Assert.Equal(1, ledger.JobCount);
        Assert.Equal(100_000_000_000L, Sql("SELECT last_ns FROM epochs"));
    }

    [Fact]
    public void RemovingARefusedChargeCannotRefillTheLiveDailyPromotionBudget()
    {
        var policy = Policy(LearningPolicyTests.Valid.Replace("\"promotions_per_day\":8", "\"promotions_per_day\":1"));
        Assert.Equal(1, policy.PromotionsPerDay);
        using var ledger = LearningLedger.Create(root, policy, clock);
        var first = PendingActivation(ledger);
        Assert.Equal("refused", ledger.ResolveIntent(first.Id, first.Before with { Ok = false, Reason = ControlReason.Refused }));
        var discard = ledger.BeginDiscard(first.JobId, first.Before);
        Assert.Equal("applied", ledger.ResolveIntent(discard.Id, discard.Expected));
        var (second, staged) = Evaluated(ledger, 8, 9);
        Assert.Equal("learning_promotion_budget_exhausted", Assert.Throws<InvalidOperationException>(
            () => ledger.BeginActivate(second.Id, staged)).Message);
        Sql("DELETE FROM promotion_charges");
        IntegrityRefusal(() => ledger.BeginActivate(second.Id, staged), "learning_ledger_promotion_integrity");
        Assert.Null(ledger.PendingIntent);
        Assert.Equal("evaluated", ledger.JobState(second.Id));
        Assert.Equal(1L, Sql("SELECT count(*) FROM intents WHERE kind='activate'"));
    }

    [Fact]
    public void AChargeInAnotherEpochMustBeWithinThatEpochNotTheCurrentOne()
    {
        using (var ledger = LearningLedger.Create(root, Policy(), clock))
        {
            PendingActivation(ledger);
            clock.Now = new("00000000-0000-0000-0000-000000000002", 10_000_000_000);
            ledger.RecordDemand("calibration", 7, true);
        }
        Sql("UPDATE promotion_charges SET ns=10000000000");
        IntegrityRefusal(() => { using var reopened = LearningLedger.Open(root, Policy(), clock); },
            "learning_ledger_promotion_integrity");
    }
}
