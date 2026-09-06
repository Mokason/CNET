using System.Diagnostics;
using System.Net.Sockets;
using System.Runtime.InteropServices;
using System.Runtime.Versioning;
using System.Text;
using System.Text.Json;
using CnetControlPlane.Learning;
using Xunit;

namespace CnetControlPlane.Tests;

[SupportedOSPlatform("linux")]
public sealed class LearningRunCancellationTests : IClassFixture<LearningCommandInstallation>
{
    private readonly LearningCommandInstallation installation;
    public LearningRunCancellationTests(LearningCommandInstallation installation) { this.installation = installation; }
    [DllImport("libc", SetLastError = true)] private static extern int kill(int pid, int signal);

    [Theory]
    [InlineData("io")]
    [InlineData("access")]
    [InlineData("invalid")]
    public async Task ClockConstructorFailureCancelsWorkAndReturnsAClassifiedOutcome(string kind)
    {
        var policy = LearningPolicy.Parse(Encoding.UTF8.GetBytes(LearningPolicyTests.Valid));
        var run = new LearningRun("00000000-0000-0000-0000-000000000001", 0, 0, 0, "running", "run_started");
        using var deadline = new CancellationTokenSource();
        var calls = 0;
        ILearningClock Factory()
        {
            calls++;
            throw kind switch
            {
                "io" => new IOException("fixture clock construction failed"),
                "access" => new UnauthorizedAccessException("fixture clock access refused"),
                _ => new InvalidOperationException("learning_boot_identity_invalid")
            };
        }
        string? outcome = null;
        var error = await Record.ExceptionAsync(async () => outcome = await LearningRunLoop.Monitor(run, policy, deadline, default, Factory));
        Assert.True(error is null, "LEARNING_RUN_MONITOR_RED: clock constructor escaped monitor failure classification");
        Assert.Equal("clock_failed", outcome);
        Assert.True(deadline.IsCancellationRequested);
        Assert.Equal(1, calls);
    }

    [Fact]
    public async Task OwnerCancellationDuringNaturalQuiesceWinsBeforeCompletionWithoutCancellingCleanup()
    {
        using var deployment = installation.Deploy();
        deployment.Put("policy.json", Encoding.UTF8.GetBytes(LearningPolicyTests.Valid
            .Replace("\"max_run_seconds\":259200", "\"max_run_seconds\":1")
            .Replace("\"worker_seconds\":30", "\"worker_seconds\":5")));
        Assert.Equal(0, (await deployment.Command("initialize")).Code);
        using var fixture = new DelayedQuiesce(deployment.Root);
        var start = new ProcessStartInfo(LearningCommandInstallation.Dotnet)
        {
            WorkingDirectory = deployment.Root, UseShellExecute = false,
            RedirectStandardInput = true, RedirectStandardOutput = true, RedirectStandardError = true
        };
        start.Environment.Clear();
        foreach (var arg in new[] { Path.Combine(deployment.Root, "managed/cnet-control.dll"), "learning", "run", deployment.Root })
            start.ArgumentList.Add(arg);
        using var process = Process.Start(start)!;
        process.StandardInput.Close();
        // Fixed trusted CLI emits a finite handful of diagnostic lines in this
        // one-second idle fixture; no untrusted worker/program is launched here.
        var output = process.StandardOutput.ReadToEndAsync(); var error = process.StandardError.ReadToEndAsync();
        try
        {
            await fixture.Reached.Task.WaitAsync(TimeSpan.FromSeconds(15));
            Assert.False(process.HasExited);
            Assert.Equal(0, kill(process.Id, 15)); // Only this test-owned run process.
            // Keep native cleanup blocked while the registered signal callback
            // runs. Releasing it must still permit the private STATUS to finish.
            await Task.Delay(100);
            fixture.Release.TrySetResult();
            await fixture.Completed.Task.WaitAsync(TimeSpan.FromSeconds(10));
            await process.WaitForExitAsync().WaitAsync(TimeSpan.FromSeconds(15));
            var stdout = await output; var stderr = await error;
            Assert.Contains("learning_run_quiesced", stdout);
            Assert.True(process.ExitCode == 2,
                "LEARNING_RUN_QUIESCE_CANCEL_RED: owner cancellation became budget completion: " + stderr);
            using var status = JsonDocument.Parse((await deployment.Command("status")).Output);
            Assert.Equal("failed", status.RootElement.GetProperty("run_state").GetString());
            Assert.Equal("cancelled", status.RootElement.GetProperty("run").GetProperty("LastAction").GetString());
            Assert.Equal(2, fixture.RequestCount); // Cleanup completed; it was not cancelled or retried.
        }
        finally
        {
            fixture.Release.TrySetResult();
            if (!process.HasExited) { process.Kill(entireProcessTree: true); await process.WaitForExitAsync(); }
            _ = await output; _ = await error;
        }
    }

    private sealed class DelayedQuiesce : IDisposable
    {
        private readonly Socket control = new(AddressFamily.Unix, SocketType.Stream, ProtocolType.Unspecified);
        private readonly Socket ask = new(AddressFamily.Unix, SocketType.Stream, ProtocolType.Unspecified);
        private readonly CancellationTokenSource stop = new(TimeSpan.FromSeconds(20));
        private readonly Task server;
        public TaskCompletionSource Reached { get; } = new(TaskCreationOptions.RunContinuationsAsynchronously);
        public TaskCompletionSource Release { get; } = new(TaskCreationOptions.RunContinuationsAsynchronously);
        public TaskCompletionSource Completed { get; } = new(TaskCreationOptions.RunContinuationsAsynchronously);
        public int RequestCount { get; private set; }
        public DelayedQuiesce(string root)
        {
            var controlPath = Path.Combine(root, "ipc/control.sock"); var askPath = Path.Combine(root, "ipc/ask.sock");
            control.Bind(new UnixDomainSocketEndPoint(controlPath)); control.Listen(4);
            ask.Bind(new UnixDomainSocketEndPoint(askPath)); ask.Listen(1);
            File.SetUnixFileMode(controlPath, UnixFileMode.UserRead | UnixFileMode.UserWrite);
            File.SetUnixFileMode(askPath, UnixFileMode.UserRead | UnixFileMode.UserWrite);
            server = Serve();
        }
        private async Task Serve()
        {
            try
            {
                for (var index = 0; index < 2; index++)
                {
                    using var peer = await control.AcceptAsync(stop.Token);
                    using var stream = new NetworkStream(peer, ownsSocket: false);
                    var request = new byte[7];
                    await stream.ReadExactlyAsync(request, stop.Token);
                    Assert.Equal("STATUS\n", Encoding.ASCII.GetString(request));
                    RequestCount++;
                    if (index == 1) { Reached.TrySetResult(); await Release.Task.WaitAsync(stop.Token); }
                    var frame = Encoding.ASCII.GetBytes("OK revision=1 active=" + new string('1', 64)
                        + " rollback=- staged=- durable=1 reason=ok\n");
                    await stream.WriteAsync(frame, stop.Token);
                }
                Completed.TrySetResult();
            }
            catch (Exception error) { Reached.TrySetException(error); Completed.TrySetException(error); }
        }
        public void Dispose()
        {
            stop.Cancel(); Release.TrySetResult(); control.Dispose(); ask.Dispose();
            server.GetAwaiter().GetResult(); stop.Dispose();
        }
    }
}
