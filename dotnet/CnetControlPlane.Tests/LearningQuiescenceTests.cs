using System.Diagnostics;
using System.Net.Sockets;
using System.Runtime.Versioning;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using CnetControlPlane.Learning;
using Microsoft.Data.Sqlite;
using Xunit;

namespace CnetControlPlane.Tests;

[SupportedOSPlatform("linux")]
public sealed class LearningQuiescenceTests : IDisposable
{
    private const UnixFileMode Private = UnixFileMode.UserRead | UnixFileMode.UserWrite | UnixFileMode.UserExecute;
    private readonly string root;
    private readonly LearningRuntime runtime;
    private readonly LearningPolicy policy = LearningPolicy.Parse(Encoding.UTF8.GetBytes(LearningPolicyTests.Valid));
    private readonly Clock clock = new();
    private Process? daemon;
    private string Work => Path.Combine(root, "work");
    private string AskSocket => Path.Combine(root, "ipc/ask.sock");
    private string ControlSocket => Path.Combine(root, "ipc/control.sock");
    private static byte[] Source => Encoding.ASCII.GetBytes(
        "CNET_LOCAL_TABLE_V1\ndataset calibration\nauthority verified_tool\ninput_bits 8\noutput_bits 16\nrows 1\n7\t42\n");
    private sealed class Clock : ILearningClock
    {
        public LearningInstant Now => new("00000000-0000-0000-0000-000000000001", 100_000_000_000);
    }

