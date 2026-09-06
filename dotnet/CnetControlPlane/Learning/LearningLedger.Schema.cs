using Microsoft.Data.Sqlite;

namespace CnetControlPlane.Learning;

internal sealed partial class LearningLedger
{
    private const int SchemaVersion = 2;

    private void RequireSchemaVersion(SqliteTransaction? tx = null)
    {
        if (Scalar("PRAGMA user_version", tx) is not long version || version != SchemaVersion)
            throw new InvalidOperationException("learning_ledger_schema_version");
    }

    private void RequirePromotionIntegrity(SqliteTransaction tx)
    {
        // Foreign keys cannot detect a deleted child charge. Every activation,
        // including pending/refused attempts, must retain its one durable debit;
        // other native operations never consume the promotion budget. The PK
        // already forbids duplicate charges for the same intent.
        if (Convert.ToInt64(Scalar("""
            SELECT EXISTS(
                SELECT 1 FROM intents i LEFT JOIN promotion_charges c ON c.intent=i.id
                WHERE i.kind='activate' AND c.intent IS NULL
            ) OR EXISTS(
                SELECT 1 FROM promotion_charges c
                LEFT JOIN intents i ON i.id=c.intent LEFT JOIN epochs e ON e.boot=c.boot
                WHERE i.id IS NULL OR i.kind<>'activate' OR e.boot IS NULL
                    OR e.first_ns<0 OR e.last_ns<e.first_ns
                    OR c.ns<e.first_ns OR c.ns>e.last_ns
            )
            """, tx)) != 0)
            throw new InvalidOperationException("learning_ledger_promotion_integrity");
    }

    // One fresh, opt-in schema. There is no migration from legacy scheduler
    // state and no reset-on-open fallback. Accounting and native authority are
    // initialized in the same durable transaction, before the first job.
    private void Initialize()
    {
        using var tx = db.BeginTransaction(deferred: false);
        Execute("""
            CREATE TABLE epochs(boot TEXT NOT NULL PRIMARY KEY, first_ns INTEGER NOT NULL CHECK(first_ns>=0),
                last_ns INTEGER NOT NULL CHECK(last_ns>=first_ns)) STRICT;
            CREATE TABLE configuration(id INTEGER PRIMARY KEY CHECK(id=1), policy TEXT NOT NULL,
                boot TEXT NOT NULL REFERENCES epochs(boot), paused INTEGER NOT NULL CHECK(paused IN(0,1))) STRICT;
            CREATE TABLE demand(dataset TEXT NOT NULL, key INTEGER NOT NULL CHECK(key BETWEEN 0 AND 255),
                requests INTEGER NOT NULL CHECK(requests>=0), misses INTEGER NOT NULL CHECK(misses BETWEEN 0 AND requests),
                PRIMARY KEY(dataset,key)) STRICT;
            CREATE TABLE jobs(id INTEGER PRIMARY KEY AUTOINCREMENT, dataset TEXT NOT NULL, source TEXT NOT NULL,
                boot TEXT NOT NULL REFERENCES epochs(boot), reserved_ns INTEGER NOT NULL CHECK(reserved_ns>=0),
                state TEXT NOT NULL, reason TEXT NOT NULL) STRICT;
            CREATE INDEX jobs_source ON jobs(dataset,source);
            """, tx);
        Execute("""
            CREATE TABLE runtime_binding(id INTEGER PRIMARY KEY CHECK(id=1),
                digest TEXT NOT NULL CHECK(length(digest)=64 AND digest NOT GLOB '*[^0-9a-f]*')) STRICT;
            CREATE TABLE managed_binding(id INTEGER PRIMARY KEY CHECK(id=1),
                digest TEXT NOT NULL CHECK(length(digest)=64 AND digest NOT GLOB '*[^0-9a-f]*')) STRICT;
            CREATE TABLE learning_run(id INTEGER PRIMARY KEY CHECK(id=1), boot TEXT REFERENCES epochs(boot),
                start_ns INTEGER, last_ns INTEGER, tick_count INTEGER NOT NULL CHECK(tick_count>=0),
                state TEXT NOT NULL CHECK(state IN('not_started','running','failed','budget_complete')),
                last_action TEXT NOT NULL CHECK(length(last_action) BETWEEN 1 AND 31),
                CHECK((state='not_started' AND boot IS NULL AND start_ns IS NULL AND last_ns IS NULL
                    AND tick_count=0 AND last_action='not_started') OR
                    (state<>'not_started' AND boot IS NOT NULL AND start_ns IS NOT NULL AND last_ns IS NOT NULL
                    AND start_ns>=0 AND last_ns>=start_ns))) STRICT;
            INSERT INTO learning_run VALUES(1,NULL,NULL,NULL,0,'not_started','not_started');
            CREATE TABLE native_binding(id INTEGER PRIMARY KEY CHECK(id=1), frame TEXT NOT NULL) STRICT;
            CREATE TABLE candidates(job INTEGER PRIMARY KEY REFERENCES jobs(id), set_name TEXT NOT NULL,
                digest TEXT NOT NULL CHECK(length(digest)=64 AND digest NOT GLOB '*[^0-9a-f]*'),
                receipt TEXT CHECK(receipt IS NULL OR (length(receipt)=64 AND receipt NOT GLOB '*[^0-9a-f]*'))) STRICT;
            CREATE TABLE intents(id INTEGER PRIMARY KEY, job INTEGER NOT NULL REFERENCES jobs(id),
                kind TEXT NOT NULL CHECK(kind IN('stage','activate','rollback','discard')),
                request TEXT NOT NULL, before_frame TEXT NOT NULL, expected_frame TEXT NOT NULL,
                state TEXT NOT NULL CHECK(state IN('pending','applied','refused')), UNIQUE(job,kind)) STRICT;
            CREATE UNIQUE INDEX one_pending_intent ON intents((1)) WHERE state='pending';
            CREATE TABLE promotion_charges(intent INTEGER PRIMARY KEY REFERENCES intents(id),
                boot TEXT NOT NULL REFERENCES epochs(boot), ns INTEGER NOT NULL CHECK(ns>=0)) STRICT;
            CREATE TABLE probation(id INTEGER PRIMARY KEY CHECK(id=1), job INTEGER UNIQUE NOT NULL REFERENCES jobs(id),
                boot TEXT NOT NULL REFERENCES epochs(boot), last_ns INTEGER NOT NULL CHECK(last_ns>=0),
                probes INTEGER NOT NULL CHECK(probes BETWEEN 0 AND 32),
                rollback_required INTEGER NOT NULL CHECK(rollback_required IN(0,1))) STRICT;
            """, tx);
        var now = ValidNow();
        Execute("INSERT INTO epochs VALUES($boot,$now,$now); INSERT INTO configuration VALUES(1,$policy,$boot,0)", tx,
            ("$policy", policy.Sha256), ("$boot", now.Boot), ("$now", now.Nanoseconds));
        Execute($"PRAGMA user_version={SchemaVersion}", tx);
        tx.Commit();
    }
}
