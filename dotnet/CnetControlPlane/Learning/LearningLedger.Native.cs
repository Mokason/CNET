using System.Globalization;
using System.Text;
using Microsoft.Data.Sqlite;

namespace CnetControlPlane.Learning;

internal sealed record LearningIntent(long Id, long JobId, string Kind, string Request,
    ControlStatus Before, ControlStatus Expected);
internal sealed record LearningRecovery(string Action, LearningIntent? Intent);

internal sealed partial class LearningLedger
{
    // Persist the existing canonical protocol, including full uint64 revisions.
    // This validates synthetic internal values too; callers cannot inject frames.
    private static string Frame(ControlStatus status)
    {
        var reason = status.Reason switch { ControlReason.Ok => "ok", ControlReason.Refused => "refused",
            ControlReason.DurabilityUncertain => "durability_uncertain", _ => throw new ArgumentException("learning_native_status") };
        var text = $"{(status.Ok ? "OK" : "ERR")} revision={status.Revision.ToString(CultureInfo.InvariantCulture)} "
            + $"active={status.Active ?? "-"} rollback={status.Rollback ?? "-"} staged={status.Staged ?? "-"} "
            + $"durable={(status.Durable ? 1 : 0)} reason={reason}\n";
        if (NativeControlProtocol.Parse(Encoding.ASCII.GetBytes(text)) != status)
            throw new ArgumentException("learning_native_status");
        return text;
    }
    private static ControlStatus ParseFrame(string frame) => NativeControlProtocol.Parse(Encoding.ASCII.GetBytes(frame));
    private ControlStatus Bound(SqliteTransaction tx) => Scalar("SELECT frame FROM native_binding WHERE id=1", tx) is string text
        ? ParseFrame(text) : throw new InvalidOperationException("learning_native_not_bound");
    private void SaveBound(ControlStatus status, SqliteTransaction tx)
    {
        if (Execute("UPDATE native_binding SET frame=$f WHERE id=1", tx, ("$f", Frame(status))) != 1)
            throw new InvalidOperationException("learning_native_not_bound");
    }
    private void SetPaused(SqliteTransaction tx) => Execute("UPDATE configuration SET paused=1 WHERE id=1", tx);
    // The mismatch path returns, rather than throwing in the transaction, so its
    // durable pause survives. Never adopt an unexplained owner-side revision.
    private bool Observe(ControlStatus status, SqliteTransaction tx)
    {
        _ = Frame(status);
        if (status.Ok && status.Durable && status == Bound(tx)) return true;
        SetPaused(tx); return false;
    }
    private void NoPending(SqliteTransaction tx)
    {
        if (Pending(tx) is not null) throw new InvalidOperationException("learning_native_intent_pending");
    }
    private void NoProbation(SqliteTransaction tx)
    {
        if (Convert.ToInt64(Scalar("SELECT count(*) FROM probation", tx)) != 0)
            throw new InvalidOperationException("learning_probation_active");
    }
    private void MayPromote(SqliteTransaction tx)
    {
        policy.RequireEnabled();
        if (Convert.ToInt64(Scalar("SELECT paused FROM configuration WHERE id=1", tx)) != 0)
            throw new InvalidOperationException("learning_paused");
    }
    private string State(long job, SqliteTransaction? tx) => Scalar("SELECT state FROM jobs WHERE id=$j", tx, ("$j", job)) is string state
        ? state : throw new InvalidOperationException("learning_job_missing");
    private void RequireState(long job, SqliteTransaction tx, params string[] states)
    {
        if (!states.Contains(State(job, tx), StringComparer.Ordinal)) throw new InvalidOperationException("learning_job_state_conflict");
    }
    private string Candidate(long job, SqliteTransaction tx) => Scalar("SELECT digest FROM candidates WHERE job=$j", tx, ("$j", job)) is string hash
        ? hash : throw new InvalidOperationException("learning_candidate_missing");
    private LearningIntent StoreIntent(long job, string kind, string request, ControlStatus before, ControlStatus expected, SqliteTransaction tx)
    {
        Execute("INSERT INTO intents(job,kind,request,before_frame,expected_frame,state) VALUES($j,$k,$r,$b,$e,'pending')", tx,
            ("$j", job), ("$k", kind), ("$r", request), ("$b", Frame(before)), ("$e", Frame(expected)));
        return new(Convert.ToInt64(Scalar("SELECT last_insert_rowid()", tx)), job, kind, request, before, expected);
    }
    private LearningIntent? Pending(SqliteTransaction? tx)
    {
        using var command = Command("SELECT id,job,kind,request,before_frame,expected_frame FROM intents WHERE state='pending'", tx);
        using var reader = command.ExecuteReader();
        return reader.Read() ? new(reader.GetInt64(0), reader.GetInt64(1), reader.GetString(2), reader.GetString(3),
            ParseFrame(reader.GetString(4)), ParseFrame(reader.GetString(5))) : null;
    }
    public LearningIntent? PendingIntent { get { files.AssertPathIdentity(); return Pending(null); } }
    public LearningRecovery Recover(ControlStatus observed)
    {
        _ = Frame(observed);
        return Transaction((now, tx) =>
        {
            var intent = Pending(tx);
            if (!observed.Ok || !observed.Durable) return Freeze();
            if (intent is not null)
            {
                if (observed == intent.Before) return new LearningRecovery("reissue", intent);
                if (observed == intent.Expected)
                {
                    // Volatile stage/discard have no native token and their
                    // exact desired state suffices. Durable mutations require
                    // exact reissue to verify the native last-request tuple.
                    if (intent.Kind is "activate" or "rollback") return new LearningRecovery("reissue", intent);
                    ApplyIntent(intent, tx);
                    return new LearningRecovery("applied", null);
                }
                if (intent.Kind == "activate" && intent.Before.Staged is not null &&
                    observed == intent.Before with { Staged = null })
                {
                    // A durable old revision with no stage proves publication
                    // did not survive. Never recreate its promotion charge.
                    Execute("UPDATE intents SET state='refused' WHERE id=$i", tx, ("$i", intent.Id));
                    LoseStage(intent.JobId, observed, tx);
                    return new LearningRecovery("stage_lost", null);
                }
                return Freeze();
            }
            var before = Bound(tx);
            if (observed == before) return new LearningRecovery("matched", null);
            if (before.Staged is not null && observed == before with { Staged = null })
            {
                if (Scalar("""
                    SELECT c.job FROM candidates c JOIN jobs j ON c.job=j.id
                    WHERE c.digest=$d AND j.state IN('staged','evaluated')
                    """, tx, ("$d", before.Staged)) is not long job)
                    throw new InvalidOperationException("learning_owned_stage_missing");
                LoseStage(job, observed, tx);
                return new LearningRecovery("stage_lost", null);
            }
            return Freeze();

            LearningRecovery Freeze() { SetPaused(tx); return new("frozen", intent); }
        });
    }
    private void LoseStage(long job, ControlStatus observed, SqliteTransaction tx)
    {
        RequireState(job, tx, "staged", "evaluated");
        NoProbation(tx);
        SaveBound(observed, tx);
        Execute("UPDATE jobs SET state='failed',reason='native_stage_lost' WHERE id=$j", tx, ("$j", job));
    }
    public string JobState(long jobId) { files.AssertPathIdentity(); return State(jobId, null); }

