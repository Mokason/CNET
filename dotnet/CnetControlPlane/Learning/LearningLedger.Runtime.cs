namespace CnetControlPlane.Learning;

internal sealed partial class LearningLedger
{
    // The supervisor calls this before reserving any work on every start.
    // Changing an installation requires explicit owner migration, not rebinding
    // an old ledger and silently treating old evidence as a new binary's proof.
    public void BindRuntime(LearningRuntime runtime)
    {
        ArgumentNullException.ThrowIfNull(runtime);
        runtime.Verify();
        var matched = Transaction((now, tx) =>
        {
            var old = Scalar("SELECT digest FROM runtime_binding WHERE id=1", tx) as string;
            if (old is not null)
            {
                if (old == runtime.Sha256) return true;
                SetPaused(tx);
                return false; // Commit the pause before reporting the mismatch.
            }
            if (Convert.ToInt64(Scalar("SELECT count(*) FROM jobs", tx)) != 0)
                throw new InvalidOperationException("learning_runtime_unbound_jobs");
            Execute("INSERT INTO runtime_binding VALUES(1,$h)", tx, ("$h", runtime.Sha256));
            return true;
        });
        if (!matched) throw new InvalidOperationException("learning_runtime_identity_changed");
    }
    public string? RuntimeSha256
    {
        get { files.AssertPathIdentity(); return Scalar("SELECT digest FROM runtime_binding WHERE id=1") as string; }
    }
}
