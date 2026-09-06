namespace CnetControlPlane.Learning;

internal sealed partial class LearningLedger
{
    internal string WorkRoot => files.FullPath;

    internal LearningStorageUsage MeasureStorage(long maximumBytes)
    {
        files.AssertPathIdentity();
        // Operator status/demand calls are writers too. Hold their SQLite
        // transactions outside the entire metadata scan, including journal
        // creation/removal. Do not use Transaction: its epoch UPDATE would
        // create a measurement-only journal and change the bytes at commit.
        using var tx = db.BeginTransaction(deferred: false);
        RequireSchemaVersion(tx);
        var usage = LearningStorage.Measure(files.FullPath, maximumBytes);
        tx.Commit(); // No dirty pages: commit does not add or remove file bytes.
        return usage;
    }
}
