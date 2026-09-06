using System.Collections.Immutable;
using System.ComponentModel;
using System.Diagnostics;

namespace CnetControlPlane.Learning;

internal sealed record LearningChildResult(int ExitCode, ImmutableArray<byte> Stdout,
    ImmutableArray<byte> Stderr, long ElapsedNanoseconds);
internal sealed class LearningChildException(string code) : InvalidOperationException(code)
{
    internal string Code { get; } = code;
}
internal static class LearningChild
{
    /// <summary>
    /// Executes only a caller-attested, privately installed fixed native command with a
    /// read-only library closure. The caller must ensure the command cannot fork (for
    /// workers, the native guard enforces this); never pass a shell or a daemon here.
    /// This is not a filesystem attestor or an arbitrary-program sandbox. It guarantees
    /// direct-child kill/reap and bounded output waiting, not cleanup of descendants
    /// that outlive their leader. Native workers must also bind their expected parent.
    /// </summary>
    internal static async Task<LearningChildResult> RunAsync(string executable, IReadOnlyList<string> arguments,
        string workingDirectory, IReadOnlyDictionary<string, string> environment, int seconds, int pipeByteCap,
        CancellationToken cancellationToken, ILearningClock? clock = null)
    {
        var startInfo = Prepare(executable, arguments, workingDirectory, environment, seconds, pipeByteCap);
        CheckCancellation(cancellationToken);
        LearningInstant start;
        try
        {
            clock ??= new LearningClock();
            start = clock.Now;
            if (string.IsNullOrEmpty(start.Boot) || start.Nanoseconds < 0 ||
                start.Nanoseconds > long.MaxValue - seconds * 1_000_000_000L)
                throw new InvalidOperationException();
        }
        catch (Exception exception) when (exception is InvalidOperationException or IOException or OverflowException)
        {
            throw new LearningChildException("learning_child_clock_invalid");
        }

        // Linux PDEATHSIG follows the thread that spawned the process, not
        // merely the lifetime of this managed owner process. Keep that dedicated
        // thread alive until the entire async kill/reap/pipe lifecycle finishes.
        // Caller-owned arguments/environment and the initial clock were already
        // validated and snapshotted synchronously above, before this handoff.
        return await Task.Factory.StartNew(
            () => RunPreparedAsync(startInfo, start, clock, seconds, pipeByteCap, cancellationToken).GetAwaiter().GetResult(),
            CancellationToken.None, TaskCreationOptions.LongRunning, TaskScheduler.Default).ConfigureAwait(false);
    }

    private static async Task<LearningChildResult> RunPreparedAsync(ProcessStartInfo startInfo, LearningInstant start,
        ILearningClock clock, int seconds, int pipeByteCap, CancellationToken cancellationToken)
    {
        using var process = new Process { StartInfo = startInfo };
        using var pipeCancellation = new CancellationTokenSource();
        Task<ImmutableArray<byte>>? stdout = null;
        Task<ImmutableArray<byte>>? stderr = null;
        var started = false;
        try
        {
            try
            {
                // Initial clock access may have observed cancellation. Check at
                // the launch boundary; cancellation racing the OS launch itself
                // is still handled by the kill/reap path below.
                CheckCancellation(cancellationToken);
                started = process.Start();
                if (!started) throw new LearningChildException("learning_child_launch_failed");
            }
            catch (Exception exception) when (exception is Win32Exception or InvalidOperationException)
            {
                throw new LearningChildException("learning_child_launch_failed");
            }

            process.StandardInput.Close();
            stdout = ReadBounded(process.StandardOutput.BaseStream, pipeByteCap, pipeCancellation.Token);
            stderr = ReadBounded(process.StandardError.BaseStream, pipeByteCap, pipeCancellation.Token);
            var exited = process.WaitForExitAsync(CancellationToken.None);
            var last = start.Nanoseconds;
            while (true)
            {
                CheckCancellation(cancellationToken);
                LearningInstant now;
                try { now = clock.Now; }
                catch (Exception exception) when (exception is InvalidOperationException or IOException or OverflowException)
                {
                    throw new LearningChildException("learning_child_clock_invalid");
                }
                if (now.Boot != start.Boot || now.Nanoseconds < last)
                    throw new LearningChildException("learning_child_clock_invalid");
                last = now.Nanoseconds;
                if (now.Nanoseconds - start.Nanoseconds >= seconds * 1_000_000_000L)
                    throw new LearningChildException("learning_child_timeout");

                // Inspect faults even while the other pipe or child is still pending.
                if (stdout.IsCompleted) await stdout.ConfigureAwait(false);
                if (stderr.IsCompleted) await stderr.ConfigureAwait(false);
                if (exited.IsCompleted) await exited.ConfigureAwait(false);
                if (exited.IsCompletedSuccessfully && stdout.IsCompletedSuccessfully && stderr.IsCompletedSuccessfully)
                    return new(process.ExitCode, stdout.Result, stderr.Result, now.Nanoseconds - start.Nanoseconds);

                // Do not include already-completed tasks: that would busy-spin while
                // a descendant holds a pipe open after its leader has exited.
                var pending = new List<Task>(4) { Task.Delay(20, cancellationToken) };
                if (!exited.IsCompleted) pending.Add(exited);
                if (!stdout.IsCompleted) pending.Add(stdout);
                if (!stderr.IsCompleted) pending.Add(stderr);
                await Task.WhenAny(pending).ConfigureAwait(false);
            }
        }
        catch (Exception exception) when (exception is IOException or ObjectDisposedException)
        {
            throw new LearningChildException("learning_child_pipe_failure");
        }
        catch (Exception exception) when (exception is Win32Exception ||
            exception is InvalidOperationException and not LearningChildException)
        {
            throw new LearningChildException("learning_child_reap_failed");
        }
        finally
        {
            if (started)
            {
                // Reap before returning or throwing. Kernel process termination is
                // not replaced by an elapsed-time assertion or a cancelled wait.
                try { await KillAndReap(process).ConfigureAwait(false); }
                finally
                {
                    pipeCancellation.Cancel();
                    process.StandardOutput.Dispose();
                    process.StandardError.Dispose();
                    var drains = new List<Task>(2);
                    if (stdout is not null) drains.Add(Observe(stdout));
                    if (stderr is not null) drains.Add(Observe(stderr));
                    try { await Task.WhenAll(drains).WaitAsync(TimeSpan.FromSeconds(1)).ConfigureAwait(false); }
                    catch (TimeoutException) { throw new LearningChildException("learning_child_pipe_failure"); }
                }
            }
        }
    }