    public void BindNative(ControlStatus status)
    {
        _ = Frame(status);
        if (!status.Ok || !status.Durable || status.Active is null) throw new InvalidOperationException("learning_native_unready");
        var matched = Transaction((now, tx) =>
        {
            if (Convert.ToInt64(Scalar("SELECT count(*) FROM native_binding", tx)) != 0) return Observe(status, tx);
            if (status.Staged is not null) throw new InvalidOperationException("learning_native_unowned_stage");
            Execute("INSERT INTO native_binding VALUES(1,$f)", tx, ("$f", Frame(status)));
            return true;
        });
        if (!matched) throw new InvalidOperationException("learning_native_state_changed");
    }

    // The caller has frozen the COMPLETE set with the existing native snapshot
    // algorithm. Worker-provided hashes and success markers are not authority.
    public LearningIntent BeginStage(long jobId, string set, string digest, ControlStatus observed)
    {
        if (!IsHash(digest)) throw new ArgumentException("learning_candidate_hash_required");
        var request = NativeControlProtocol.FormatStage(observed.Revision, set);
        var result = Transaction<LearningIntent?>((now, tx) =>
        {
            NoPending(tx); NoProbation(tx); MayPromote(tx);
            if (!Observe(observed, tx)) return null;
            RequireState(jobId, tx, "reserved");
            if (observed.Staged is not null) throw new InvalidOperationException("learning_native_stage_present");
            Execute("INSERT INTO candidates VALUES($j,$s,$d,NULL)", tx, ("$j", jobId), ("$s", set), ("$d", digest));
            return StoreIntent(jobId, "stage", request, observed, observed with { Staged = digest }, tx);
        });
        return result ?? throw new InvalidOperationException("learning_native_state_changed");
    }