    public LearningQuiescenceTests()
    {
        string[] names = ["cnet_table_capsule", "cnet_table_verify", "cnet_learning_snapshot", "cnet_capsulectl", "cnetd", "libcnet_capsule_core.so"];
        var repo = LearningTestRepository.RequireBuilt(names);
        root = Directory.CreateTempSubdirectory("cnet-quiescence-").FullName;
        File.SetUnixFileMode(root, Private);
        try
        {
            foreach (var path in new[] { "native", "work", "work/data", "work/jobs", "work/sets", "work/frozen", "work/state", "work/state/snapshots", "ipc", "packs" })
                Directory.CreateDirectory(Path.Combine(root, path), Private);
            var hashes = new Dictionary<string, string>();
            foreach (var name in names)
            {
                var target = Path.Combine(root, "native", name);
                File.Copy(Path.Combine(repo, "bin", name), target);
                File.SetUnixFileMode(target, name.EndsWith(".so", StringComparison.Ordinal) ? UnixFileMode.UserRead : UnixFileMode.UserRead | UnixFileMode.UserExecute);
                hashes.Add(name, Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(target))).ToLowerInvariant());
            }
            runtime = LearningRuntime.Load(Path.Combine(root, "native"), JsonSerializer.SerializeToUtf8Bytes(new { schema_version = 1, files = hashes }));
            File.WriteAllBytes(Path.Combine(Work, "data/calibration.tsv"), Source);
            File.SetUnixFileMode(Path.Combine(Work, "data/calibration.tsv"), UnixFileMode.UserRead | UnixFileMode.UserWrite);
        }
        catch { Cleanup(); throw; }
    }
    private void Cleanup()
    {
        foreach (var directory in Directory.EnumerateDirectories(root, "*", SearchOption.AllDirectories)) File.SetUnixFileMode(directory, Private);
        Directory.Delete(root, true);
    }
    public void Dispose()
    {
        if (daemon is not null)
        {
            if (!daemon.HasExited) daemon.Kill();
            daemon.WaitForExit(); daemon.Dispose();
        }
        runtime.Dispose(); Cleanup();
    }
    private LearningControlClient Control() => new(runtime, ControlSocket, 5);
    private LearningSupervisor Supervisor() => new(runtime, policy, Work, ControlSocket, AskSocket, clock);
    private async Task Start()
    {
        File.WriteAllText(Path.Combine(root, "packs/ROUTES.jsonl"), "{\"pattern\":\"fixture\",\"pack\":\"fixture\"}\n");
        var start = new ProcessStartInfo(runtime.PathFor(LearningNativeCommand.Daemon))
        { WorkingDirectory = root, UseShellExecute = false, RedirectStandardOutput = true, RedirectStandardError = true };
        start.Environment.Clear();
        foreach (var (name, value) in new Dictionary<string, string>
        {
            ["CNET_PACKS_ROOT"] = Path.Combine(root, "packs"), ["CNET_MINIMAL_ROOT"] = root,
            ["CNET_SOCK"] = AskSocket, ["CNET_CAPSULE_CONTROL_SOCK"] = ControlSocket,
            ["CNET_CAPSULE_SETS_DIR"] = Path.Combine(Work, "sets"), ["CNET_CAPSULE_STATE_DIR"] = Path.Combine(Work, "state"),
            ["CNET_CAPSULE_DATA_ROOT"] = Path.Combine(Work, "data"), ["CNET_SELF_ANSWER"] = "0",
            ["CNET_TEACHER_ON_MISS"] = "0", ["CNET_CORE_AUTO_EVOLVE"] = "0",
        }) start.Environment[name] = value;
        daemon = Process.Start(start)!;
        for (var attempt = 0; attempt < 250; attempt++)
        {
            Assert.False(daemon.HasExited, "LEARNING_QUIESCENCE_RED daemon exited");
            if (File.Exists(AskSocket) && File.Exists(ControlSocket)) return;
            await Task.Delay(20);
        }
        Assert.Fail("LEARNING_QUIESCENCE_RED daemon readiness");
    }
    private long Scalar(string sql)
    {
        using var database = new SqliteConnection(new SqliteConnectionStringBuilder
        { DataSource = Path.Combine(Work, "ledger.sqlite"), Mode = SqliteOpenMode.ReadOnly, Pooling = false }.ToString());
        database.Open(); using var command = database.CreateCommand(); command.CommandText = sql;
        return Convert.ToInt64(command.ExecuteScalar());
    }
    private async Task<(ControlStatus Before, LearningIntent? Intent)> Prepare(string state, bool published, CancellationToken cancellation)
    {
        var control = Control(); var before = await control.StatusAsync(cancellation);
        using var ledger = LearningLedger.Create(Work, policy, clock);
        ledger.BindRuntime(runtime); ledger.BindNative(before);
        ledger.RecordDemand("calibration", 7, true);
        var job = ledger.Reserve("calibration", LocalTableReference.Parse(Source, policy.Datasets[0]).SourceSha256).Job!;
        if (state == "reserved") return (before, null);
        var acquisition = new LearningAcquisition(runtime, policy, ledger);
        var candidate = await acquisition.PrepareAsync(job, before.Active!, cancellation);
        var stage = ledger.BeginStage(job.Id, candidate.SetName, candidate.Digest, before);
        if (state == "stage_pending")
        {
            if (published) Assert.Equal(stage.Expected, await control.SendAsync(stage, cancellation));
            return (before, stage);
        }
        Assert.Equal("applied", ledger.ResolveIntent(stage.Id, await control.SendAsync(stage, cancellation)));
        if (state == "staged") return (before, null);
        ledger.RecordEvaluation(job.Id, await acquisition.EvaluateAsync(candidate, candidate.Digest, cancellation));
        if (state == "evaluated") return (before, null);
        if (state == "discard_pending")
        {
            var discard = ledger.BeginDiscard(job.Id, stage.Expected);
            if (published) Assert.Equal(discard.Expected, await control.SendAsync(discard, cancellation));
            return (before, discard);
        }
        var activate = ledger.BeginActivate(job.Id, stage.Expected);
        if (state == "activate_pending")
        {
            if (published) Assert.Equal(activate.Expected, await control.SendAsync(activate, cancellation));
            return (before, activate);
        }
        Assert.Equal("applied", ledger.ResolveIntent(activate.Id, await control.SendAsync(activate, cancellation)));
        if (state == "probation") return (before, null);
        Assert.Equal("rollback_pending", state);
        Assert.Equal("rollback_required", ledger.Probe(activate.Expected, false));
        var rollback = ledger.BeginRollback(activate.Expected);
        if (published) Assert.Equal(rollback.Expected, await control.SendAsync(rollback, cancellation));
        return (before, rollback);
    }

    [Theory]
    [InlineData(false, false)]
    [InlineData(true, false)]
    [InlineData(false, true)]
    [InlineData(true, true)]
    public async Task PendingActivationWithdrawsBeforeOrReconcilesPublishedThenRollsBack(bool published, bool missingSource)
    {
        await Start(); using var deadline = new CancellationTokenSource(TimeSpan.FromSeconds(20));
        var (before, intent) = await Prepare("activate_pending", published, deadline.Token);
        if (missingSource) File.Delete(Path.Combine(Work, "data/calibration.tsv"));
        using var supervisor = Supervisor();
        Assert.Equal(published ? "rolled_back" : "discarded", await supervisor.QuiesceAsync(deadline.Token));
        var after = await Control().StatusAsync(deadline.Token);
        Assert.Equal(before.Active, after.Active); Assert.Null(after.Staged);
        Assert.Equal(before.Revision + (published ? 2UL : 0UL), after.Revision);
        using var ledger = LearningLedger.Open(Work, policy, clock);
        Assert.Null(ledger.PendingIntent); Assert.Null(ledger.Outstanding);
        Assert.Equal("failed", ledger.JobState(1)); Assert.False(ledger.IsPaused);
        Assert.Equal(1, Scalar($"SELECT count(*) FROM promotion_charges WHERE intent={intent!.Id}"));
        Assert.Equal(published ? 1 : 0, Scalar($"SELECT count(*) FROM intents WHERE id={intent.Id} AND state='applied'"));
        Assert.Equal("settled", await supervisor.QuiesceAsync(deadline.Token));
    }

    [Theory]
    [InlineData("")]
    [InlineData("injected")]
    [InlineData("OWNER_STOPPED")]
    [InlineData("owner_stopped\n")]
    public async Task WithdrawalReasonRefusesUnknownValuesWithoutChangingIntentOrCharge(string reason)
    {
        await Start(); using var deadline = new CancellationTokenSource(TimeSpan.FromSeconds(20));
        var (_, intent) = await Prepare("activate_pending", false, deadline.Token);
        using var ledger = LearningLedger.Open(Work, policy, clock);
        Assert.Throws<ArgumentException>(() => ledger.RefuseUnpublishedActivation(intent!.Id, intent.Before, reason));
        Assert.Equal(intent, ledger.PendingIntent); Assert.Equal("evaluated", ledger.JobState(1));
        Assert.Equal(1, Scalar("SELECT count(*) FROM promotion_charges"));
    }

    [Theory]
    [InlineData("reserved")]
    [InlineData("staged")]
    [InlineData("evaluated")]
    public async Task UnpublishedOutstandingWorkTerminatesWithoutActivation(string state)
    {
        await Start(); using var deadline = new CancellationTokenSource(TimeSpan.FromSeconds(20));
        var (before, _) = await Prepare(state, false, deadline.Token);
        using var supervisor = Supervisor();
        Assert.Equal(state == "reserved" ? "settled" : "discarded", await supervisor.QuiesceAsync(deadline.Token));
        Assert.Equal(before, await Control().StatusAsync(deadline.Token));
        using var ledger = LearningLedger.Open(Work, policy, clock);
        Assert.Null(ledger.PendingIntent); Assert.Null(ledger.Outstanding);
        Assert.Equal("failed", ledger.JobState(1)); Assert.Equal(1, ledger.JobCount);
        Assert.False(ledger.IsPaused); Assert.Equal(0, Scalar("SELECT count(*) FROM promotion_charges"));
    }

    [Theory]
    [InlineData("stage_pending", false)]
    [InlineData("stage_pending", true)]
    [InlineData("rollback_pending", false)]
    [InlineData("rollback_pending", true)]
    [InlineData("discard_pending", false)]
    [InlineData("discard_pending", true)]
    public async Task PendingStageAndRollbackRecoverExactRequestsWithoutStartingNewWork(string state, bool published)
    {
        await Start(); using var deadline = new CancellationTokenSource(TimeSpan.FromSeconds(20));
        var (before, _) = await Prepare(state, published, deadline.Token);
        using var supervisor = Supervisor();
        Assert.Equal(state == "rollback_pending" ? "rolled_back" : state == "discard_pending" && published ? "settled" : "discarded",
            await supervisor.QuiesceAsync(deadline.Token));
        var after = await Control().StatusAsync(deadline.Token);
        Assert.Equal(before.Active, after.Active); Assert.Null(after.Staged);
        Assert.Equal(before.Revision + (state == "rollback_pending" ? 2UL : 0UL), after.Revision);
        using var ledger = LearningLedger.Open(Work, policy, clock);
        Assert.Null(ledger.PendingIntent); Assert.Null(ledger.Outstanding); Assert.Equal(1, ledger.JobCount);
        Assert.False(ledger.IsPaused);
    }

    [Fact]
    public async Task PrecancelledStopLeavesPendingActivationAndChargeUntouched()
    {
        await Start(); using var deadline = new CancellationTokenSource(TimeSpan.FromSeconds(20));
        var (_, intent) = await Prepare("activate_pending", false, deadline.Token);
        using var supervisor = Supervisor();
        using var stop = new CancellationTokenSource(); stop.Cancel();
        await Assert.ThrowsAnyAsync<OperationCanceledException>(() => supervisor.QuiesceAsync(stop.Token));
        using var ledger = LearningLedger.Open(Work, policy, clock);
        Assert.Equal(intent, ledger.PendingIntent); Assert.False(ledger.IsPaused);
        Assert.Equal(intent!.Before, await Control().StatusAsync(deadline.Token));
        Assert.Equal(1, Scalar("SELECT count(*) FROM promotion_charges"));
    }

    [Fact]
    public async Task MissingControlRetainsPendingIntentUntilExplicitSuccessfulRetry()
    {
        await Start(); using var deadline = new CancellationTokenSource(TimeSpan.FromSeconds(20));
        var (_, intent) = await Prepare("activate_pending", false, deadline.Token);
        using var supervisor = Supervisor();
        File.Move(ControlSocket, ControlSocket + ".held");
        try { await Assert.ThrowsAsync<InvalidOperationException>(() => supervisor.QuiesceAsync(deadline.Token)); }
        finally { File.Move(ControlSocket + ".held", ControlSocket); }
        using (var ledger = LearningLedger.Open(Work, policy, clock))
        { Assert.Equal(intent, ledger.PendingIntent); Assert.Equal("evaluated", ledger.JobState(1)); }
        Assert.Equal("discarded", await supervisor.QuiesceAsync(deadline.Token));
        Assert.Equal(1, Scalar("SELECT count(*) FROM promotion_charges"));
    }

    [Fact]
    public async Task DifferentNativeActivationTokenFreezesWithoutClaimingSettled()
    {
        await Start(); using var deadline = new CancellationTokenSource(TimeSpan.FromSeconds(20));
        var (_, intent) = await Prepare("activate_pending", false, deadline.Token);
        // Actual owner-side publication has matching snapshots/revision but a
        // different token; STATUS must not bless the ledger's unexecuted tuple.
        var other = intent! with { Request = NativeControlProtocol.FormatActivate(intent!.Before.Revision,
            "different_owner_token", intent.Expected.Active!) };
        Assert.Equal(other.Expected, await Control().SendAsync(other, deadline.Token));
        using var supervisor = Supervisor();
        Assert.Equal("frozen", await supervisor.QuiesceAsync(deadline.Token));
        using var ledger = LearningLedger.Open(Work, policy, clock);
        Assert.Equal(intent, ledger.PendingIntent); Assert.Equal("evaluated", ledger.JobState(1));
        Assert.True(ledger.IsPaused); Assert.NotNull(ledger.Outstanding);
        Assert.Equal(other.Expected, await Control().StatusAsync(deadline.Token));
        Assert.Equal(1, Scalar("SELECT count(*) FROM promotion_charges"));
    }

    private sealed class HeldControl : IAsyncDisposable
    {
        private readonly string path;
        private readonly Socket listener = new(AddressFamily.Unix, SocketType.Stream, ProtocolType.Unspecified);
        private readonly CancellationTokenSource stop = new();
        private readonly Task serving;
        public TaskCompletionSource<bool> Reached { get; } = new(TaskCreationOptions.RunContinuationsAsynchronously);
        public HeldControl(string path, ControlStatus observed, string? replay)
        {
            this.path = path; File.Move(path, path + ".held");
            listener.Bind(new UnixDomainSocketEndPoint(path));
            File.SetUnixFileMode(path, UnixFileMode.UserRead | UnixFileMode.UserWrite); listener.Listen(1);
            serving = Serve(observed, replay);
        }
        private async Task<string> Request(Socket peer)
        {
            var bytes = new byte[160]; var used = 0;
            while (used < bytes.Length)
            {
                var count = await peer.ReceiveAsync(bytes.AsMemory(used), SocketFlags.None, stop.Token);
                Assert.True(count > 0, "quiescence request incomplete"); used += count;
                if (bytes[used - 1] == '\n') return Encoding.ASCII.GetString(bytes, 0, used);
            }
            throw new InvalidOperationException("quiescence request limit");
        }
        private async Task Serve(ControlStatus observed, string? replay)
        {
            try
            {
                // Deterministic transport fault after the already-observed real
                // daemon state. No fabricated publication or elapsed probation.
                var frame = Encoding.ASCII.GetBytes(FormattableString.Invariant(
                    $"OK revision={observed.Revision} active={observed.Active} rollback={observed.Rollback ?? "-"} staged={observed.Staged ?? "-"} durable=1 reason=ok\n"));
                foreach (var expected in replay is null ? new[] { "STATUS\n" } : new[] { "STATUS\n", replay })
                {
                    using var peer = await listener.AcceptAsync(stop.Token);
                    Assert.Equal(expected, await Request(peer));
                    using var stream = new NetworkStream(peer, ownsSocket: false);
                    await stream.WriteAsync(frame, stop.Token);
                }
                using var held = await listener.AcceptAsync(stop.Token);
                Assert.Equal("STATUS\n", await Request(held));
                Reached.TrySetResult(true);
                await Task.Delay(Timeout.Infinite, stop.Token);
            }
            catch (Exception error) when (stop.IsCancellationRequested && error is OperationCanceledException or ObjectDisposedException or SocketException) { }
        }
        public async ValueTask DisposeAsync()
        {
            stop.Cancel(); listener.Dispose();
            try { await serving.WaitAsync(TimeSpan.FromSeconds(3)); }
            finally { File.Delete(path); File.Move(path + ".held", path); stop.Dispose(); }
        }
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task CancellationAfterStopDecisionKeepsRollbackDutyAcrossRestart(bool pendingPublished)
    {
        await Start(); using var deadline = new CancellationTokenSource(TimeSpan.FromSeconds(20));
        var (_, intent) = await Prepare(pendingPublished ? "activate_pending" : "probation", true, deadline.Token);
        var observed = await Control().StatusAsync(deadline.Token);
        // Confirm the exact replay against the real daemon before the transport
        // fixture repeats that response and holds the following STATUS open.
        if (intent is not null) Assert.Equal(observed, await Control().SendAsync(intent, deadline.Token));
        using (var supervisor = Supervisor())
        {
            await using var held = new HeldControl(ControlSocket, observed, intent?.Request);
            using var stop = CancellationTokenSource.CreateLinkedTokenSource(deadline.Token);
            var stopping = supervisor.QuiesceAsync(stop.Token);
            await held.Reached.Task.WaitAsync(TimeSpan.FromSeconds(3));
            Assert.Equal(1, Scalar("SELECT rollback_required FROM probation"));
            Assert.Equal(0, Scalar("SELECT probes FROM probation"));
            stop.Cancel();
            await Assert.ThrowsAnyAsync<OperationCanceledException>(() => stopping);
        }
        using var restarted = Supervisor();
        Assert.Equal("rolled_back", await restarted.QuiesceAsync(deadline.Token));
        using var ledger = LearningLedger.Open(Work, policy, clock);
        Assert.Null(ledger.PendingIntent); Assert.Null(ledger.Outstanding); Assert.False(ledger.IsPaused);
        Assert.Equal("failed", ledger.JobState(1)); Assert.Equal(1, ledger.JobCount);
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task SettledQuiescenceDoesNotReserveDemandOrChangePause(bool paused)
    {
        await Start();
        using (var ledger = LearningLedger.Create(Work, policy, clock))
        { ledger.RecordDemand("calibration", 7, true); if (paused) ledger.Pause(); }
        using var supervisor = Supervisor();
        using var deadline = new CancellationTokenSource(TimeSpan.FromSeconds(15));
        var before = await Control().StatusAsync(deadline.Token);
        Assert.Equal("settled", await supervisor.QuiesceAsync(deadline.Token));
        Assert.Equal("settled", await supervisor.QuiesceAsync(deadline.Token));
        Assert.Equal(before, await Control().StatusAsync(deadline.Token));
        using var checkedLedger = LearningLedger.Open(Work, policy, clock);
        Assert.Equal(paused, checkedLedger.IsPaused); Assert.Equal(0, checkedLedger.JobCount);
        Assert.Null(checkedLedger.PendingIntent); Assert.Null(checkedLedger.Outstanding);
        Assert.Empty(Directory.EnumerateFileSystemEntries(Path.Combine(Work, "jobs")));
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task ProbationIsRolledBackWithoutProbeOrPauseMutation(bool paused)
    {
        await Start(); using (LearningLedger.Create(Work, policy, clock)) { }
        using var supervisor = Supervisor();
        using var deadline = new CancellationTokenSource(TimeSpan.FromSeconds(15));
        var before = await Control().StatusAsync(deadline.Token);
        await supervisor.AskAsync("calibration", 7, deadline.Token);
        Assert.Equal("activated", await supervisor.TickAsync(deadline.Token));
        if (paused) { using var ledger = LearningLedger.Open(Work, policy, clock); ledger.Pause(); }
        // No probation time has passed. Missing ASK proves stop cannot certify
        // through a live probe or turn its failure into fresh acquisition.
        File.Move(AskSocket, AskSocket + ".held");
        try { Assert.Equal("rolled_back", await supervisor.QuiesceAsync(deadline.Token)); }
        finally { File.Move(AskSocket + ".held", AskSocket); }
        var after = await Control().StatusAsync(deadline.Token);
        Assert.Equal(before.Active, after.Active); Assert.Equal(before.Revision + 2, after.Revision);
        Assert.Equal("settled", await supervisor.QuiesceAsync(deadline.Token));
        using var checkedLedger = LearningLedger.Open(Work, policy, clock);
        Assert.Equal(paused, checkedLedger.IsPaused); Assert.Equal("failed", checkedLedger.JobState(1));
        Assert.Null(checkedLedger.PendingIntent); Assert.Null(checkedLedger.Outstanding);
        Assert.Equal(1, Scalar("SELECT count(*) FROM promotion_charges"));
    }
}
