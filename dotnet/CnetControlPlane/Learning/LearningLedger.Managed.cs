namespace CnetControlPlane.Learning;

internal sealed partial class LearningLedger
{
    public void BindManaged(LearningRunningManagedRuntime running)
    {
        ArgumentNullException.ThrowIfNull(running);
        running.Verify();
        var matched = Transaction((now, tx) =>
        {
            var old = Scalar("SELECT digest FROM managed_binding WHERE id=1", tx) as string;
            if (old is not null)
            {
                if (old == running.Sha256) return true;
                SetPaused(tx); return false;
            }
            if (Convert.ToInt64(Scalar("SELECT count(*) FROM jobs", tx)) != 0 || ReadRun(tx) is not null)
                throw new InvalidOperationException("learning_managed_unbound_work");
            Execute("INSERT INTO managed_binding VALUES(1,$hash)", tx, ("$hash", running.Sha256));
            return true;
        });
        if (!matched) throw new InvalidOperationException("learning_managed_identity_changed");
    }
    public string? ManagedSha256
    {
        get { files.AssertPathIdentity(); return Scalar("SELECT digest FROM managed_binding WHERE id=1") as string; }
    }
}