    // The immutable factory result proves the complete finite reference
    // comparison. The supervisor must still require child exit zero, unchanged
    // source before/after execution, binding to the actual native staged view,
    // and verification of old obligations; a digest alone proves none of these.
    public void RecordEvaluation(long jobId, LearningTableEvaluation evaluation)
    {
        ArgumentNullException.ThrowIfNull(evaluation);
        Transaction((now, tx) =>
        {
            NoPending(tx); NoProbation(tx); MayPromote(tx); RequireState(jobId, tx, "staged");
            if (Candidate(jobId, tx) != evaluation.CandidateSha256 || Bound(tx).Staged != evaluation.CandidateSha256 ||
                Convert.ToInt64(Scalar("SELECT count(*) FROM jobs WHERE id=$j AND dataset=$d AND source=$s", tx,
                    ("$j", jobId), ("$d", evaluation.Dataset), ("$s", evaluation.SourceSha256))) != 1)
                throw new InvalidOperationException("learning_evaluation_identity_mismatch");
            Execute("UPDATE candidates SET receipt=$r WHERE job=$j; UPDATE jobs SET state='evaluated',reason='independent_eval' WHERE id=$j", tx,
                ("$r", evaluation.ReceiptSha256), ("$j", jobId));
            return 0;
        });
    }
    private long RecentPromotions(LearningInstant now, SqliteTransaction tx)
    {
        const long window = 86400L * 1_000_000_000;
        if (Scalar("SELECT first_ns FROM epochs WHERE boot=$b", tx, ("$b", now.Boot)) is not long first)
            throw new InvalidOperationException("learning_epoch_missing");
        return Convert.ToInt64(Scalar("""
            SELECT count(*) FROM promotion_charges WHERE (boot=$b AND ns>$cutoff) OR (boot<>$b AND $quarantine=1)
            """, tx, ("$b", now.Boot), ("$cutoff", now.Nanoseconds - window), ("$quarantine", now.Nanoseconds - first < window ? 1 : 0)));
    }
    public LearningIntent BeginActivate(long jobId, ControlStatus observed)
    {
        var result = Transaction<LearningIntent?>((now, tx) =>
        {
            NoPending(tx); NoProbation(tx); MayPromote(tx);
            if (!Observe(observed, tx)) return null;
            RequireState(jobId, tx, "evaluated");
            var candidate = Candidate(jobId, tx);
            if (observed.Staged != candidate || Scalar("SELECT receipt FROM candidates WHERE job=$j", tx, ("$j", jobId)) is not string)
                throw new InvalidOperationException("learning_evaluation_required");
            if (RecentPromotions(now, tx) >= policy.PromotionsPerDay)
                throw new InvalidOperationException("learning_promotion_budget_exhausted");
            var request = NativeControlProtocol.FormatActivate(observed.Revision, Guid.NewGuid().ToString("N"), candidate);
            var intent = StoreIntent(jobId, "activate", request, observed, observed with
                { Revision = observed.Revision + 1, Active = candidate, Rollback = observed.Active, Staged = null }, tx);
            // Charge BEFORE sending. Even refusal, transport loss and restart retain it.
            Execute("INSERT INTO promotion_charges VALUES($i,$b,$n)", tx, ("$i", intent.Id), ("$b", now.Boot), ("$n", now.Nanoseconds));
            return intent;
        });
        return result ?? throw new InvalidOperationException("learning_native_state_changed");
    }

    public LearningIntent BeginRollback(ControlStatus observed)
    {
        var result = Transaction<LearningIntent?>((now, tx) =>
        {
            NoPending(tx);
            if (!Observe(observed, tx)) return null;
            if (Scalar("SELECT job FROM probation WHERE id=1 AND rollback_required=1", tx) is not long job ||
                observed.Rollback is null || observed.Staged is not null)
                throw new InvalidOperationException("learning_rollback_not_ready");
            var request = NativeControlProtocol.FormatRollback(observed.Revision, Guid.NewGuid().ToString("N"));
            return StoreIntent(job, "rollback", request, observed, observed with
                { Revision = observed.Revision + 1, Active = observed.Rollback, Rollback = observed.Active }, tx);
        });
        return result ?? throw new InvalidOperationException("learning_native_state_changed");
    }
    public LearningIntent BeginDiscard(long jobId, ControlStatus observed)
    {
        var result = Transaction<LearningIntent?>((now, tx) =>
        {
            NoPending(tx); NoProbation(tx);
            if (!Observe(observed, tx)) return null;
            RequireState(jobId, tx, "staged", "evaluated");
            var digest = Candidate(jobId, tx);
            if (observed.Staged != digest) throw new InvalidOperationException("learning_native_stage_mismatch");
            return StoreIntent(jobId, "discard", NativeControlProtocol.FormatDiscard(observed.Revision, digest),
                observed, observed with { Staged = null }, tx);
        });
        return result ?? throw new InvalidOperationException("learning_native_state_changed");
    }

