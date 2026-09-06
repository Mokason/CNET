using System.Text;
using System.Diagnostics;
using CnetControlPlane.Learning;
using Xunit;

namespace CnetControlPlane.Tests;

public sealed class LearningChildTests
{
    // /bin/sh is a fixed TRUSTED TEST FIXTURE only. Production must pass an
    // attested native command that cannot spawn descendants, never a shell.
    private static Task<LearningChildResult> Fixture(string script, int seconds = 3,
        int cap = 1024, CancellationToken cancellation = default, ILearningClock? clock = null) =>
        LearningChild.RunAsync("/bin/sh", ["-c", script], "/tmp", new Dictionary<string, string>(),
            seconds, cap, cancellation, clock);

    [Fact]
    public async Task BothRawPipesDrainConcurrentlyAndStdinIsClosed()
    {
        var result = await Fixture("read ignored; printf '\\000\\377'; printf 'error' >&2; exit 7");
        Assert.Equal(7, result.ExitCode);
        Assert.Equal(new byte[] { 0, 255 }, result.Stdout.ToArray());
        Assert.Equal("error", Encoding.ASCII.GetString(result.Stderr.AsSpan()));
        Assert.True(result.ElapsedNanoseconds >= 0);
    }

    [Fact]
    public async Task OutputOverflowRefusesWithAFixedCode()
    {
        var error = await Assert.ThrowsAsync<LearningChildException>(() => Fixture("printf '12345'", cap: 4));
        Assert.Equal("learning_child_output_limit", error.Code);
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task TimeoutAndOverflowKillAndReapTheDirectChild(bool overflow)
    {
        var directory = Directory.CreateTempSubdirectory("cnet-child-reap-");
        var pidFile = Path.Combine(directory.FullName, "pid");
        try
        {
            var script = "printf '%s' $$ > \"$1\"; " + (overflow ? "printf '12345'; " : "") + "exec /bin/sleep 30";
            var execution = LearningChild.RunAsync("/bin/sh", ["-c", script, "fixture", pidFile],
                directory.FullName, new Dictionary<string, string>(), 1, 4, default);
            var pid = await ReadOwnedPid(pidFile);
            var error = await Assert.ThrowsAsync<LearningChildException>(() => execution);
            Assert.Equal(overflow ? "learning_child_output_limit" : "learning_child_timeout", error.Code);
            Assert.False(Directory.Exists($"/proc/{pid}"));
        }
        finally { directory.Delete(recursive: true); }
    }

    [Fact]
    public async Task AlreadyCancelledDoesNotLaunch()
    {
        using var cancellation = new CancellationTokenSource();
        cancellation.Cancel();
        await Assert.ThrowsAnyAsync<OperationCanceledException>(() => Fixture("exit 0", cancellation: cancellation.Token));
    }

    [Fact]
    public async Task ExactCapOnBothPipesIsAccepted()
    {
        var result = await Fixture("printf '1234'; printf 'abcd' >&2", cap: 4);
        Assert.Equal("1234", Encoding.ASCII.GetString(result.Stdout.AsSpan()));
        Assert.Equal("abcd", Encoding.ASCII.GetString(result.Stderr.AsSpan()));
    }

    [Fact]
    public async Task LargerSimultaneousPipesDoNotDeadlock()
    {
        var result = await Fixture("i=0; while [ $i -lt 2048 ]; do printf 'abcdefgh'; printf '12345678' >&2; i=$((i+1)); done", cap: 16384);
        Assert.Equal(16384, result.Stdout.Length);
        Assert.Equal(16384, result.Stderr.Length);
    }

    [Fact]
    public async Task StderrOverflowAlsoRefuses()
    {
        var error = await Assert.ThrowsAsync<LearningChildException>(() => Fixture("printf '12345' >&2", cap: 4));
        Assert.Equal("learning_child_output_limit", error.Code);
    }

    [Fact]
    public async Task ArgumentsAreLiteralAndOnlyAllowlistedEnvironmentIsPassed()
    {
        const string literal = "$(false); `false` ' \" *";
        var result = await LearningChild.RunAsync("/bin/sh",
            ["-c", "printf '%s|%s|%s|%s|%s' \"$1\" \"${HOME-unset}\" \"$CNET_CAPSULE_DATA_ROOT\" \"$CNET_CAPSULE_SOURCE_ROOT\" \"$CNET_CAPSULE_CORE_PATH\"", "fixture", literal],
            "/tmp", new Dictionary<string, string>
            {
                ["CNET_CAPSULE_DATA_ROOT"] = "/data",
                ["CNET_CAPSULE_SOURCE_ROOT"] = "/source",
                ["CNET_CAPSULE_CORE_PATH"] = "/library"
            }, 3, 1024, default);
        Assert.Equal(literal + "|unset|/data|/source|/library", Encoding.ASCII.GetString(result.Stdout.AsSpan()));
    }

    [Theory]
    [InlineData("sh", "/tmp", 1, 1)]
    [InlineData("/bin/sh", "tmp", 1, 1)]
    [InlineData("/bin/sh", "/tmp", 0, 1)]
    [InlineData("/bin/sh", "/tmp", 121, 1)]
    [InlineData("/bin/sh", "/tmp", 1, 0)]
    [InlineData("/bin/sh", "/tmp", 1, 65537)]
    public async Task InvalidBoundsRefuseBeforeLaunch(string executable, string directory, int seconds, int cap)
    {
        var error = await Assert.ThrowsAsync<LearningChildException>(() => LearningChild.RunAsync(executable,
            [], directory, new Dictionary<string, string>(), seconds, cap, default));
        Assert.Equal("learning_child_arguments", error.Code);
    }

    [Theory]
    [InlineData("LD_PRELOAD", "/bad.so")]
    [InlineData("PATH", "/bin")]
    [InlineData("CNET_CAPSULE_DATA_ROOT", "relative")]
    [InlineData("CNET_CAPSULE_DATA_ROOT", "/bad\0value")]
    public async Task NonAllowlistedOrNonAbsoluteEnvironmentRefuses(string key, string value)
    {
        var error = await Assert.ThrowsAsync<LearningChildException>(() => LearningChild.RunAsync("/bin/sh",
            ["-c", "exit 0"], "/tmp", new Dictionary<string, string> { [key] = value }, 1, 1, default));
        Assert.Equal("learning_child_arguments", error.Code);
    }

    [Fact]
    public async Task NulArgumentRefusesBeforeLaunch()
    {
        var error = await Assert.ThrowsAsync<LearningChildException>(() => LearningChild.RunAsync("/bin/sh",
            ["-c", "exit\0 0"], "/tmp", new Dictionary<string, string>(), 1, 1, default));
        Assert.Equal("learning_child_arguments", error.Code);
    }

    [Fact]
    public async Task MissingExecutableHasFixedFailureWithoutExternalText()
    {
        var error = await Assert.ThrowsAsync<LearningChildException>(() => LearningChild.RunAsync("/nonexistent/child-untrusted-name",
            [], "/tmp", new Dictionary<string, string>(), 1, 1, default));
        Assert.Equal("learning_child_launch_failed", error.Message);
    }

    private sealed class SequenceClock(Func<int, LearningInstant> read) : ILearningClock
    {
        private int reads;
        public LearningInstant Now => read(reads++);
    }

    [Fact]
    public async Task CancellationDuringInitialClockReadPreventsEvenALaunchAttempt()
    {
        using var cancellation = new CancellationTokenSource();
        var clock = new SequenceClock(_ =>
        {
            cancellation.Cancel();
            return new("boot", 0);
        });
        // A launch attempt deterministically yields launch_failed here, rather
        // than relying on whether the scheduler lets a marker-writing child run.
        var error = await Assert.ThrowsAnyAsync<OperationCanceledException>(() =>
            LearningChild.RunAsync("/nonexistent/learning-child-cancelled-before-launch", [],
                "/tmp", new Dictionary<string, string>(), 3, 1024, cancellation.Token, clock));
        Assert.Equal("learning_child_cancelled", error.Message);
        Assert.Equal(cancellation.Token, error.CancellationToken);
    }

    [Fact]
    public async Task CancellationDuringInitialClockReadLeavesNoChildMarker()
    {
        var directory = Directory.CreateTempSubdirectory("cnet-child-no-launch-");
        var marker = Path.Combine(directory.FullName, "launched");
        try
        {
            using var cancellation = new CancellationTokenSource();
            var clock = new SequenceClock(_ =>
            {
                cancellation.Cancel();
                return new("boot", 0);
            });
            var error = await Assert.ThrowsAnyAsync<OperationCanceledException>(() =>
                LearningChild.RunAsync("/bin/sh", ["-c", "printf launched > \"$1\"", "fixture", marker],
                    directory.FullName, new Dictionary<string, string>(), 3, 1024, cancellation.Token, clock));
            Assert.Equal("learning_child_cancelled", error.Message);
            Assert.False(File.Exists(marker));
        }
        finally { directory.Delete(recursive: true); }
    }

    [Fact]
    public async Task BoottimeJumpConsumesDeadlineWithoutWallClockDelay()
    {
        var wall = Stopwatch.StartNew();
        var clock = new SequenceClock(index => new("boot", index < 2 ? 0 : 60_000_000_000));
        var error = await Assert.ThrowsAsync<LearningChildException>(() => Fixture("exec /bin/sleep 30", seconds: 30, clock: clock));
        Assert.Equal("learning_child_timeout", error.Code);
        Assert.True(wall.Elapsed < TimeSpan.FromSeconds(5));
    }

    [Theory]
    [InlineData(true)]
    [InlineData(false)]
    public async Task BootIdentityChangeAndBackwardsClockRefuse(bool changeBoot)
    {
        var clock = new SequenceClock(index => index == 0 ? new("boot", 100) :
            changeBoot ? new("other", 100) : new("boot", 99));
        var error = await Assert.ThrowsAsync<LearningChildException>(() => Fixture("exec /bin/sleep 30", clock: clock));
        Assert.Equal("learning_child_clock_invalid", error.Code);
    }

    [Fact]
    public async Task CancellationKillsAndReapsBeforeThrowing()
    {
        var directory = Directory.CreateTempSubdirectory("cnet-child-cancel-");
        var pidFile = Path.Combine(directory.FullName, "pid");
        try
        {
            using var cancellation = new CancellationTokenSource();
            var execution = LearningChild.RunAsync("/bin/sh", ["-c", "printf '%s' $$ > \"$1\"; exec /bin/sleep 30", "fixture", pidFile],
                directory.FullName, new Dictionary<string, string>(), 10, 1024, cancellation.Token);
            var pid = await ReadOwnedPid(pidFile);
            cancellation.Cancel();
            var error = await Assert.ThrowsAnyAsync<OperationCanceledException>(() => execution);
            Assert.Equal("learning_child_cancelled", error.Message);
            Assert.False(Directory.Exists($"/proc/{pid}"));
        }
        finally { directory.Delete(recursive: true); }
    }

    [Fact]
    public async Task ExitedLeaderWithDescendantHeldPipesRefusesAtDeadline()
    {
        // Deliberately violates the production no-fork precondition. The runner
        // bounds pipe waiting, but the TEST owns and cleans this known descendant.
        var directory = Directory.CreateTempSubdirectory("cnet-child-pipe-");
        var pidFile = Path.Combine(directory.FullName, "pid");
        int? descendant = null;
        try
        {
            var wall = Stopwatch.StartNew();
            var execution = LearningChild.RunAsync("/bin/sh", ["-c", "/bin/sleep 30 & printf '%s' \"$!\" > \"$1\"; exit 0", "fixture", pidFile],
                directory.FullName, new Dictionary<string, string>(), 1, 1024, default);
            descendant = await ReadOwnedPid(pidFile);
            var error = await Assert.ThrowsAsync<LearningChildException>(() => execution);
            Assert.Equal("learning_child_timeout", error.Code);
            Assert.True(wall.Elapsed < TimeSpan.FromSeconds(5));
        }
        finally
        {
            if (descendant is int pid)
            {
                using var owned = Process.GetProcessById(pid);
                if (!owned.HasExited) owned.Kill();
                await owned.WaitForExitAsync();
            }
            directory.Delete(recursive: true);
        }
    }

    private static async Task<int> ReadOwnedPid(string path)
    {
        var deadline = Stopwatch.StartNew();
        while (deadline.Elapsed < TimeSpan.FromSeconds(3))
        {
            if (File.Exists(path) && int.TryParse(await File.ReadAllTextAsync(path), out var pid)) return pid;
            await Task.Delay(10);
        }
        throw new InvalidOperationException("trusted_fixture_did_not_publish_pid");
    }
}
