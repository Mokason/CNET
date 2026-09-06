using Microsoft.Data.Sqlite;

namespace CnetControlPlane.Learning;

internal sealed record LearningJob(long Id, string Dataset, string SourceSha256);
internal sealed record LearningReservation(LearningJob? Job, string Reason);
internal sealed partial class LearningLedger : IDisposable
{
    private readonly LearningFiles files;
    private readonly SqliteConnection db;
    private readonly LearningPolicy policy;
    private readonly ILearningClock clock;
    private LearningLedger(LearningFiles files, SqliteConnection db, LearningPolicy policy, ILearningClock clock)
    { this.files = files; this.db = db; this.policy = policy; this.clock = clock; }

    public static LearningLedger Create(string root, LearningPolicy policy, ILearningClock clock) => Connect(root, policy, clock, true);
    public static LearningLedger Open(string root, LearningPolicy policy, ILearningClock clock) => Connect(root, policy, clock, false);
    private static LearningLedger Connect(string root, LearningPolicy policy, ILearningClock clock, bool create)
    {
        var files = LearningFiles.Open(root);
        SqliteConnection? db = null;
        try
        {
            if (create)
            {
                files.WriteNew("ledger.sqlite", []);
                files.WriteNew("owner.lock", []);
            }
            // Never open/close a non-SQLite FD for the database inode: closing
            // ANY such FD drops this process's POSIX locks on other connections.
            files.AssertPathIdentity();
            _ = files.ValidateFile("ledger.sqlite");
            _ = files.ValidateFile("owner.lock");
            // The trusted owner must not supply pre-existing journal aliases.
            // No fallback to File.Exists: dangling links and access errors refuse.
            foreach (var name in new[] { "ledger.sqlite-journal", "ledger.sqlite-wal", "ledger.sqlite-shm" })
                _ = files.ValidateFile(name, allowMissing: true);
            db = new SqliteConnection(new SqliteConnectionStringBuilder
            { DataSource = Path.Combine(files.FullPath, "ledger.sqlite"), Mode = SqliteOpenMode.ReadWrite,
                Pooling = false, Cache = SqliteCacheMode.Private, DefaultTimeout = 5 }.ToString());
            db.Open();
            var result = new LearningLedger(files, db, policy, clock);
            result.Execute("PRAGMA trusted_schema=OFF; PRAGMA foreign_keys=ON; PRAGMA synchronous=EXTRA; PRAGMA temp_store=MEMORY;");
            // Opening existing state must not rewrite its journal identity,
            // including when the later policy/schema checks will refuse it.
            if (!string.Equals(Convert.ToString(result.Scalar(create ? "PRAGMA journal_mode=DELETE" : "PRAGMA journal_mode")), "delete", StringComparison.Ordinal)
                || Convert.ToInt64(result.Scalar("PRAGMA synchronous")) != 3)
                throw new InvalidOperationException("learning_durable_sqlite_required");
            if (create) result.Initialize();
            result.RequireSchemaVersion();
            if (Convert.ToString(result.Scalar("PRAGMA quick_check")) != "ok"
                || result.Scalar("PRAGMA foreign_key_check") is not null
                || Convert.ToInt64(result.Scalar("""
                    SELECT count(*) FROM jobs j JOIN epochs e ON j.boot=e.boot
                    WHERE j.reserved_ns<e.first_ns OR j.reserved_ns>e.last_ns
                    """)) != 0
                || Convert.ToString(result.Scalar("SELECT policy FROM configuration WHERE id=1")) != policy.Sha256)
                throw new InvalidOperationException("learning_ledger_identity_or_integrity");
            result.Transaction((time, tx) => 0);
            return result;
        }
        catch (Exception ex)
        {
            db?.Dispose(); files.Dispose();
            if (ex is InvalidOperationException or ArgumentException) throw;
            throw new InvalidOperationException("learning_ledger_open_refused", ex);
        }
    }

