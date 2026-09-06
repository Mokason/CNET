using System.Diagnostics;
using System.Globalization;
using System.Runtime.Versioning;
using System.Text;
using CnetControlPlane.Learning;
using Xunit;

namespace CnetControlPlane.Tests;

[SupportedOSPlatform("linux")]
public sealed class LearningChildParentLifetimeTests
{
    private sealed class InitialClock(Action firstRead) : ILearningClock
    {
        private readonly LearningClock clock = new();
        private int reads;
        public bool ReadSynchronously { get; private set; }
        public LearningInstant Now
        {
            get
            {
                if (Interlocked.Increment(ref reads) == 1)
                {
                    firstRead(); ReadSynchronously = true;
                }
                return clock.Now;
            }
        }
    }

    [Fact]
    public async Task ArgumentsEnvironmentAndInitialClockAreCapturedBeforeThreadHandoff()
    {
        var callerThread = Environment.CurrentManagedThreadId;
        var arguments = new List<string> { "-c", "printf '%s|%s' \"$1\" \"$CNET_CAPSULE_DATA_ROOT\"", "fixture", "original" };
        var environment = new Dictionary<string, string> { ["CNET_CAPSULE_DATA_ROOT"] = "/original" };
        var clock = new InitialClock(() =>
        {
            Assert.Equal(callerThread, Environment.CurrentManagedThreadId);
            // Preparation must already have made private copies before the
            // initial clock is sampled, still synchronously on the caller.
            arguments[3] = "changed";
            environment["CNET_CAPSULE_DATA_ROOT"] = "/changed";
        });
        // Fixed trusted shell builtins are a TEST fixture only, not production input.
        var pending = LearningChild.RunAsync("/bin/sh", arguments, "/tmp", environment, 3, 64, default, clock);
        Assert.True(clock.ReadSynchronously);
        var result = await pending;
        Assert.Equal(0, result.ExitCode);
        Assert.Equal("original|/original", Encoding.ASCII.GetString(result.Stdout.AsSpan()));
    }

    private const string HelperSource = """
        #define _POSIX_C_SOURCE 200809L
        #include <fcntl.h>
        #include <signal.h>
        #include <stdio.h>
        #include <stdlib.h>
        #include <sys/prctl.h>
        #include <time.h>
        #include <unistd.h>

        /* Fixed no-fork test helper; no production guard is changed. */
        int main(int argc, char **argv) {
            if (argc != 4) return 90;
            char *end = NULL;
            long expected = strtol(argv[1], &end, 10);
            if (expected <= 1 || *end || getppid() != expected) return 91;
            if (prctl(PR_SET_PDEATHSIG, SIGKILL, 0, 0, 0) != 0 || getppid() != expected) return 92;
            int fd = open(argv[2], O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
            if (fd < 0 || write(fd, "ready\n", 6) != 6 || close(fd) != 0) return 93;
            struct timespec pause = { .tv_sec = 0, .tv_nsec = 1000000 };
            for (int count = 0; count < 5000; ++count) {
                if (access(argv[3], F_OK) == 0) {
                    if (getppid() != expected) return 94;
                    return puts("finished") < 0 ? 95 : 0;
                }
                nanosleep(&pause, NULL);
            }
            return 96;
        }
        """;

    [Fact]
    public async Task ParentDeathGuardSurvivesTheInvokingManagedThreadEnding()
    {
        var root = Directory.CreateTempSubdirectory("cnet-child-parent-thread-").FullName;
        File.SetUnixFileMode(root, UnixFileMode.UserRead | UnixFileMode.UserWrite | UnixFileMode.UserExecute);
        var source = Path.Combine(root, "parent-thread.c"); var helper = Path.Combine(root, "parent-thread");
        var ready = Path.Combine(root, "READY"); var release = Path.Combine(root, "RELEASE");
        Task<LearningChildResult>? execution = null;
        Exception? invokingFailure = null;
        Thread? invoking = null;
        using var cancellation = new CancellationTokenSource(TimeSpan.FromSeconds(15));
        try
        {
            File.WriteAllText(source, HelperSource);
            var compile = new ProcessStartInfo("/usr/bin/cc")
            {
                WorkingDirectory = root, UseShellExecute = false,
                RedirectStandardOutput = true, RedirectStandardError = true
            };
            foreach (var argument in new[] { "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", source, "-o", helper })
                compile.ArgumentList.Add(argument);
            using (var compiler = Process.Start(compile)!)
            {
                var stdout = compiler.StandardOutput.ReadToEndAsync(); var stderr = compiler.StandardError.ReadToEndAsync();
                try { await compiler.WaitForExitAsync(cancellation.Token); }
                catch { compiler.Kill(entireProcessTree: true); await compiler.WaitForExitAsync(); throw; }
                Assert.True(compiler.ExitCode == 0, "fixed native parent-thread fixture did not compile: " + await stdout + await stderr);
            }
            File.SetUnixFileMode(helper, UnixFileMode.UserRead | UnixFileMode.UserExecute);
            invoking = new Thread(() =>
            {
                try
                {
                    execution = LearningChild.RunAsync(helper,
                        [Environment.ProcessId.ToString(CultureInfo.InvariantCulture), ready, release],
                        root, new Dictionary<string, string>(), 10, 64, cancellation.Token);
                    var wait = Stopwatch.StartNew();
                    while (!File.Exists(ready) && wait.Elapsed < TimeSpan.FromSeconds(5)) Thread.Sleep(1);
                    if (!File.Exists(ready)) throw new InvalidOperationException("fixed native parent-thread fixture did not become ready");
                    // The whole test process stays alive; only this invoking
                    // thread exits while its asynchronous child is still waiting.
                }
                catch (Exception error) { invokingFailure = error; }
            });
            invoking.Start();
            Assert.True(invoking.Join(TimeSpan.FromSeconds(8)), "invoking fixture thread did not exit");
            Assert.Null(invokingFailure);
            Assert.NotNull(execution);
            Assert.Equal(UnixFileMode.UserRead | UnixFileMode.UserWrite, File.GetUnixFileMode(ready));
            File.WriteAllText(release, "finish");
            var result = await execution.WaitAsync(TimeSpan.FromSeconds(10));
            Assert.True(result.ExitCode == 0,
                "LEARNING_CHILD_PARENT_THREAD_RED: invoking thread exited while owner process lives; child exit=" + result.ExitCode);
            Assert.Equal("finished\n", Encoding.ASCII.GetString(result.Stdout.AsSpan()));
            Assert.True(result.Stderr.IsEmpty);
        }
        finally
        {
            cancellation.Cancel();
            if (invoking is { IsAlive: true }) invoking.Join(TimeSpan.FromSeconds(8));
            if (execution is not null)
            {
                try { await execution.WaitAsync(TimeSpan.FromSeconds(12)); }
                catch (Exception error) when (error is OperationCanceledException or LearningChildException) { }
            }
            Directory.Delete(root, true);
        }
    }
}
