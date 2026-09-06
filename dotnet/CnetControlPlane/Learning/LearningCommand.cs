using System.Text.Json;
using System.Globalization;
using System.Runtime.InteropServices;
using Microsoft.Data.Sqlite;

namespace CnetControlPlane.Learning;

internal static class LearningCommand
{
    public static int RunCli(string[] args)
    {
        var correlation = Guid.NewGuid().ToString("N");
        try
        {
            if (args.Length < 2 || args[0] is not ("inspect" or "initialize" or "status" or "pause" or "resume" or "ask" or "tick" or "run" or "quiesce")
                || args.Length != (args[0] == "ask" ? 4 : 2))
                throw new ArgumentException("learning_command_usage");
            byte key = 0;
            if (args[0] == "ask" && (!LearningPolicy.IsId(args[2])
                || !byte.TryParse(args[3], NumberStyles.None, CultureInfo.InvariantCulture, out key)
                || key.ToString(CultureInfo.InvariantCulture) != args[3]))
                throw new ArgumentException("learning_command_usage");
            using var root = LearningFiles.Open(args[1]);
            using var managed = LearningManagedRuntime.Load(Path.Combine(root.FullPath, "managed"), root.Read("managed.json", 4096));
            var running = managed.BindRunning(); // Before any SQLite initialization.
            using var native = LearningRuntime.Load(Path.Combine(root.FullPath, "native"), root.Read("runtime.json", 4096));
            var policy = LearningPolicy.Parse(root.Read("policy.json", 16384));
            policy.RequireEnabled();
            if (policy.AllocatorEnabled) throw new InvalidOperationException("learning_allocator_not_approved");
            root.AssertPathIdentity();
            if (args[0] == "inspect")
            {
                // A real provider import exercises the fixed verified resolver.
                using var connection = new SqliteConnection("Data Source=:memory:;Pooling=False");
                connection.Open(); using var query = connection.CreateCommand();
                query.CommandText = "SELECT sqlite_version()";
                Emit(new { @event = "learning_inspected", correlation_id = correlation, managed_sha256 = running.Sha256,
                    native_sha256 = native.Sha256, policy_sha256 = policy.Sha256, sqlite_version = (string)query.ExecuteScalar()! });
                return 0;
            }
            var workPath = Path.Combine(root.FullPath, "work");
            if (args[0] == "initialize")
            {
                using var work = root.CreateDirectory("work");
                using var ipc = root.CreateDirectory("ipc");
                foreach (var name in new[] { "data", "jobs", "sets", "frozen" }) using (work.CreateDirectory(name)) { }
                using (var state = work.CreateDirectory("state")) using (state.CreateDirectory("snapshots")) { }
                using var created = LearningLedger.Create(workPath, policy, new LearningClock());
                created.BindRuntime(native); created.BindManaged(running);
                Status(created, policy, correlation, "learning_initialized");
                return 0;
            }
            using var ledger = LearningLedger.Open(workPath, policy, new LearningClock());
            if (ledger.RuntimeSha256 is null || ledger.ManagedSha256 is null)
            {
                ledger.Pause();
                throw new InvalidOperationException("learning_installation_binding_missing");
            }
            ledger.BindRuntime(native); ledger.BindManaged(running);
            switch (args[0])
            {
                case "pause": ledger.Pause(); Status(ledger, policy, correlation, "learning_paused"); return 0;
                case "resume":
                    using (var work = LearningFiles.Open(workPath))
                    using (work.AcquireLock("owner.lock"))
                    {
                        if (ledger.RunStatus is { State: not "running" })
                            throw new InvalidOperationException("learning_run_terminal");
                        var control = new LearningControlClient(native, Path.Combine(root.FullPath, "ipc/control.sock"), Math.Min(policy.WorkerSeconds, 60));
                        var observed = control.StatusAsync(default).GetAwaiter().GetResult();
                        if (ledger.NativeStatus is null) ledger.BindNative(observed);
                        ledger.Resume(observed);
                    }
                    Status(ledger, policy, correlation, "learning_resumed"); return 0;
                case "ask":
                    var dataset = policy.Datasets.SingleOrDefault(d => d.Id == args[2])
                        ?? throw new ArgumentException("learning_dataset_not_authorized");
                    using (var client = new LearningAskClient(Path.Combine(root.FullPath, "ipc/ask.sock"), policy.WorkerSeconds))
                    {
                        var answer = client.AskAsync(dataset, key, default).GetAwaiter().GetResult();
                        ledger.RecordDemand(dataset.Id, key, !answer.Verified);
                        Emit(new { @event = "learning_answer", correlation_id = correlation, dataset = dataset.Id, key,
                            verified = answer.Verified, value = answer.Value });
                    }
                    return 0;
                case "quiesce":
                case "run":
                case "tick":
                    using (var supervisor = new LearningSupervisor(native, policy, workPath,
                        Path.Combine(root.FullPath, "ipc/control.sock"), Path.Combine(root.FullPath, "ipc/ask.sock")))
                    {
                        running.Verify();
                        if (args[0] == "quiesce")
                        {
                            // Acquire the supervisor lock BEFORE stopping: a
                            // competing owner must not be mutated by a failed lock attempt.
                            ledger.Pause(); ledger.StopRun("operator_stop");
                            using var cleanup = new CancellationTokenSource(TimeSpan.FromSeconds(Math.Min(120, policy.WorkerSeconds * 4 + 1)));
                            var outcome = supervisor.QuiesceAsync(cleanup.Token).GetAwaiter().GetResult();
                            var settled = outcome != "frozen" && ledger.PendingIntent is null && ledger.Outstanding is null
                                && ledger.NativeStatus?.Staged is null;
                            Status(ledger, policy, correlation, settled ? "learning_quiesced" : "learning_unsettled", outcome);
                            return settled ? 0 : 2;
                        }
                        if (args[0] == "run")
                        {
                            using var stop = new CancellationTokenSource();
                            using var interrupt = PosixSignalRegistration.Create(PosixSignal.SIGINT, context => { context.Cancel = true; stop.Cancel(); });
                            using var terminate = PosixSignalRegistration.Create(PosixSignal.SIGTERM, context => { context.Cancel = true; stop.Cancel(); });
                            return LearningRunLoop.RunAsync(supervisor, ledger, running, policy, correlation, stop.Token).GetAwaiter().GetResult();
                        }
                        var action = supervisor.TickAsync(default).GetAwaiter().GetResult();
                        Emit(new { @event = "learning_tick", correlation_id = correlation, action });
                        return action is "frozen" or "verified_mismatch" or "outcome_unknown" ? 2 : 0;
                    }
            }
            Status(ledger, policy, correlation, "learning_status");
            return 0;
        }
        catch (Exception error) when (error is ArgumentException or InvalidOperationException or IOException
            or UnauthorizedAccessException or SqliteException or OperationCanceledException)
        {
            // Never echo paths, source data, SQL, native tokens or arbitrary exception prose.
            var code = error.Message is { Length: <= 96 } message && message.StartsWith("learning_", StringComparison.Ordinal)
                && message.All(c => c is >= 'a' and <= 'z' or '_') ? message : "learning_command_refused";
            Console.Error.WriteLine(JsonSerializer.Serialize(new { @event = "learning_refused", correlation_id = correlation, code }));
            return 2;
        }
    }
    private static void Emit<T>(T value) => Console.WriteLine(JsonSerializer.Serialize(value));
    private static void Status(LearningLedger ledger, LearningPolicy policy, string correlation, string name, string? action = null)
    {
        var run = ledger.RunStatus;
        Emit(new { @event = name, correlation_id = correlation, managed_sha256 = ledger.ManagedSha256,
            native_sha256 = ledger.RuntimeSha256, policy_sha256 = policy.Sha256, paused = ledger.IsPaused,
            jobs = ledger.JobCount, run_state = run?.State ?? "not_started", run, pending_intent = ledger.PendingIntent?.Kind,
            outstanding_state = ledger.Outstanding?.State, action });
    }
}