    private SqliteCommand Command(string sql, SqliteTransaction? tx, params (string Name, object Value)[] parameters)
    {
        var cmd = db.CreateCommand(); cmd.CommandText = sql; cmd.Transaction = tx;
        foreach (var (name, value) in parameters) cmd.Parameters.AddWithValue(name, value);
        return cmd;
    }
    private object? Scalar(string sql, SqliteTransaction? tx = null, params (string Name, object Value)[] parameters)
    { using var cmd = Command(sql, tx, parameters); return cmd.ExecuteScalar(); }
    private int Execute(string sql, SqliteTransaction? tx = null, params (string Name, object Value)[] parameters)
    { using var cmd = Command(sql, tx, parameters); return cmd.ExecuteNonQuery(); }

    private LearningInstant ValidNow()
    {
        var now = clock.Now;
        if (!Guid.TryParseExact(now.Boot, "D", out _) || now.Nanoseconds < 0)
            throw new InvalidOperationException("learning_clock_invalid");
        return now;
    }
    private T Transaction<T>(Func<LearningInstant, SqliteTransaction, T> action)
    {
        // SQLite canonicalizes paths and requires a stable live pathname.
        // A renamed/replaced root is an explicit refusal, never a redirection.
        files.AssertPathIdentity();
        using var tx = db.BeginTransaction(deferred: false);
        RequireSchemaVersion(tx);
        if (Scalar("SELECT boot FROM configuration WHERE id=1", tx) is not string previous
            || !Guid.TryParseExact(previous, "D", out _))
            throw new InvalidOperationException("learning_ledger_epoch_integrity");
        // The prior epoch must exist even when this call observes a new boot.
        _ = Epoch(previous, tx);
        // Check persisted charges before advancing the epoch: a later clock
        // must not make an invalid future timestamp appear valid. This also
        // covers already-open connections before any budget-affecting action.
        RequirePromotionIntegrity(tx);
        var run = ReadRun(tx);
        LearningInstant now;
        try { now = ValidNow(); }
        catch (Exception error) when (error is OverflowException || error is InvalidOperationException
            { Message: "learning_clock_invalid" or "learning_boot_clock_unavailable" })
        {
            CommitRunClockFailure(run, "run_clock_invalid", tx);
            throw;
        }
        if (previous != now.Boot)
        {
            if (Convert.ToInt64(Scalar("SELECT count(*) FROM epochs WHERE boot=$boot", tx, ("$boot", now.Boot))) != 0)
            {
                CommitRunClockFailure(run, "run_boot_changed", tx);
                throw new InvalidOperationException("learning_boot_identity_reused");
            }
            Execute("INSERT INTO epochs VALUES($boot,$now,$now)", tx,
                ("$boot", now.Boot), ("$now", now.Nanoseconds));
            if (Execute("UPDATE configuration SET boot=$boot WHERE id=1", tx, ("$boot", now.Boot)) != 1)
                throw new InvalidOperationException("learning_ledger_epoch_integrity");
        }
        if (now.Nanoseconds < Epoch(now.Boot, tx).Last)
        {
            CommitRunClockFailure(run, "run_clock_rollback", tx);
            throw new InvalidOperationException("learning_clock_rollback");
        }
        if (Execute("UPDATE epochs SET last_ns=$now WHERE boot=$boot", tx,
            ("$boot", now.Boot), ("$now", now.Nanoseconds)) != 1)
            throw new InvalidOperationException("learning_ledger_epoch_integrity");
        var result = action(now, tx);
        tx.Commit(); // Refused reservations still persist the observed reboot/time.
        return result;
    }
    private (long First, long Last) Epoch(string boot, SqliteTransaction tx)
    {
        using var cmd = Command("SELECT first_ns,last_ns FROM epochs WHERE boot=$boot", tx, ("$boot", boot));
        using var reader = cmd.ExecuteReader();
        if (!reader.Read() || reader.GetValue(0) is not long first || reader.GetValue(1) is not long last
            || first < 0 || last < first || reader.Read())
            throw new InvalidOperationException("learning_ledger_epoch_integrity");
        return (first, last);
    }
    private void Dataset(string dataset)
    {
        if (!policy.Datasets.Any(d => d.Id == dataset)) throw new ArgumentException("learning_dataset_not_authorized");
    }
    internal static bool IsHash(string hash) => hash is { Length: 64 }
        && hash.All(c => c is >= '0' and <= '9' or >= 'a' and <= 'f');
    public void RecordDemand(string dataset, byte key, bool missed)
    {
        Dataset(dataset);
        Transaction((time, tx) => Execute("""
            INSERT INTO demand VALUES($dataset,$key,1,$miss)
            ON CONFLICT(dataset,key) DO UPDATE SET
                requests=CASE WHEN requests=9223372036854775807 THEN requests ELSE requests+1 END,
                misses=CASE WHEN misses=9223372036854775807 THEN misses ELSE misses+$miss END
            """, tx, ("$dataset", dataset), ("$key", (int)key), ("$miss", missed ? 1 : 0)));
    }
    public (long Requests, long Misses) Demand(string dataset, byte key)
    {
        Dataset(dataset);
        files.AssertPathIdentity();
        using var cmd = Command("SELECT requests,misses FROM demand WHERE dataset=$dataset AND key=$key", null,
            ("$dataset", dataset), ("$key", (int)key));
        using var reader = cmd.ExecuteReader();
        return reader.Read() ? (reader.GetInt64(0), reader.GetInt64(1)) : (0, 0);
    }
    public LearningReservation Reserve(string dataset, string sourceSha256)
    {
        policy.RequireEnabled(); Dataset(dataset);
        if (!IsHash(sourceSha256)) throw new ArgumentException("learning_source_hash_required");
        return Transaction((now, tx) =>
        {
            string? reason = null;
            if (Convert.ToInt64(Scalar("SELECT paused FROM configuration WHERE id=1", tx)) != 0) reason = "learning_paused";
            else if (Convert.ToInt64(Scalar("SELECT count(*) FROM jobs", tx)) >= policy.MaxJobs) reason = "job_capacity_exhausted";
            else if (Convert.ToInt64(Scalar("SELECT count(*) FROM jobs WHERE dataset=$d AND source=$s", tx,
                ("$d", dataset), ("$s", sourceSha256))) >= policy.MaxAttemptsPerSource) reason = "source_attempts_exhausted";
            else if (Convert.ToInt64(Scalar("SELECT count(*) FROM jobs WHERE dataset=$d AND source=$s AND state<>'failed'", tx,
                ("$d", dataset), ("$s", sourceSha256))) != 0) reason = "source_already_pending";
            else if (RecentAttempts(now, tx) >= policy.AttemptsPerHour) reason = "attempt_budget_exhausted";
            if (reason is not null) return new LearningReservation(null, reason);
            Execute("INSERT INTO jobs(dataset,source,boot,reserved_ns,state,reason) VALUES($d,$s,$b,$t,'reserved','reserved')", tx,
                ("$d", dataset), ("$s", sourceSha256), ("$b", now.Boot), ("$t", now.Nanoseconds));
            var id = Convert.ToInt64(Scalar("SELECT last_insert_rowid()", tx));
            return new LearningReservation(new(id, dataset, sourceSha256), "reserved");
        });
    }
    private long RecentAttempts(LearningInstant now, SqliteTransaction tx)
    {
        const long window = 3600L * 1_000_000_000;
        var first = Epoch(now.Boot, tx).First;
        return Convert.ToInt64(Scalar("""
            SELECT count(*) FROM jobs WHERE (boot=$b AND reserved_ns>$cutoff)
                OR (boot<>$b AND $quarantine=1)
            """, tx, ("$b", now.Boot), ("$cutoff", now.Nanoseconds - window),
                ("$quarantine", now.Nanoseconds - first < window ? 1 : 0)));
    }
    public void FailJob(long id, string reason)
    {
        if (id < 1 || !LearningPolicy.IsId(reason)) throw new ArgumentException("learning_job_failure_identity");
        Transaction((now, tx) =>
        {
            if (Execute("""
                UPDATE jobs SET state='failed',reason=$r WHERE id=$id AND state='reserved'
                    AND NOT EXISTS(SELECT 1 FROM intents WHERE job=$id)
                """, tx,
                ("$r", reason), ("$id", id)) != 1) throw new InvalidOperationException("learning_job_state_conflict");
            return 0;
        });
    }
    public long JobCount { get { files.AssertPathIdentity(); return Convert.ToInt64(Scalar("SELECT count(*) FROM jobs")); } }
    public void Dispose() { db.Dispose(); files.Dispose(); }
}
