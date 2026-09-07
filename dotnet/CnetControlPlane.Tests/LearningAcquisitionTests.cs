using System.Runtime.Versioning;
using System.Diagnostics;
using System.Net.Sockets;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using CnetControlPlane.Learning;
using Microsoft.Data.Sqlite;
using Xunit;

namespace CnetControlPlane.Tests;

[SupportedOSPlatform("linux")]
public sealed class LearningAcquisitionTests : IDisposable
{
    private const UnixFileMode Private = UnixFileMode.UserRead | UnixFileMode.UserWrite | UnixFileMode.UserExecute;
    private readonly string root;
    private readonly LearningRuntime runtime;
    private readonly LearningPolicy policy;
    private Process? daemon;
    private string Work => Path.Combine(root, "work");
    private static byte[] Source(int value = 42) => Encoding.ASCII.GetBytes(
        "CNET_LOCAL_TABLE_V1\ndataset calibration\nauthority verified_tool\ninput_bits 8\noutput_bits 16\nrows 3\n0\t65535\n7\t" + value + "\n19\t0\n");
    public LearningAcquisitionTests()
    {
        // Resolve prerequisites before creating private artifacts, including
        // when dotnet uses an unrelated --artifacts-path.
        string[] nativeNames = ["cnet_table_capsule", "cnet_table_verify", "cnet_learning_snapshot", "cnet_capsulectl", "cnetd", "libcnet_capsule_core.so"];
        var repo = LearningTestRepository.RequireBuilt(nativeNames);
        root = Directory.CreateTempSubdirectory("cnet-acquisition-").FullName;
        File.SetUnixFileMode(root, Private);
        try
        {
            foreach (var path in new[] { "native", "work", "work/data", "work/jobs", "work/sets", "work/frozen", "work/state", "work/state/snapshots", "work/empty" })
                Directory.CreateDirectory(Path.Combine(root, path), Private);
            var hashes = new Dictionary<string, string>();
            foreach (var name in nativeNames)
            {
                var target = Path.Combine(root, "native", name);
                File.Copy(Path.Combine(repo, "bin", name), target);
                File.SetUnixFileMode(target, name.EndsWith(".so", StringComparison.Ordinal) ? UnixFileMode.UserRead : UnixFileMode.UserRead | UnixFileMode.UserExecute);
                hashes.Add(name, Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(target))).ToLowerInvariant());
            }
            runtime = LearningRuntime.Load(Path.Combine(root, "native"), JsonSerializer.SerializeToUtf8Bytes(new { schema_version = 1, files = hashes }));
            policy = LearningPolicy.Parse(Encoding.UTF8.GetBytes(LearningPolicyTests.Valid));
            WriteSource(Source());
        }
        catch { Cleanup(); throw; }
    }
    private void WriteSource(byte[] bytes)
    {
        var path = Path.Combine(Work, "data/calibration.tsv");
        File.WriteAllBytes(path, bytes); File.SetUnixFileMode(path, UnixFileMode.UserRead | UnixFileMode.UserWrite);
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
            if (!daemon.HasExited) { daemon.Kill(); daemon.WaitForExit(5000); }
            daemon.Dispose();
        }
        runtime.Dispose(); Cleanup();
    }
    private LearningJob Job(long id = 1) => new(id, "calibration", LocalTableReference.Parse(Source(), policy.Datasets[0]).SourceSha256);
    private LearningLedger CreateLedger(LearningPolicy? selected = null) => LearningLedger.Create(Work, selected ?? policy, new LearningClock());
    private async Task<string> FreezeEmpty()
    {
        var result = await LearningChild.RunAsync(runtime.PathFor(LearningNativeCommand.Snapshot),
            ["freeze", Path.Combine(Work, "empty"), Path.Combine(Work, "state/snapshots")], Work,
            new Dictionary<string, string>(), 5, 8192, default);
        Assert.Equal(0, result.ExitCode);
        return Encoding.ASCII.GetString(result.Stdout.AsSpan()).Split('\n')[1][16..];
    }

    [Fact]
    public async Task ActualSealedAcquisitionFreezesAndIndependentlyEvaluatesAllKeys()
    {
        using var ledger = CreateLedger();
        var acquisition = new LearningAcquisition(runtime, policy, ledger);
        var candidate = await acquisition.PrepareAsync(Job(), await FreezeEmpty(), default);
        Assert.Equal(Job(), candidate.Job);
        Assert.Equal(Job().SourceSha256, candidate.Reference.SourceSha256);
        Assert.Equal("job_1", candidate.SetName);
        Assert.True(candidate.ArtifactBytes > 0);
        // A real daemon copies the same named set to its snapshot cache at STAGE.
        // This test exercises the real native freeze format, not control authority.
        var staged = await LearningChild.RunAsync(runtime.PathFor(LearningNativeCommand.Snapshot),
            ["freeze", Path.Combine(Work, "sets", candidate.SetName), Path.Combine(Work, "state/snapshots")],
            Work, new Dictionary<string, string>(), 5, 8192, default);
        Assert.Equal(0, staged.ExitCode);
        Assert.Contains(candidate.Digest, Encoding.ASCII.GetString(staged.Stdout.AsSpan()));
        var receipt = await acquisition.EvaluateAsync(candidate, candidate.Digest, default);
        Assert.Equal(candidate.Digest, receipt.CandidateSha256);
        Assert.Equal(Job().SourceSha256, receipt.SourceSha256);
        Assert.Equal(Source(), File.ReadAllBytes(Path.Combine(Work, "jobs/job_1/source.tsv")));
    }

    [Fact]
    public async Task StaleReservedSourceRefusesBeforeCreatingAJobDirectory()
    {
        using var ledger = CreateLedger();
        var initial = await FreezeEmpty(); WriteSource(Source(43));
        var exception = await Assert.ThrowsAsync<InvalidOperationException>(() =>
            new LearningAcquisition(runtime, policy, ledger).PrepareAsync(Job(), initial, default));
        Assert.Equal("learning_acquisition_source_changed", exception.Message);
        Assert.Empty(Directory.EnumerateFileSystemEntries(Path.Combine(Work, "jobs")));
    }

    [Fact]
    public async Task ConservativeWorkerHeadroomRefusesBeforeLaunchingOrWriting()
    {
        var small = LearningPolicy.Parse(Encoding.UTF8.GetBytes(LearningPolicyTests.Valid.Replace("\"max_storage_mib\":256", "\"max_storage_mib\":16")));
        using var ledger = CreateLedger(small);
        var exception = await Assert.ThrowsAsync<InvalidOperationException>(() =>
            new LearningAcquisition(runtime, small, ledger).PrepareAsync(Job(), new string('0', 64), default));
        Assert.Equal("learning_acquisition_storage_headroom", exception.Message);
        Assert.Empty(Directory.EnumerateFileSystemEntries(Path.Combine(Work, "jobs")));
    }

    [Fact]
    public async Task ChangedSourceAfterPreparationCannotProduceAnEvaluationReceipt()
    {
        using var ledger = CreateLedger();
        var acquisition = new LearningAcquisition(runtime, policy, ledger);
        var candidate = await acquisition.PrepareAsync(Job(), await FreezeEmpty(), default);
        WriteSource(Source(43));
        var exception = await Assert.ThrowsAsync<InvalidOperationException>(() => acquisition.EvaluateAsync(candidate, candidate.Digest, default));
        Assert.Equal("learning_acquisition_source_changed", exception.Message);
        Assert.False(Directory.Exists(Path.Combine(Work, "jobs/job_1/evaluate")));
    }

    [Theory]
    [InlineData("leading_zero")]
    [InlineData("wrong_bytes")]
    [InlineData("uppercase_hash")]
    [InlineData("extra_line")]
    [InlineData("missing_lf")]
    [InlineData("carriage_return")]
    public void FrozenResponseRequiresExactCanonicalInventorySizeAndCompleteFraming(string mutation)
    {
        var hash = new string('a', 64);
        var text = $"CNET_LEARNING_SNAPSHOT_V1\nsnapshot_sha256 {hash}\nbytes 17\nend\n";
        Assert.Equal(hash, LearningAcquisition.ParseFrozen(Encoding.ASCII.GetBytes(text), 17));
        text = mutation switch
        {
            "leading_zero" => text.Replace("bytes 17", "bytes 017"),
            "wrong_bytes" => text.Replace("bytes 17", "bytes 18"),
            "uppercase_hash" => text.Replace(hash, hash.ToUpperInvariant()),
            "extra_line" => text + "ok\n",
            "missing_lf" => text[..^1],
            _ => text.Replace("\n", "\r\n"),
        };
        Assert.Throws<InvalidOperationException>(() => LearningAcquisition.ParseFrozen(Encoding.ASCII.GetBytes(text), 17));
    }

    [Fact]
    public async Task ReusingAChargedJobNeverOverwritesItsArtifacts()
    {
        using var ledger = CreateLedger();
        var acquisition = new LearningAcquisition(runtime, policy, ledger);
        var initial = await FreezeEmpty();
        var candidate = await acquisition.PrepareAsync(Job(), initial, default);
        var saved = File.ReadAllBytes(Path.Combine(Work, "jobs/job_1/source.tsv"));
        await Assert.ThrowsAsync<InvalidOperationException>(() => acquisition.PrepareAsync(Job(), initial, default));
        Assert.Equal(saved, File.ReadAllBytes(Path.Combine(Work, "jobs/job_1/source.tsv")));
        Assert.Single(Directory.EnumerateDirectories(Path.Combine(Work, "sets")));
        Assert.True(Directory.Exists(Path.Combine(Work, "frozen", candidate.Digest)));
    }

    [Fact]
    public async Task PrecancelledAcquisitionHasNoOutputSideEffect()
    {
        using var ledger = CreateLedger();
        using var stop = new CancellationTokenSource(); stop.Cancel();
        await Assert.ThrowsAnyAsync<OperationCanceledException>(() =>
            new LearningAcquisition(runtime, policy, ledger).PrepareAsync(Job(), new string('a', 64), stop.Token));
        Assert.Empty(Directory.EnumerateFileSystemEntries(Path.Combine(Work, "jobs")));
    }

    private sealed class FakeClock : ILearningClock
    {
        public LearningInstant Now { get; private set; } = new("00000000-0000-0000-0000-000000000001", 100_000_000_000);
        public void Advance(int seconds) => Now = Now with { Nanoseconds = Now.Nanoseconds + seconds * 1_000_000_000L };
    }
    private string AskSocket => Path.Combine(root, "ipc/ask.sock");
    private string ControlSocket => Path.Combine(root, "ipc/control.sock");
    private async Task StartDaemon()
    {
        Directory.CreateDirectory(Path.Combine(root, "ipc"), Private);
        Directory.CreateDirectory(Path.Combine(root, "packs"), Private);
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
            Assert.False(daemon.HasExited, "LEARNING_SUPERVISOR_RED private daemon exited");
            if (File.Exists(AskSocket) && File.Exists(ControlSocket)) return;
            await Task.Delay(20);
        }
        Assert.Fail("LEARNING_SUPERVISOR_RED private daemon did not become ready");
    }

    [Fact]
    public async Task ManagedDemandPromotesProbatesRefreshesAndRollsBackActualResidentCapsules()
    {
        await StartDaemon();
        var clock = new FakeClock();
        using (LearningLedger.Create(Work, policy, clock)) { }
        using var supervisor = new LearningSupervisor(runtime, policy, Work, ControlSocket, AskSocket, clock);
        using var deadline = new CancellationTokenSource(TimeSpan.FromSeconds(20));
        Assert.False((await supervisor.AskAsync("calibration", 7, deadline.Token)).Verified);
        Assert.Equal("activated", await supervisor.TickAsync(deadline.Token));
        Assert.Equal((ushort)42, (await supervisor.AskAsync("calibration", 7, deadline.Token)).Value);
        for (var index = 1; index <= policy.ProbationProbes; index++)
        {
            clock.Advance(policy.TickSeconds); // Simulated probation timing, not elapsed-time qualification.
            Assert.Equal(index == policy.ProbationProbes ? "accepted" : "probation", await supervisor.TickAsync(deadline.Token));
        }
        Assert.Equal("idle", await supervisor.TickAsync(deadline.Token));
        WriteSource(Source(43));
        Assert.False((await supervisor.AskAsync("calibration", 7, deadline.Token)).Verified);
        Assert.Equal("activated", await supervisor.TickAsync(deadline.Token));
        Assert.Equal((ushort)43, (await supervisor.AskAsync("calibration", 7, deadline.Token)).Value);
        WriteSource(Source(44));
        clock.Advance(policy.TickSeconds);
        Assert.Equal("rolled_back", await supervisor.TickAsync(deadline.Token));
        WriteSource(Source());
        Assert.Equal((ushort)42, (await supervisor.AskAsync("calibration", 7, deadline.Token)).Value);
        using var ledger = LearningLedger.Open(Work, policy, clock);
        Assert.Equal(2, ledger.JobCount);
        Assert.Equal("accepted", ledger.JobState(1)); Assert.Equal("failed", ledger.JobState(2));
        Assert.Null(ledger.PendingIntent);
        Assert.False(daemon!.HasExited);
    }

    [Fact]
    public async Task RestartDuringProbationDoesNotResetMissedProbeGap()
    {
        await StartDaemon(); var clock = new FakeClock();
        using (LearningLedger.Create(Work, policy, clock)) { }
        using var deadline = new CancellationTokenSource(TimeSpan.FromSeconds(20));
        using (var first = new LearningSupervisor(runtime, policy, Work, ControlSocket, AskSocket, clock))
        {
            await first.AskAsync("calibration", 7, deadline.Token);
            Assert.Equal("activated", await first.TickAsync(deadline.Token));
        }
        clock.Advance(policy.MaxProbeGapSeconds + 1);
        using var restarted = new LearningSupervisor(runtime, policy, Work, ControlSocket, AskSocket, clock);
        Assert.Equal("rolled_back", await restarted.TickAsync(deadline.Token));
        Assert.False((await restarted.AskAsync("calibration", 7, deadline.Token)).Verified);
        using var ledger = LearningLedger.Open(Work, policy, clock);
        Assert.Equal(1, ledger.JobCount); Assert.Equal("failed", ledger.JobState(1));
    }

    [Fact]
    public async Task InvalidAuthorizedEvidenceIsReportedWithoutReservingWork()
    {
        await StartDaemon(); var clock = new FakeClock();
        using (var ledger = LearningLedger.Create(Work, policy, clock)) ledger.RecordDemand("calibration", 7, true);
        WriteSource(Encoding.ASCII.GetBytes("not independent table evidence\n"));
        using var supervisor = new LearningSupervisor(runtime, policy, Work, ControlSocket, AskSocket, clock);
        using var stop = new CancellationTokenSource(TimeSpan.FromSeconds(10));
        Assert.Equal("evidence_unavailable", await supervisor.TickAsync(stop.Token));
        using var checkedLedger = LearningLedger.Open(Work, policy, clock);
        Assert.Equal(0, checkedLedger.JobCount);
        Assert.Empty(Directory.EnumerateFileSystemEntries(Path.Combine(Work, "jobs")));
    }

    [Fact]
    public async Task ExhaustedAttemptsAreReportedInsteadOfPretendingTheQueueIsIdle()
    {
        await StartDaemon(); var clock = new FakeClock();
        using (var ledger = LearningLedger.Create(Work, policy, clock))
        {
            ledger.BindRuntime(runtime); ledger.RecordDemand("calibration", 7, true);
            for (var index = 0; index < policy.AttemptsPerHour; index++)
            {
                var job = ledger.Reserve("calibration", index.ToString("x64")).Job!;
                ledger.FailJob(job.Id, "fixture_refused");
            }
        }
        using var supervisor = new LearningSupervisor(runtime, policy, Work, ControlSocket, AskSocket, clock);
        using var stop = new CancellationTokenSource(TimeSpan.FromSeconds(10));
        Assert.Equal("attempt_budget_exhausted", await supervisor.TickAsync(stop.Token));
        using var checkedLedger = LearningLedger.Open(Work, policy, clock);
        Assert.Equal(policy.AttemptsPerHour, checkedLedger.JobCount);
        Assert.Empty(Directory.EnumerateFileSystemEntries(Path.Combine(Work, "jobs")));
    }

    // Owner-controlled test fault, never a host service: one wrong resident
    // answer, followed by either missing control IPC or a held-open STATUS.
    private sealed class FailedProbeFixture : IAsyncDisposable
    {
        private readonly string askPath, controlPath;
        private readonly bool holdControl, symbolic;
        private readonly CancellationTokenSource stop = new();
        private readonly Socket askListener = new(AddressFamily.Unix, SocketType.Stream, ProtocolType.Unspecified);
        private Socket? controlListener;
        private readonly Task answer;
        public TaskCompletionSource<bool> AfterFailedProbeStatus { get; } = new(TaskCreationOptions.RunContinuationsAsynchronously);
        public FailedProbeFixture(string askPath, string controlPath, bool holdControl, bool symbolic = false)
        {
            this.askPath = askPath; this.controlPath = controlPath; this.holdControl = holdControl; this.symbolic = symbolic;
            File.Move(askPath, askPath + ".held");
            Bind(askListener, askPath);
            answer = Answer();
        }
        private static void Bind(Socket socket, string path)
        {
            socket.Bind(new UnixDomainSocketEndPoint(path));
            File.SetUnixFileMode(path, UnixFileMode.UserRead | UnixFileMode.UserWrite);
            socket.Listen(1);
        }
        private async Task ReadRequest(Socket peer)
        {
            var bytes = new byte[80]; var used = 0;
            while (used < bytes.Length)
            {
                var read = await peer.ReceiveAsync(bytes.AsMemory(used), SocketFlags.None, stop.Token);
                if (read == 0) throw new InvalidOperationException("test_probe_request_incomplete");
                used += read;
                if (bytes[used - 1] == '\n') return;
            }
            throw new InvalidOperationException("test_probe_request_unbounded");
        }
        private async Task Answer()
        {
            try
            {
                if (symbolic)
                    for (var key = 0; key < 256; key++)
                    {
                        using var numeric = await askListener.AcceptAsync(stop.Token);
                        await ReadRequest(numeric);
                        using var stream = new NetworkStream(numeric, ownsSocket: false);
                        await stream.WriteAsync(LearningSymbolFixture.Reply(key < 2 ? key.ToString(System.Globalization.CultureInfo.InvariantCulture) : null), stop.Token);
                    }
                using (var peer = await askListener.AcceptAsync(stop.Token))
                {
                    await ReadRequest(peer);
                    File.Move(controlPath, controlPath + ".held");
                    if (holdControl)
                    {
                        controlListener = new(AddressFamily.Unix, SocketType.Stream, ProtocolType.Unspecified);
                        Bind(controlListener, controlPath);
                    }
                    // Numeric first key expects 65535; symbolic first token
                    // expects "first label" after all 256 numeric checks pass.
                    // In either mode "1" is well-framed but incorrect.
                    var wrong = Encoding.ASCII.GetBytes("{\"ok\":true,\"verified\":true,\"miss\":false,\"teacher\":false,\"source\":\"LOCAL\",\"skill\":\"capsule_core\",\"answer\":\"1\"}\n");
                    using var stream = new NetworkStream(peer, ownsSocket: false);
                    await stream.WriteAsync(wrong, stop.Token);
                }
                if (holdControl)
                {
                    using var peer = await controlListener!.AcceptAsync(stop.Token);
                    await ReadRequest(peer);
                    AfterFailedProbeStatus.TrySetResult(true);
                    await Task.Delay(Timeout.Infinite, stop.Token);
                }
            }
            catch (Exception exception) when (stop.IsCancellationRequested && exception is OperationCanceledException or ObjectDisposedException or SocketException)
            { AfterFailedProbeStatus.TrySetCanceled(); }
        }
        public async ValueTask DisposeAsync()
        {
            stop.Cancel(); askListener.Dispose(); controlListener?.Dispose();
            try { await answer.WaitAsync(TimeSpan.FromSeconds(3)); }
            finally
            {
                File.Delete(askPath); File.Move(askPath + ".held", askPath);
                if (File.Exists(controlPath + ".held"))
                { File.Delete(controlPath); File.Move(controlPath + ".held", controlPath); }
                stop.Dispose();
            }
        }
    }

    [Theory]
    [InlineData(false, false)]
    [InlineData(true, false)]
    [InlineData(false, true)]
    [InlineData(true, true)]
    public async Task KnownFailedProbeSurvivesSubsequentControlFailureOrCancellation(bool cancelled, bool symbolic)
    {
        var selected = symbolic ? LearningPolicy.Parse(Encoding.UTF8.GetBytes(LearningSymbolFixture.Policy)) : policy;
        if (symbolic) WriteSource(Encoding.ASCII.GetBytes(LearningSymbolFixture.Source));
        await StartDaemon(); var clock = new FakeClock();
        using (LearningLedger.Create(Work, selected, clock)) { }
        using var supervisor = new LearningSupervisor(runtime, selected, Work, ControlSocket, AskSocket, clock);
        using var deadline = new CancellationTokenSource(TimeSpan.FromSeconds(20));
        await supervisor.AskAsync("calibration", symbolic ? (byte)0 : (byte)7, deadline.Token);
        Assert.Equal("activated", await supervisor.TickAsync(deadline.Token));
        clock.Advance(selected.TickSeconds);
        await using (var fault = new FailedProbeFixture(AskSocket, ControlSocket, cancelled, symbolic))
        {
            using var cancellation = CancellationTokenSource.CreateLinkedTokenSource(deadline.Token);
            var failed = supervisor.TickAsync(cancellation.Token);
            try
            {
                if (cancelled)
                {
                    // A STATUS request after the wrong answer proves failure
                    // was observed before cancellation is delivered.
                    var reached = await Task.WhenAny(fault.AfterFailedProbeStatus.Task, Task.Delay(3000));
                    Assert.True(reached == fault.AfterFailedProbeStatus.Task,
                        "LEARNING_SYMBOL_PROBATION_RED known failure continued into another ASK before durable rollback duty");
                    await fault.AfterFailedProbeStatus.Task;
                    cancellation.Cancel();
                    await Assert.ThrowsAnyAsync<OperationCanceledException>(() => failed);
                }
                else await Assert.ThrowsAsync<InvalidOperationException>(() => failed);
            }
            finally
            {
                cancellation.Cancel();
                try { await failed; }
                catch (Exception error) when (error is InvalidOperationException or OperationCanceledException) { }
            }
        }
        // Restored daemon now answers every key correctly. The earlier failure
        // still mandates rollback; a later pass must never erase it.
        Assert.Equal("rolled_back", await supervisor.TickAsync(deadline.Token));
        using var ledger = LearningLedger.Open(Work, selected, clock);
        Assert.Equal("failed", ledger.JobState(1)); Assert.Null(ledger.PendingIntent);
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task StalePendingActivationNeverPublishesIfStillBeforeButReconcilesIfAlreadyPublished(bool alreadyPublished)
    {
        await StartDaemon(); var clock = new FakeClock();
        using var deadline = new CancellationTokenSource(TimeSpan.FromSeconds(20));
        var control = new LearningControlClient(runtime, ControlSocket, 5);
        var before = await control.StatusAsync(deadline.Token);
        long intentId;
        using (var ledger = LearningLedger.Create(Work, policy, clock))
        {
            ledger.BindRuntime(runtime); ledger.BindNative(before);
            ledger.RecordDemand("calibration", 7, true);
            var job = ledger.Reserve("calibration", Job().SourceSha256).Job!;
            var acquisition = new LearningAcquisition(runtime, policy, ledger);
            var candidate = await acquisition.PrepareAsync(job, before.Active!, deadline.Token);
            var stage = ledger.BeginStage(job.Id, candidate.SetName, candidate.Digest, before);
            Assert.Equal("applied", ledger.ResolveIntent(stage.Id, await control.SendAsync(stage, deadline.Token)));
            ledger.RecordEvaluation(job.Id, await acquisition.EvaluateAsync(candidate, candidate.Digest, deadline.Token));
            var intent = ledger.BeginActivate(job.Id, await control.StatusAsync(deadline.Token));
            intentId = intent.Id;
            if (alreadyPublished) Assert.Equal(intent.Expected, await control.SendAsync(intent, deadline.Token));
            // Simulated owner death leaves an exact durable intent and charge;
            // native publication either has not started or already completed.
        }
        WriteSource(Source(43));
        using (var supervisor = new LearningSupervisor(runtime, policy, Work, ControlSocket, AskSocket, clock))
            await supervisor.TickAsync(deadline.Token);
        var after = await control.StatusAsync(deadline.Token);
        Assert.Equal(before.Revision + (alreadyPublished ? 2UL : 0UL), after.Revision);
        Assert.Equal(before.Active, after.Active); Assert.Null(after.Staged);
        using (var ledger = LearningLedger.Open(Work, policy, clock))
        { Assert.Equal("failed", ledger.JobState(1)); Assert.Null(ledger.PendingIntent); }
        using var database = new SqliteConnection(new SqliteConnectionStringBuilder
        { DataSource = Path.Combine(Work, "ledger.sqlite"), Mode = SqliteOpenMode.ReadOnly, Pooling = false }.ToString());
        database.Open();
        using var charge = database.CreateCommand();
        charge.CommandText = "SELECT count(*) FROM promotion_charges WHERE intent=$id";
        charge.Parameters.AddWithValue("$id", intentId);
        Assert.Equal(1L, charge.ExecuteScalar());
    }
}