    private static ProcessStartInfo Prepare(string executable, IReadOnlyList<string> arguments,
        string workingDirectory, IReadOnlyDictionary<string, string> environment, int seconds, int cap)
    {
        static bool PathIsValid(string? path) => !string.IsNullOrEmpty(path) && path.Length <= 4096 &&
            path.IndexOf('\0') < 0 && Path.IsPathFullyQualified(path);
        if (!PathIsValid(executable) || !PathIsValid(workingDirectory) || arguments is null ||
            environment is null || seconds is < 1 or > 120 || cap is < 1 or > 65536 || arguments.Count > 32)
            throw new LearningChildException("learning_child_arguments");
        var info = new ProcessStartInfo
        {
            FileName = executable, WorkingDirectory = workingDirectory, UseShellExecute = false,
            RedirectStandardInput = true, RedirectStandardOutput = true, RedirectStandardError = true,
            CreateNoWindow = true
        };
        info.Environment.Clear();
        foreach (var argument in arguments)
        {
            if (argument is null || argument.Length > 4096 || argument.IndexOf('\0') >= 0)
                throw new LearningChildException("learning_child_arguments");
            info.ArgumentList.Add(argument);
        }
        foreach (var (key, value) in environment)
        {
            if (key is not ("CNET_CAPSULE_DATA_ROOT" or "CNET_CAPSULE_SOURCE_ROOT" or "CNET_CAPSULE_CORE_PATH") ||
                !PathIsValid(value))
                throw new LearningChildException("learning_child_arguments");
            info.Environment.Add(key, value);
        }
        return info;
    }

    private static async Task<ImmutableArray<byte>> ReadBounded(Stream stream, int cap, CancellationToken cancellation)
    {
        // The one extra byte distinguishes exact-cap EOF from an overflowing pipe.
        var buffer = new byte[cap + 1];
        var used = 0;
        while (true)
        {
            var count = await stream.ReadAsync(buffer.AsMemory(used), cancellation).ConfigureAwait(false);
            if (count == 0) return ImmutableArray.Create(buffer, 0, used);
            used += count;
            if (used > cap) throw new LearningChildException("learning_child_output_limit");
        }
    }

    private static void CheckCancellation(CancellationToken cancellation)
    {
        if (cancellation.IsCancellationRequested)
            throw new OperationCanceledException("learning_child_cancelled", cancellation);
    }

    private static async Task KillAndReap(Process process)
    {
        try
        {
            if (!process.HasExited)
            {
                try { process.Kill(); }
                catch (Exception exception) when (exception is Win32Exception or InvalidOperationException)
                {
                    // The sole acceptable kill race is a child that already exited.
                    if (!process.HasExited) throw;
                }
            }
            await process.WaitForExitAsync(CancellationToken.None).ConfigureAwait(false);
        }
        catch (Exception exception) when (exception is Win32Exception or InvalidOperationException)
        {
            throw new LearningChildException("learning_child_reap_failed");
        }
    }

    private static async Task Observe(Task task)
    {
        try { await task.ConfigureAwait(false); }
        catch (Exception exception) when (exception is IOException or OperationCanceledException or
            ObjectDisposedException or LearningChildException) { }
    }
}
