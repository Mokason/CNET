using System.Globalization;

namespace CnetControlPlane.Learning;

internal sealed class LearningSupervisor : IDisposable
{
    private readonly LearningRuntime runtime;
    private readonly LearningPolicy policy;
    private readonly string workRoot;
    private readonly LearningFiles files;
    private readonly LearningOwnerLock owner;
    private readonly LearningLedger ledger;
    private readonly LearningControlClient control;
    private readonly LearningAskClient ask;
    private readonly LearningAcquisition acquisition;
    private readonly SemaphoreSlim serial = new(1);
    private const long MiB = 1024 * 1024;

    // Explicitly provisioned private workspace and running private daemon only.
    // No installation, policy creation, daemon launch or runtime upgrade here.
    internal LearningSupervisor(LearningRuntime runtime, LearningPolicy policy, string workRoot,
        string controlSocket, string askSocket, ILearningClock? clock = null)
    {
        policy.RequireEnabled();
        if (policy.AllocatorEnabled) throw new InvalidOperationException("learning_allocator_not_approved");
        this.runtime = runtime; this.policy = policy; this.workRoot = workRoot;
        files = LearningFiles.Open(workRoot);
        try
        {
            owner = files.AcquireLock("owner.lock");
            ledger = LearningLedger.Open(workRoot, policy, clock ?? new LearningClock());
            ledger.BindRuntime(runtime);
            control = new(runtime, controlSocket, Math.Min(policy.WorkerSeconds, 60));
            ask = new(askSocket, policy.WorkerSeconds);
            acquisition = new(runtime, policy, ledger);
        }
        catch
        {
            try { ask?.Dispose(); ledger?.Dispose(); }
            finally
            {
                try { owner?.Dispose(); }
                finally { files.Dispose(); serial.Dispose(); }
            }
            throw;
        }
    }
    private LearningDataset Authorized(string dataset) => policy.Datasets.SingleOrDefault(x => x.Id == dataset)
        ?? throw new ArgumentException("learning_dataset_not_authorized");
    private LocalTableReference Source(LearningDataset dataset)
    {
        using var data = LearningFiles.Open(Path.Combine(workRoot, "data"));
        var reference = LocalTableReference.Parse(data.Read(dataset.Id + ".tsv", 4096), dataset);
        data.AssertPathIdentity(); return reference;
    }
    internal async Task<LearningAskResult> AskAsync(string dataset, byte key, CancellationToken cancellation)
    {
        var authorized = Authorized(dataset);
        await serial.WaitAsync(cancellation).ConfigureAwait(false);
        try
        {
            runtime.Verify();
            var result = await ask.AskAsync(authorized, key, cancellation).ConfigureAwait(false);
            // The answer only creates normalized demand. It never becomes a label.
            ledger.RecordDemand(dataset, key, missed: !result.Verified);
            return result;
        }
        finally { serial.Release(); }
    }
    internal async Task<string> TickAsync(CancellationToken cancellation)
    {
        await serial.WaitAsync(cancellation).ConfigureAwait(false);
        try { return await Tick(cancellation).ConfigureAwait(false); }
        finally { serial.Release(); }
    }
    // Stop existing work without changing the owner's independent pause state.
    // Cancellation/transport uncertainty retains durable recovery obligations.
    internal async Task<string> QuiesceAsync(CancellationToken cancellation)
    {
        await serial.WaitAsync(cancellation).ConfigureAwait(false);
        try { return await Tick(cancellation, stopping: true).ConfigureAwait(false); }
        finally { serial.Release(); }
    }
    private async Task<string> Send(LearningIntent intent, CancellationToken cancellation)
    {
        var reply = await control.SendAsync(intent, cancellation).ConfigureAwait(false);
        return ledger.ResolveIntent(intent.Id, reply);
    }
    private async Task<string> Tick(CancellationToken cancellation, bool stopping = false)
    {
        runtime.Verify(); files.AssertPathIdentity();
        var observed = await control.StatusAsync(cancellation).ConfigureAwait(false);
        if (ledger.NativeStatus is null) ledger.BindNative(observed);
        var recovery = ledger.Recover(observed);
        if (recovery.Action == "frozen") return "frozen";
        if (recovery.Intent is not null)
        {
            if (recovery.Intent.Kind == "activate" && observed == recovery.Intent.Before)
            {
                var pendingJob = ledger.Outstanding?.Job;
                if (pendingJob is null || pendingJob.Id != recovery.Intent.JobId)
                    throw new InvalidOperationException("learning_supervisor_job_integrity");
                var fresh = false;
                if (!stopping)
                {
                    try { fresh = Source(Authorized(pendingJob.Dataset)).SourceSha256 == pendingJob.SourceSha256; }
                    catch (Exception exception) when (exception is ArgumentException or InvalidOperationException or IOException) { }
                }
                if (!fresh)
                {
                    ledger.RefuseUnpublishedActivation(recovery.Intent.Id, observed, stopping ? "owner_stopped" : "evidence_changed");
                    return await Send(ledger.BeginDiscard(pendingJob.Id, observed), cancellation).ConfigureAwait(false) == "applied"
                        ? "discarded" : "frozen";
                }
            }
            var resolved = await Send(recovery.Intent, cancellation).ConfigureAwait(false);
            if (resolved != "applied") return stopping ? "frozen" : resolved;
            if (stopping)
            {
                if (recovery.Intent.Kind == "activate")
                    return await StopProbation(recovery.Intent.Expected, cancellation).ConfigureAwait(false);
                if (recovery.Intent.Kind == "rollback") return "rolled_back";
                if (recovery.Intent.Kind == "discard") return "discarded";
            }
            observed = await control.StatusAsync(cancellation).ConfigureAwait(false);
        }
        var outstanding = ledger.Outstanding;
        if (outstanding is not null)
        {
            switch (outstanding.State)
            {
                case "probation": return stopping
                    ? await StopProbation(observed, cancellation).ConfigureAwait(false)
                    : await Probe(outstanding.Job, observed, cancellation).ConfigureAwait(false);
                case "reserved":
                    // A previous owner died or stopped before admission. Native
                    // sealed workers bind parent death; never reuse partial output.
                    ledger.FailJob(outstanding.Job.Id, stopping ? "owner_stopped" : "owner_interrupted");
                    return stopping ? "settled" : "interrupted";
                case "staged": case "evaluated":
                    return await Send(ledger.BeginDiscard(outstanding.Job.Id, observed), cancellation).ConfigureAwait(false) == "applied"
                        ? "discarded" : "frozen";
            }
        }
        if (stopping) return "settled";
        if (ledger.IsPaused) return "paused";
        var limit = policy.MaxStorageMiB * MiB;
        _ = ledger.MeasureStorage(limit);
        // Stable rotating dataset order is the incumbent. Historical misses
        // count only if current independently supplied evidence can cover them.
        var start = (int)(ledger.JobCount % policy.Datasets.Count);
        var waiting = "idle";
        for (var offset = 0; offset < policy.Datasets.Count; offset++)
        {
            var dataset = policy.Datasets[(start + offset) % policy.Datasets.Count];
            var keys = ledger.DemandedKeys(dataset.Id);
            if (keys.Count == 0) continue;
            LocalTableReference reference;
            try { reference = Source(dataset); }
            catch (Exception exception) when (exception is ArgumentException or InvalidOperationException or IOException)
            { waiting = "evidence_unavailable"; continue; } // Never invent labels or expose raw source text.
            var missing = false;
            foreach (var key in keys.Where(key => reference.ExpectedFor(key).HasValue))
            {
                var reply = await ask.AskAsync(dataset, key, cancellation).ConfigureAwait(false);
                if (!reply.Verified) { missing = true; break; }
                if (reply.Value != reference.ExpectedFor(key)) { ledger.Pause(); return "verified_mismatch"; }
            }
            if (!missing) continue;
            var reservation = ledger.Reserve(dataset.Id, reference.SourceSha256);
            if (reservation.Job is null) { waiting = reservation.Reason; continue; }
            return await Acquire(reservation.Job, observed, cancellation).ConfigureAwait(false);
        }
        return waiting;
    }
    private async Task<string> StopProbation(ControlStatus observed, CancellationToken cancellation)
    {
        // Never count a stop as a successful probe. Persist the rollback duty
        // before the fresh status request can fail or observe cancellation.
        if (ledger.Probe(observed, false) == "frozen") return "frozen";
        observed = await control.StatusAsync(cancellation).ConfigureAwait(false);
        return await Send(ledger.BeginRollback(observed), cancellation).ConfigureAwait(false) == "applied"
            ? "rolled_back" : "frozen";
    }
    private async Task<string> Acquire(LearningJob job, ControlStatus observed, CancellationToken cancellation)
    {
        try
        {
            var candidate = await acquisition.PrepareAsync(job, observed.Active!, cancellation).ConfigureAwait(false);
            var limit = policy.MaxStorageMiB * MiB;
            if (limit - ledger.MeasureStorage(limit).Bytes < candidate.ArtifactBytes + MiB)
                throw new InvalidOperationException("learning_acquisition_storage_headroom");
            observed = await control.StatusAsync(cancellation).ConfigureAwait(false);
            if (await Send(ledger.BeginStage(job.Id, candidate.SetName, candidate.Digest, observed), cancellation).ConfigureAwait(false) != "applied")
                return "stage_refused";
            var evaluation = await acquisition.EvaluateAsync(candidate, candidate.Digest, cancellation).ConfigureAwait(false);
            ledger.RecordEvaluation(job.Id, evaluation);
            if (Source(Authorized(job.Dataset)).SourceSha256 != job.SourceSha256)
                throw new InvalidOperationException("learning_acquisition_source_changed");
            observed = await control.StatusAsync(cancellation).ConfigureAwait(false);
            var result = await Send(ledger.BeginActivate(job.Id, observed), cancellation).ConfigureAwait(false);
            return result == "applied" ? "activated" : result;
        }
        catch (OperationCanceledException) { throw; } // Exact pending intent remains for reconciliation.
        catch (Exception exception) when (exception is ArgumentException or InvalidOperationException or IOException)
        {
            if (ledger.PendingIntent is not null) return "outcome_unknown";
            var state = ledger.JobState(job.Id);
            if (state == "reserved") ledger.FailJob(job.Id, "acquisition_refused");
            else if (state is "staged" or "evaluated")
            {
                var current = await control.StatusAsync(cancellation).ConfigureAwait(false);
                if (await Send(ledger.BeginDiscard(job.Id, current), cancellation).ConfigureAwait(false) != "applied") return "frozen";
            }
            return "acquisition_refused";
        }
    }
    private async Task<string> Probe(LearningJob job, ControlStatus observed, CancellationToken cancellation)
    {
        var passed = true;
        try
        {
            var dataset = Authorized(job.Dataset);
            using var directory = LearningFiles.Open(Path.Combine(workRoot, "jobs/job_" + job.Id.ToString(CultureInfo.InvariantCulture)));
            var reference = LocalTableReference.Parse(directory.Read("source.tsv", 4096), dataset);
            if (reference.SourceSha256 != job.SourceSha256 || Source(dataset).SourceSha256 != job.SourceSha256)
                passed = false;
            else
            {
                var timer = new LearningClock(); var began = timer.Now;
                for (var key = 0; key < 256 && passed; key++)
                {
                    var now = timer.Now;
                    if (now.Boot != began.Boot || now.Nanoseconds < began.Nanoseconds
                        || now.Nanoseconds - began.Nanoseconds >= policy.WorkerSeconds * 1_000_000_000L)
                        throw new InvalidOperationException("learning_probe_deadline");
                    var reply = await ask.AskAsync(dataset, (byte)key, cancellation).ConfigureAwait(false);
                    var expected = reference.ExpectedFor((byte)key);
                    passed = reply.Verified == expected.HasValue && reply.Value == expected;
                }
                if (Source(dataset).SourceSha256 != job.SourceSha256) passed = false;
            }
        }
        catch (OperationCanceledException) { throw; }
        catch (Exception exception) when (exception is ArgumentException or InvalidOperationException or IOException)
        { passed = false; }
        // A known failed observation is itself durable evidence. Record it
        // against the already matched binding BEFORE a later status transport
        // failure or cancellation can interrupt this tick. Native rollback
        // still requires a freshly checked revision; no stale mutation is sent.
        if (!passed && ledger.Probe(observed, false) == "frozen") return "frozen";
        // The real status must still match after all actual resident probes.
        observed = await control.StatusAsync(cancellation).ConfigureAwait(false);
        var result = ledger.Probe(observed, passed);
        if (result != "rollback_required") return result;
        return await Send(ledger.BeginRollback(observed), cancellation).ConfigureAwait(false) == "applied" ? "rolled_back" : "frozen";
    }
    // Caller awaits in-flight calls before disposal; all calls share one ledger lane.
    public void Dispose()
    {
        try { ask.Dispose(); ledger.Dispose(); }
        finally
        {
            try { owner.Dispose(); }
            finally { files.Dispose(); serial.Dispose(); }
        }
    }
}
