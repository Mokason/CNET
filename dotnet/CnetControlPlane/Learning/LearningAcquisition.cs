using System.Globalization;
using System.Text;

namespace CnetControlPlane.Learning;

internal sealed record LearningPreparedCandidate(LearningJob Job, LocalTableReference Reference,
    string SetName, string Digest, long ArtifactBytes);

// Fixed-command acquisition only. The serialized supervisor owns reservations,
// native intents, activation and recovery; this helper never grants activation.
internal sealed class LearningAcquisition(LearningRuntime runtime, LearningPolicy policy,
    LearningLedger ledger, ILearningClock? clock = null)
{
    private const long MiB = 1024 * 1024;
    private readonly string workRoot = ledger.WorkRoot;
    private long Limit => policy.MaxStorageMiB * MiB;
    private string Part(string name) => Path.Combine(workRoot, name);
    private static string JobName(LearningJob job) => "job_" + job.Id.ToString(CultureInfo.InvariantCulture);
    private LearningDataset Authorized(LearningJob job)
    {
        if (job.Id < 1 || !LearningLedger.IsHash(job.SourceSha256)) throw new ArgumentException("learning_acquisition_job");
        return policy.Datasets.SingleOrDefault(x => x.Id == job.Dataset)
            ?? throw new ArgumentException("learning_acquisition_job");
    }
    private LocalTableReference Reference(LearningJob job)
    {
        var authorized = Authorized(job);
        using var data = LearningFiles.Open(Part("data"));
        var reference = LocalTableReference.Parse(data.Read(authorized.Id + ".tsv", 4096), authorized);
        data.AssertPathIdentity();
        if (reference.SourceSha256 != job.SourceSha256) throw new InvalidOperationException("learning_acquisition_source_changed");
        return reference;
    }
    private long Headroom(long required)
    {
        var available = Limit - ledger.MeasureStorage(Limit).Bytes;
        if (available < required) throw new InvalidOperationException("learning_acquisition_storage_headroom");
        return available;
    }
    private async Task<LearningChildResult> Run(LearningNativeCommand command, string[] arguments, CancellationToken cancellation)
    {
        var result = await LearningChild.RunAsync(runtime.PathFor(command), arguments, workRoot,
            new Dictionary<string, string>(), policy.WorkerSeconds, 8192, cancellation, clock).ConfigureAwait(false);
        // No worker success marker is treated as certification. Nonzero exit,
        // uncertainty, diagnostics or transport failure cannot yield a receipt.
        if (result.ExitCode != 0 || !result.Stderr.IsEmpty) throw new InvalidOperationException("learning_acquisition_worker_refused");
        return result;
    }
    private string[] Limits => [Environment.ProcessId.ToString(CultureInfo.InvariantCulture),
        policy.WorkerSeconds.ToString(CultureInfo.InvariantCulture), policy.WorkerMemoryMiB.ToString(CultureInfo.InvariantCulture)];

    internal async Task<LearningPreparedCandidate> PrepareAsync(LearningJob job, string activeDigest,
        CancellationToken cancellation)
    {
        policy.RequireEnabled();
        if (policy.AllocatorEnabled) throw new InvalidOperationException("learning_allocator_not_approved");
        if (!LearningLedger.IsHash(activeDigest)) throw new ArgumentException("learning_acquisition_active_identity");
        cancellation.ThrowIfCancellationRequested();
        var reference = Reference(job);
        // Three fixed producer files each have a native 16MiB file limit.
        // Keep additional space for source and small bookkeeping. This is
        // conservative preflight, not a kernel quota against arbitrary code.
        Headroom(50 * MiB);
        runtime.Verify();
        using var work = LearningFiles.Open(workRoot);
        using var jobs = LearningFiles.Open(Part("jobs"));
        using var directory = jobs.CreateDirectory(JobName(job));
        using (var data = LearningFiles.Open(Part("data")))
        {
            var source = data.Read(job.Dataset + ".tsv", 4096);
            if (LocalTableReference.Parse(source, Authorized(job)).SourceSha256 != job.SourceSha256)
                throw new InvalidOperationException("learning_acquisition_source_changed");
            directory.WriteNew("source.tsv", source);
        }
        using var build = directory.CreateDirectory("build");
        await Run(LearningNativeCommand.BuildTable,
            ["build-worker", Part("data"), job.Dataset, build.FullPath, .. Limits], cancellation).ConfigureAwait(false);
        _ = Reference(job);
        // Copy, frozen preflight and daemon STAGE each need one complete set.
        // Retain failed/old artifacts in measured storage; never auto-delete.
        var available = Headroom(MiB + 3);
        cancellation.ThrowIfCancellationRequested();
        var copied = LearningInventory.CopyAndAppend(Part("state/snapshots/" + activeDigest), build.FullPath,
            Part("sets"), JobName(job), "table_" + job.Id.ToString(CultureInfo.InvariantCulture), (available - MiB) / 3);
        Headroom(checked(2 * copied.ArtifactBytes + MiB));
        var frozen = await Run(LearningNativeCommand.Snapshot,
            ["freeze", copied.FullPath, Part("frozen")], cancellation).ConfigureAwait(false);
        var digest = ParseFrozen(frozen.Stdout.ToArray(), copied.ArtifactBytes);
        _ = Reference(job);
        Headroom(checked(copied.ArtifactBytes + MiB));
        work.AssertPathIdentity(); jobs.AssertPathIdentity(); directory.AssertPathIdentity(); build.AssertPathIdentity();
        return new(job, reference, copied.SetName, digest, copied.ArtifactBytes);
    }

    internal static string ParseFrozen(byte[] bytes, long expectedBytes)
    {
        if (bytes.Length is < 1 or > 256 || bytes.Any(b => b is not (>= 32 and <= 126 or 10)))
            throw new InvalidOperationException("learning_acquisition_snapshot_response");
        var lines = Encoding.ASCII.GetString(bytes).Split('\n');
        if (lines.Length != 5 || lines[0] != "CNET_LEARNING_SNAPSHOT_V1" || lines[3] != "end" || lines[4] != ""
            || lines[1].Length != 80 || !lines[1].StartsWith("snapshot_sha256 ", StringComparison.Ordinal)
            || !LearningLedger.IsHash(lines[1][16..])
            || lines[2] != "bytes " + expectedBytes.ToString(CultureInfo.InvariantCulture)
            || expectedBytes is < 1 or > 4L * 1024 * 1024 * 1024)
            throw new InvalidOperationException("learning_acquisition_snapshot_response");
        return lines[1][16..];
    }

    internal async Task<LearningTableEvaluation> EvaluateAsync(LearningPreparedCandidate candidate,
        string stagedDigest, CancellationToken cancellation)
    {
        policy.RequireEnabled();
        if (!LearningLedger.IsHash(stagedDigest) || stagedDigest != candidate.Digest
            || candidate.Reference.SourceSha256 != candidate.Job.SourceSha256
            || candidate.Reference.Dataset != candidate.Job.Dataset)
            throw new InvalidOperationException("learning_acquisition_evaluation_identity");
        cancellation.ThrowIfCancellationRequested();
        _ = Reference(candidate.Job);
        Headroom(MiB);
        using var work = LearningFiles.Open(workRoot);
        using var directory = LearningFiles.Open(Part("jobs/" + JobName(candidate.Job)));
        var saved = LocalTableReference.Parse(directory.Read("source.tsv", 4096), Authorized(candidate.Job));
        if (saved.SourceSha256 != candidate.Job.SourceSha256) throw new InvalidOperationException("learning_acquisition_source_changed");
        using var output = directory.CreateDirectory("evaluate");
        var result = await Run(LearningNativeCommand.VerifyTable,
            ["snapshot-worker", Part("data"), candidate.Job.Dataset, Part("state/snapshots"), stagedDigest,
                output.FullPath, .. Limits], cancellation).ConfigureAwait(false);
        _ = Reference(candidate.Job);
        var receipt = LearningTableEvaluation.Parse(result.Stdout.ToArray(), saved, stagedDigest);
        directory.WriteNew("evaluation.txt", result.Stdout.AsSpan());
        Headroom(MiB);
        work.AssertPathIdentity(); directory.AssertPathIdentity(); output.AssertPathIdentity();
        return receipt;
    }
}
