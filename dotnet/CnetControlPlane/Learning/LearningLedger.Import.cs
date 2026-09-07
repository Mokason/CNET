namespace CnetControlPlane.Learning;

internal sealed partial class LearningLedger
{
    /// <summary>
    /// Initial owner-approved source publication only, not worker authority.
    /// A digest pins reviewed bytes; it does not certify the truth of labels.
    /// Never overwrites evidence, creates demand or changes policy/run history.
    /// </summary>
    internal LocalTableReference ImportSource(string dataset, string sourcePath, string approvedHash)
    {
        policy.RequireEnabled();
        var authorized = policy.Datasets.SingleOrDefault(d => d.Id == dataset)
            ?? throw new ArgumentException("learning_dataset_not_authorized");
        if (!IsHash(approvedHash)) throw new ArgumentException("learning_source_hash_required");
        if (string.IsNullOrEmpty(sourcePath) || !Path.IsPathFullyQualified(sourcePath))
            throw new ArgumentException("learning_absolute_canonical_path_required");
        // Same lock as the supervisor, before reading/publishing source bytes.
        using var owner = files.AcquireLock("owner.lock");
        files.AssertPathIdentity();
        using var intake = LearningFiles.Open(Path.GetDirectoryName(sourcePath)!);
        var bytes = intake.Read(Path.GetFileName(sourcePath), 4096);
        intake.AssertPathIdentity();
        var reference = LocalTableReference.Parse(bytes, authorized);
        if (reference.SourceSha256 != approvedHash)
            throw new InvalidOperationException("learning_source_approval_mismatch");
        using var data = LearningFiles.Open(Path.Combine(files.FullPath, "data"));
        var target = authorized.Id + ".tsv";
        if (data.ValidateFile(target, allowMissing: true).HasValue)
            throw new InvalidOperationException("learning_source_already_exists");
        // Block ordinary status/demand SQLite writes across the metadata scan
        // AND source publication. No database pages or clock epochs are changed.
        using var tx = db.BeginTransaction(deferred: false);
        RequireSchemaVersion(tx);
        var maximum = policy.MaxStorageMiB * 1024L * 1024;
        var usage = LearningStorage.Measure(files.FullPath, maximum - bytes.Length);
        if (usage.FileCount + usage.DirectoryCount - 1 >= 262144)
            throw new InvalidOperationException("learning_storage_entry_limit");
        files.AssertPathIdentity(); data.AssertPathIdentity();
        data.WriteNew(target, bytes);
        tx.Commit();
        // A post-publication fsync/commit failure is uncertain, not rollback:
        // keep the file, report refusal, and never retry by overwriting it.
        return reference;
    }
}
