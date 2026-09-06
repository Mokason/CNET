namespace CnetControlPlane.Learning;

internal sealed partial class LearningLedger
{
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
        tx.Commit();
    }
}
