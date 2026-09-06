using System.Text.Json;

namespace CnetControlPlane.Learning;

internal static class LearningRunLoop
{
    // Supervisor owns the workspace lock for this entire call. The second
    // connection records telemetry only between serialized supervisor calls.
    internal static async Task<int> RunAsync(LearningSupervisor supervisor, LearningLedger ledger,
        LearningRunningManagedRuntime managed, LearningPolicy policy, string correlation, CancellationToken stopping)
    {
        var run = ledger.BeginOrResumeRun();
        Emit("learning_run_observed", correlation, run);
        if (run.State != "running") return 2; // Terminal is never a new budget.
        using var monitoring = new CancellationTokenSource();
        using var deadline = new CancellationTokenSource();
        using var work = CancellationTokenSource.CreateLinkedTokenSource(stopping, deadline.Token);
        var monitor = Monitor(run, policy, deadline, monitoring.Token);
        var budgetExpired = false;
        var failed = false;
        var cancelled = false;
        try
        {
            while (true)
            {
                work.Token.ThrowIfCancellationRequested();
                run = ledger.BeginOrResumeRun(); // Restart/heartbeat/gap checks, no refresh.
                if (run.State != "running") { failed = true; break; }
                managed.Verify();
                var action = await supervisor.TickAsync(work.Token).ConfigureAwait(false);
                run = ledger.RecordRunTick(action); // Only an actual completed tick.
                Emit("learning_run_tick", correlation, run, action);
                if (run.State != "running") { failed = true; break; }
                await Task.Delay(TimeSpan.FromSeconds(policy.TickSeconds), work.Token).ConfigureAwait(false);
            }
        }
        catch (OperationCanceledException) when (work.IsCancellationRequested)
        {
            cancelled = true;
        }
        catch (Exception error) when (error is ArgumentException or InvalidOperationException or IOException
            or UnauthorizedAccessException or Microsoft.Data.Sqlite.SqliteException)
        {
            failed = true;
        }
        finally
        {
            monitoring.Cancel();
            var outcome = await monitor.ConfigureAwait(false);
            // Cancel wakes the worker before the monitor's Task necessarily
            // becomes complete. Classify only after awaiting that final outcome.
            budgetExpired = cancelled && outcome == "budget" && !stopping.IsCancellationRequested;
            if (cancelled && !budgetExpired) failed = true;
            if (outcome == "clock_failed") { budgetExpired = false; failed = true; }
        }
        if (stopping.IsCancellationRequested) { failed = true; budgetExpired = false; }
        if (failed) run = ledger.StopRun(stopping.IsCancellationRequested ? "cancelled" : "tick_failed")!;
        // Cleanup has its own bounded allowance AFTER the acquisition budget.
        // Expiry never means leaving an unaccepted active candidate unmonitored.
        using var cleanup = new CancellationTokenSource(TimeSpan.FromSeconds(Math.Min(120, policy.WorkerSeconds * 4 + 1)));
        try
        {
            managed.Verify();
            var action = await supervisor.QuiesceAsync(cleanup.Token).ConfigureAwait(false);
            Emit("learning_run_quiesced", correlation, ledger.RunStatus!, action);
            if (action == "frozen" || ledger.PendingIntent is not null || ledger.Outstanding is not null
                || ledger.NativeStatus?.Staged is not null)
                throw new InvalidOperationException("learning_run_native_unsettled");
            // Safety cleanup deliberately has its own cancellation token. An
            // owner stop observed during that cleanup still wins at this last
            // cancellation check before the terminal completion transaction.
            if (stopping.IsCancellationRequested)
            {
                failed = true; budgetExpired = false;
                run = ledger.StopRun("cancelled")!;
            }
            if (budgetExpired && !failed) run = ledger.FinishRun();
        }
        catch (Exception error) when (error is ArgumentException or InvalidOperationException or IOException
            or UnauthorizedAccessException or Microsoft.Data.Sqlite.SqliteException or OperationCanceledException)
        {
            run = ledger.StopRun(stopping.IsCancellationRequested ? "cancelled" : "tick_failed")!;
            Emit("learning_run_unsettled", correlation, run);
            return 2; // Pending intent/rollback evidence is retained for owner recovery.
        }
        Emit("learning_run_finished", correlation, run);
        return run.State == "budget_complete" && !failed ? 0 : 2;
    }

    internal static async Task<string> Monitor(LearningRun run, LearningPolicy policy,
        CancellationTokenSource deadline, CancellationToken stop, Func<ILearningClock>? clockFactory = null)
    {
        var last = run.LastNanoseconds;
        try
        {
            // The internal factory is invoked once for deterministic clock
            // failure tests. Production always constructs the fixed BOOTTIME clock.
            var clock = clockFactory is null ? new LearningClock() : clockFactory();
            while (!stop.IsCancellationRequested)
            {
                var now = clock.Now;
                if (now.Boot != run.Boot || now.Nanoseconds < last)
                { deadline.Cancel(); return "clock_failed"; }
                last = now.Nanoseconds;
                if (now.Nanoseconds - run.StartNanoseconds >= policy.MaxRunSeconds * 1_000_000_000L)
                { deadline.Cancel(); return "budget"; }
                await Task.Delay(25, stop).ConfigureAwait(false);
            }
            return "stopped";
        }
        catch (OperationCanceledException) when (stop.IsCancellationRequested) { return "stopped"; }
        catch (Exception error) when (error is InvalidOperationException or OverflowException
            or IOException or UnauthorizedAccessException)
        { deadline.Cancel(); return "clock_failed"; }
    }

    private static void Emit(string name, string correlation, LearningRun run, string? action = null) =>
        Console.WriteLine(JsonSerializer.Serialize(new { @event = name, correlation_id = correlation,
            run_boot = run.Boot, run_start_ns = run.StartNanoseconds, last_ns = run.LastNanoseconds,
            ticks = run.TickCount, state = run.State, last_action = run.LastAction, action }));
}