    // Only an actual reply to this exact persisted request may resolve an
    // activation/rollback. STATUS alone cannot prove native token deduplication.
    // A transport exception leaves the intent intact for exact reissue.
    public string ResolveIntent(long id, ControlStatus reply)
    {
        _ = Frame(reply);
        return Transaction((now, tx) =>
        {
            var intent = Pending(tx);
            if (intent is null || intent.Id != id) throw new InvalidOperationException("learning_native_intent_missing");
            if (reply != intent.Expected)
            {
                var refusedBefore = reply.Durable && !reply.Ok && reply.Reason == ControlReason.Refused &&
                    reply with { Ok = true, Reason = ControlReason.Ok } == intent.Before;
                if (!refusedBefore || intent.Kind is "rollback" or "discard")
                { SetPaused(tx); return "frozen"; }
                Execute("UPDATE intents SET state='refused' WHERE id=$i", tx, ("$i", id));
                if (intent.Kind == "stage") Execute("UPDATE jobs SET state='failed',reason='stage_refused' WHERE id=$j", tx, ("$j", intent.JobId));
                return "refused";
            }
            ApplyIntent(intent, tx);
            return "applied";
        });
    }

    private void ApplyIntent(LearningIntent intent, SqliteTransaction tx)
    {
        SaveBound(intent.Expected, tx);
        Execute("UPDATE intents SET state='applied' WHERE id=$i", tx, ("$i", intent.Id));
        switch (intent.Kind)
        {
            case "stage":
                RequireState(intent.JobId, tx, "reserved");
                Execute("UPDATE jobs SET state='staged',reason='staged' WHERE id=$j", tx, ("$j", intent.JobId)); break;
            case "activate":
                RequireState(intent.JobId, tx, "evaluated");
                // Use the earliest possible publication time, not recovery time.
                Execute("""
                    UPDATE jobs SET state='probation',reason='probation' WHERE id=$j;
                    INSERT INTO probation SELECT 1,$j,boot,ns,0,0 FROM promotion_charges WHERE intent=$i
                    """, tx, ("$j", intent.JobId), ("$i", intent.Id));
                if (Scalar("SELECT job FROM probation WHERE id=1", tx) is not long probationJob || probationJob != intent.JobId)
                    throw new InvalidOperationException("learning_promotion_charge_missing");
                break;
            case "rollback":
                RequireState(intent.JobId, tx, "probation");
                Execute("DELETE FROM probation; UPDATE jobs SET state='failed',reason='rolled_back' WHERE id=$j", tx, ("$j", intent.JobId)); break;
            case "discard":
                RequireState(intent.JobId, tx, "staged", "evaluated");
                Execute("UPDATE jobs SET state='failed',reason='candidate_discarded' WHERE id=$j", tx, ("$j", intent.JobId)); break;
            default: throw new InvalidOperationException("learning_native_intent_corrupt");
        }
    }

    public string Probe(ControlStatus observed, bool passed) => Transaction((now, tx) =>
    {
        NoPending(tx);
        if (!Observe(observed, tx)) return "frozen";
        using var command = Command("SELECT job,boot,last_ns,probes,rollback_required FROM probation WHERE id=1", tx);
        using var reader = command.ExecuteReader();
        if (!reader.Read()) throw new InvalidOperationException("learning_probation_missing");
        var job = reader.GetInt64(0); var boot = reader.GetString(1); var last = reader.GetInt64(2);
        var probes = reader.GetInt32(3); var rollback = reader.GetInt32(4) != 0;
        reader.Close();
        if (rollback || !passed || boot != now.Boot || now.Nanoseconds < last ||
            now.Nanoseconds - last > policy.MaxProbeGapSeconds * 1_000_000_000L)
        {
            Execute("UPDATE probation SET rollback_required=1 WHERE id=1", tx);
            return "rollback_required";
        }
        if (now.Nanoseconds - last < policy.TickSeconds * 1_000_000_000L) return "probe_too_soon";
        if (probes + 1 == policy.ProbationProbes)
        {
            Execute("DELETE FROM probation; UPDATE jobs SET state='accepted',reason='probation_passed' WHERE id=$j", tx, ("$j", job));
            return "accepted";
        }
        Execute("UPDATE probation SET probes=probes+1,last_ns=$n WHERE id=1", tx, ("$n", now.Nanoseconds));
        return "probation";
    });
    public void Pause() => Transaction((now, tx) => { SetPaused(tx); return 0; });
    public void Resume(ControlStatus observed)
    {
        policy.RequireEnabled();
        var ready = Transaction((now, tx) =>
        {
            NoPending(tx);
            if (!Observe(observed, tx)) return false;
            if (Convert.ToInt64(Scalar("SELECT count(*) FROM probation WHERE rollback_required=1", tx)) != 0)
                throw new InvalidOperationException("learning_rollback_required");
            Execute("UPDATE configuration SET paused=0 WHERE id=1", tx); return true;
        });
        if (!ready) throw new InvalidOperationException("learning_native_state_changed");
    }
}
