namespace CnetControlPlane.Learning;

internal sealed partial class LearningLedger
{
    internal LearningExperience FinishExperience(string id, LearningAskResult? answer)
    {
        if (!IsRequestId(id)) throw new ArgumentException("learning_task_identity_refused");
        return Transaction((now, tx) =>
        {
            var row = Experience(id, tx) ?? throw new InvalidOperationException("learning_task_missing");
            if (row.State != "pending") throw new InvalidOperationException("learning_task_already_finished");
            string state;
            var sourceReadable = TryTaskSource(row.Dataset, out var after);
            if (!sourceReadable || after?.SourceSha256 != row.SourceSha256) state = "conflict";
            else if (answer is null || now.Boot != row.Boot) state = "unknown";
            else
            {
                if (answer.Verified &&
                    (row.Expected is null || answer.Value != row.Expected || answer.Text is not null)) state = "conflict";
                else if (answer.Verified) state = "verified";
                else state = row.SourceSha256 is null ? "awaiting_evidence" : row.Expected is null ? "abstain" : "miss";
            }
            if (state == "conflict") SetPaused(tx);
            Execute("""
                UPDATE experiences SET finished_boot=$b,finished_ns=$n,state=$s,value=$v
                WHERE request_id=$id AND state='pending'
                """, tx, ("$b", now.Boot), ("$n", now.Nanoseconds), ("$s", state),
                ("$v", state == "unknown" ? DBNull.Value : (object?)answer?.Value ?? DBNull.Value), ("$id", id));
            // Return normally so mismatch evidence AND pause commit before reporting failure.
            return Experience(id, tx)!;
        });
    }

    internal LearningExperience ApproveExperience(string id, string sourceSha256)
    {
        if (!IsRequestId(id) || !IsHash(sourceSha256)) throw new ArgumentException("learning_task_approval_identity");
        var result = Transaction((now, tx) =>
        {
            var row = Experience(id, tx) ?? throw new InvalidOperationException("learning_task_missing");
            if (row.ReviewConflict) throw new InvalidOperationException("learning_task_review_conflict");
            if (row.ApprovedSourceSha256 is not null)
            {
                if (row.ApprovedSourceSha256 != sourceSha256) throw new InvalidOperationException("learning_task_approval_conflict");
                return row; // A repeated approval cannot create another demand charge.
            }
            RequireTaskAdmission(now, tx);
            if (row.State is not ("miss" or "awaiting_evidence")) throw new InvalidOperationException("learning_task_not_approvable");
            var sourceReadable = TryTaskSource(row.Dataset, out var source);
            var pin = Scalar("SELECT source FROM task_source_pins WHERE dataset=$d", tx, ("$d", row.Dataset)) as string;
            if (!sourceReadable || row.SourceSha256 is not null && row.SourceSha256 != source?.SourceSha256
                || pin is not null && pin != source?.SourceSha256)
            {
                SetPaused(tx);
                Execute("UPDATE experiences SET review_conflict=1 WHERE request_id=$id", tx, ("$id", id));
                return Experience(id, tx)!; // Commit the review conflict without rewriting the historical observation.
            }
            if (source is null) throw new InvalidOperationException("learning_task_evidence_missing");
            if (source.SourceSha256 != sourceSha256)
                throw new InvalidOperationException("learning_task_source_conflict");
            var expected = source.ExpectedFor(row.Key) ?? throw new InvalidOperationException("learning_task_evidence_excludes_key");
            Execute("INSERT OR IGNORE INTO task_source_pins VALUES($d,$s)", tx, ("$d", row.Dataset), ("$s", sourceSha256));
            Execute("""
                UPDATE experiences SET approved_source=$s,approved_expected=$e,approved_boot=$b,approved_ns=$n
                WHERE request_id=$id AND approved_source IS NULL
                """, tx, ("$s", sourceSha256), ("$e", (int)expected), ("$b", now.Boot),
                ("$n", now.Nanoseconds), ("$id", id));
            IncrementDemand(row.Dataset, row.Key, true, tx);
            return Experience(id, tx)!;
        });
        if (result.ReviewConflict) throw new InvalidOperationException("learning_task_source_conflict");
        return result;
    }

    private bool TryTaskSource(string dataset, out LocalTableReference? source)
    {
        try { source = TaskSource(dataset); return true; }
        catch (Exception error) when (error is ArgumentException or InvalidOperationException or IOException or UnauthorizedAccessException)
        {
            source = null;
            return false; // The caller must durably record conflict and pause, never accept a degraded verifier.
        }
    }
}
