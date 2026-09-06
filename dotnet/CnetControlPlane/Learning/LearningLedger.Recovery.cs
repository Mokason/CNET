namespace CnetControlPlane.Learning;

internal sealed partial class LearningLedger
{
    // An exact durable Before status proves this publication has not survived.
    // The supervisor may withdraw stale evidence or stop without sending ACTIVATE.
    // Keep its promotion charge; the staged candidate still requires DISCARD.
    // Expected (already published) must instead use native token reconciliation.
    internal void RefuseUnpublishedActivation(long id, ControlStatus observed, string reason = "evidence_changed")
    {
        if (reason is not ("evidence_changed" or "owner_stopped"))
            throw new ArgumentException("learning_activation_withdrawal_reason");
        _ = Frame(observed);
        var refused = Transaction((now, tx) =>
        {
            var intent = Pending(tx);
            if (intent is null || intent.Id != id || intent.Kind != "activate")
                throw new InvalidOperationException("learning_native_intent_missing");
            NoProbation(tx);
            if (!Observe(observed, tx)) return false;
            if (observed != intent.Before) { SetPaused(tx); return false; }
            RequireState(intent.JobId, tx, "evaluated");
            Execute("UPDATE intents SET state='refused' WHERE id=$i; UPDATE jobs SET reason=$r WHERE id=$j", tx,
                ("$i", id), ("$j", intent.JobId), ("$r", reason));
            return true;
        });
        if (!refused) throw new InvalidOperationException("learning_native_state_changed");
    }
}
