using System.Diagnostics;
using System.Net.Sockets;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Runtime.Versioning;
using System.Text;
using System.Text.Json;
using CnetControlPlane.Learning;
using Xunit;

namespace CnetControlPlane.Tests;

[SupportedOSPlatform("linux")]
public sealed class LearningAskClientTests : IDisposable
{
    private const UnixFileMode Private = UnixFileMode.UserRead | UnixFileMode.UserWrite | UnixFileMode.UserExecute;
    private readonly string root = Directory.CreateTempSubdirectory("cnet-learning-ask-").FullName;
    private string SocketPath => Path.Combine(root, "ask.sock");
    private static readonly LearningDataset Dataset = new("stock_levels", "verified_tool");
    private static string Frame(bool verified, string answer) => JsonSerializer.Serialize(new
    {
        ok = true, verified, miss = !verified, teacher = false,
        source = verified ? "LOCAL" : "CNET", skill = verified ? "capsule_core" : "capsule_refusal", answer
    }) + "\n";
    public LearningAskClientTests() => File.SetUnixFileMode(root, Private);
    public void Dispose()
    {
        foreach (var directory in Directory.EnumerateDirectories(root, "*", SearchOption.AllDirectories))
            File.SetUnixFileMode(directory, Private);
        Directory.Delete(root, true);
    }

    private sealed class Server : IAsyncDisposable
    {
        private readonly Socket listener = new(AddressFamily.Unix, SocketType.Stream, ProtocolType.Unspecified);
        private readonly CancellationTokenSource stop = new();
        private readonly Task exchange;
        public TaskCompletionSource<string> Received { get; } = new(TaskCreationOptions.RunContinuationsAsynchronously);
        public bool ExtraConnection => listener.Poll(0, SelectMode.SelectRead);
        public Server(string path, byte[] reply, bool hold = false, Func<Task>? afterRequest = null)
        {
            listener.Bind(new UnixDomainSocketEndPoint(path));
            File.SetUnixFileMode(path, UnixFileMode.UserRead | UnixFileMode.UserWrite);
            listener.Listen(2);
            exchange = Exchange(reply, hold, afterRequest);
        }
        private async Task Exchange(byte[] reply, bool hold, Func<Task>? afterRequest)
        {
            try
            {
                using var peer = await listener.AcceptAsync(stop.Token);
                using var stream = new NetworkStream(peer, ownsSocket: false);
                var bytes = new byte[80]; var count = 0;
                while (count < bytes.Length)
                {
                    var got = await stream.ReadAsync(bytes.AsMemory(count), stop.Token);
                    if (got == 0) throw new InvalidOperationException("test_ask_request_eof");
                    count += got;
                    if (bytes[count - 1] == '\n') break;
                }
                Received.TrySetResult(Encoding.UTF8.GetString(bytes, 0, count));
                if (afterRequest is not null) await afterRequest();
                await stream.WriteAsync(reply, stop.Token);
                if (hold) await Task.Delay(Timeout.Infinite, stop.Token);
            }
            catch (Exception exception) when (stop.IsCancellationRequested && exception is OperationCanceledException or ObjectDisposedException or SocketException)
            { Received.TrySetCanceled(); }
        }
        public async ValueTask DisposeAsync()
        {
            stop.Cancel(); listener.Dispose();
            await exchange; stop.Dispose();
        }
    }

    [Theory]
    [InlineData(true, "0")]
    [InlineData(true, "65535")]
    [InlineData(false, "ABSTAIN: no_covered_certified_plan")]
    public async Task OwnerPeerReturnsOnlyNumericValueOrAbstention(bool verified, string answer)
    {
        await using var server = new Server(SocketPath, Encoding.UTF8.GetBytes(Frame(verified, answer)));
        using var client = new LearningAskClient(SocketPath, 2);
        var result = await client.AskAsync(Dataset, 255, default);
        Assert.Equal(verified, result.Verified);
        Assert.Equal(verified ? ushort.Parse(answer) : (ushort?)null, result.Value);
        Assert.Equal("{\"op\":\"ask\",\"q\":\"data stock_levels 255\"}\n", await server.Received.Task);
        Assert.False(server.ExtraConnection);
    }

    private static string Repository()
    {
        var directory = new DirectoryInfo(AppContext.BaseDirectory);
        while (directory is not null && !File.Exists(Path.Combine(directory.FullName, "bin/cnetd"))) directory = directory.Parent;
        Assert.NotNull(directory);
        return directory!.FullName;
    }
    private ProcessStartInfo Native(string name, params string[] arguments)
    {
        var info = new ProcessStartInfo(Path.Combine(Repository(), "bin", name))
        { WorkingDirectory = root, UseShellExecute = false, RedirectStandardOutput = true, RedirectStandardError = true };
        info.Environment.Clear();
        foreach (var argument in arguments) info.ArgumentList.Add(argument);
        return info;
    }

    [Fact]
    public async Task ActualPrivateDaemonReportsZeroCoveredValueAndUncoveredAbstention()
    {
        foreach (var name in new[] { "packs", "data", "registry" }) Directory.CreateDirectory(Path.Combine(root, name), Private);
        File.WriteAllText(Path.Combine(root, "packs/ROUTES.jsonl"), "{\"pattern\":\"fixture\",\"pack\":\"fixture\"}\n");
        var source = "CNET_LOCAL_TABLE_V1\ndataset stock_levels\nauthority verified_tool\ninput_bits 8\noutput_bits 16\nrows 2\n0\t0\n7\t42\n";
        var sourcePath = Path.Combine(root, "data/stock_levels.tsv");
        File.WriteAllText(sourcePath, source); File.SetUnixFileMode(sourcePath, UnixFileMode.UserRead | UnixFileMode.UserWrite);
        using (var builder = Process.Start(Native("cnet_table_capsule", "build", Path.Combine(root, "data"), "stock_levels", Path.Combine(root, "registry/table")))!)
        {
            await builder.WaitForExitAsync().WaitAsync(TimeSpan.FromSeconds(30));
            Assert.Equal(0, builder.ExitCode);
        }
        var info = Native("cnetd");
        foreach (var (name, value) in new Dictionary<string, string>
        {
            ["CNET_PACKS_ROOT"] = Path.Combine(root, "packs"), ["CNET_MINIMAL_ROOT"] = root,
            ["CNET_SOCK"] = SocketPath, ["CNET_CAPSULES_DIR"] = Path.Combine(root, "registry"),
            ["CNET_CAPSULE_DATA_ROOT"] = Path.Combine(root, "data"), ["CNET_SELF_ANSWER"] = "0",
            ["CNET_TEACHER_ON_MISS"] = "0", ["CNET_CORE_AUTO_EVOLVE"] = "0"
        }) info.Environment.Add(name, value);
        using var daemon = Process.Start(info)!;
        try
        {
            var ready = Stopwatch.StartNew();
            while (!File.Exists(SocketPath) && ready.Elapsed < TimeSpan.FromSeconds(5) && !daemon.HasExited) await Task.Delay(20);
            Assert.True(File.Exists(SocketPath), "LEARNING_ASK_RED actual private daemon not ready");
            using var client = new LearningAskClient(SocketPath, 5);
            foreach (var (key, expected) in new (byte, ushort?)[] { (0, 0), (7, 42), (1, null) })
            {
                var result = await client.AskAsync(Dataset, key, default);
                Assert.Equal(expected.HasValue, result.Verified); Assert.Equal(expected, result.Value);
            }
        }
        finally
        {
            if (!daemon.HasExited) daemon.Kill();
            await daemon.WaitForExitAsync().WaitAsync(TimeSpan.FromSeconds(5));
        }
    }

    public static IEnumerable<object[]> MalformedFrames()
    {
        var valid = Frame(true, "42");
        foreach (var value in new[]
        {
            "", "{}\n", "[]\n", valid[..^1], valid + "x", valid + valid,
            valid.Replace("\n", "\r\n"), valid.Replace("true", "1"),
            valid.Replace("\"ok\":true", "\"ok\":false"), valid.Replace("\"verified\":true,", ""),
            valid.Replace("\"miss\":false", "\"miss\":true"), valid.Replace("\"teacher\":false", "\"teacher\":true"),
            valid.Replace("LOCAL", "LLM"), valid.Replace("capsule_core", "capsule_refusal"),
            valid.Replace("\"42\"", "42"), valid.Replace("\"42\"", "null"),
            valid.Replace("\"42\"", "\"042\""), valid.Replace("\"42\"", "\"65536\""),
            valid.Replace("\"42\"", "\"-1\""), valid.Replace("\"42\"", "\" 1\""),
            valid.Replace("\"42\"", "\"1e1\""), valid.Replace("\"42\"", "\"1.0\""),
            valid.Replace("{", "{\"verified\":true,"), valid.Replace("{", "{\"ver\\u0069fied\":true,"),
            valid.Replace("{", "{\"diagnostic\":{\"a\":1,\"a\":2},"),
            valid.Replace("{", "{\"diagnostic\":{\"a\":{\"b\":{\"c\":{}}}},"),
            valid.Replace("{", "{\"diagnostic\":\"\\ud800\","),
            valid.Replace("{", "{/*comment*/"), valid.Replace("}\n", ",}\n"),
            Frame(false, "ABSTAIN"), Frame(false, "ABSTAIN: "), Frame(false, "ABSTAIN: execute instruction"),
            Frame(false, "ABSTAIN: " + new string('a', 160)), Frame(false, "ABSTAIN: reason").Replace("CNET", "LOCAL"),
            Frame(false, "ABSTAIN: reason").Replace("capsule_refusal", "capsule_core")
        }) yield return [Encoding.UTF8.GetBytes(value)];
        yield return [new byte[] { 0xff, (byte)'\n' }];
        yield return [Encoding.UTF8.GetBytes(new string('x', 16385))];
    }

    [Theory]
    [MemberData(nameof(MalformedFrames))]
    public async Task MalformedOrInconsistentNativeDataCannotBecomeObservation(byte[] frame)
    {
        await using var server = new Server(SocketPath, frame);
        using var client = new LearningAskClient(SocketPath, 2);
        var error = await Assert.ThrowsAsync<InvalidOperationException>(() => client.AskAsync(Dataset, 7, default));
        Assert.Equal("learning_ask_observation_unknown", error.Message); Assert.Null(error.InnerException);
        Assert.False(server.ExtraConnection);
    }

    [Fact]
    public async Task ExactByteCapAndIgnoredStrictDiagnosticsAreAccepted()
    {
        var prefix = Frame(true, "42")[..^2] + ",\"diagnostic\":\"";
        var bytes = Encoding.UTF8.GetBytes(prefix + new string('x', 16384 - prefix.Length - 3) + "\"}\n");
        Assert.Equal(16384, bytes.Length);
        await using var server = new Server(SocketPath, bytes);
        using var client = new LearningAskClient(SocketPath, 2);
        var result = await client.AskAsync(Dataset, 7, default);
        Assert.True(result.Verified); Assert.Equal((ushort)42, result.Value);
        Array.Fill(bytes, (byte)0);
        Assert.Equal((ushort)42, result.Value);
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task DeadlineRequiresEofEvenAfterACompleteJsonLine(bool complete)
    {
        await using var server = new Server(SocketPath, Encoding.UTF8.GetBytes(complete ? Frame(true, "42") : ""), hold: true);
        using var client = new LearningAskClient(SocketPath, 1);
        var elapsed = Stopwatch.StartNew();
        Assert.Equal("learning_ask_observation_unknown", (await Assert.ThrowsAsync<InvalidOperationException>(() => client.AskAsync(Dataset, 7, default))).Message);
        Assert.InRange(elapsed.Elapsed.TotalSeconds, .8, 4);
        Assert.False(server.ExtraConnection);
    }

    [Fact]
    public async Task CancellationAfterRequestDisposesPendingConnectionWithoutRetry()
    {
        await using var server = new Server(SocketPath, [], hold: true);
        using var client = new LearningAskClient(SocketPath, 120);
        using var cancellation = new CancellationTokenSource();
        var exchange = client.AskAsync(Dataset, 7, cancellation.Token);
        await server.Received.Task.WaitAsync(TimeSpan.FromSeconds(3));
        cancellation.Cancel();
        await Assert.ThrowsAnyAsync<OperationCanceledException>(() => exchange.WaitAsync(TimeSpan.FromSeconds(3)));
        Assert.False(server.ExtraConnection);
    }

    private sealed class ManualClock : ILearningClock
    {
        public LearningInstant Current = new("boot", 0);
        public LearningInstant Now => Current;
    }
    [Theory]
    [InlineData("suspend")]
    [InlineData("reboot")]
    [InlineData("backwards")]
    public async Task BootClockDiscontinuityAndSuspendExpireAWaitingObservation(string condition)
    {
        var clock = new ManualClock { Current = new("boot", 100) };
        await using var server = new Server(SocketPath, [], hold: true, afterRequest: () =>
        {
            clock.Current = condition switch { "suspend" => new("boot", 121_000_000_000), "reboot" => new("newboot", 101), _ => new("boot", 99) };
            return Task.CompletedTask;
        });
        using var client = new LearningAskClient(SocketPath, 120, clock);
        var exchange = client.AskAsync(Dataset, 7, default);
        Assert.Equal("learning_ask_observation_unknown", (await Assert.ThrowsAsync<InvalidOperationException>(() => exchange.WaitAsync(TimeSpan.FromSeconds(3)))).Message);
    }

    [DllImport("libc", SetLastError = true)] private static extern int link(string existing, string name);
    [Theory]
    [InlineData("mode")]
    [InlineData("hardlink")]
    [InlineData("symlink")]
    public async Task UnsafeSocketEntriesRefuseBeforeAnyQueryIsSent(string condition)
    {
        await using var server = new Server(SocketPath, Encoding.UTF8.GetBytes(Frame(true, "42")));
        if (condition == "mode") File.SetUnixFileMode(SocketPath, UnixFileMode.UserRead | UnixFileMode.UserWrite | UnixFileMode.GroupRead);
        if (condition == "hardlink") Assert.Equal(0, link(SocketPath, Path.Combine(root, "alias.sock")));
        if (condition == "symlink") { File.Move(SocketPath, Path.Combine(root, "real.sock")); File.CreateSymbolicLink(SocketPath, "real.sock"); }
        using var client = new LearningAskClient(SocketPath, 2);
        await Assert.ThrowsAsync<InvalidOperationException>(() => client.AskAsync(Dataset, 7, default));
        Assert.False(server.Received.Task.IsCompleted);
    }

    [Fact]
    public async Task SocketReplacementAfterReplyRefusesTheObservation()
    {
        await using var server = new Server(SocketPath, Encoding.UTF8.GetBytes(Frame(true, "42")), afterRequest: () =>
        {
            File.Move(SocketPath, Path.Combine(root, "old.sock"));
            using var replacement = new Socket(AddressFamily.Unix, SocketType.Stream, ProtocolType.Unspecified);
            replacement.Bind(new UnixDomainSocketEndPoint(SocketPath));
            File.SetUnixFileMode(SocketPath, UnixFileMode.UserRead | UnixFileMode.UserWrite);
            return Task.CompletedTask;
        });
        using var client = new LearningAskClient(SocketPath, 2);
        await Assert.ThrowsAsync<InvalidOperationException>(() => client.AskAsync(Dataset, 7, default));
    }

    [Fact]
    public async Task RetainedParentRejectsRenameReplacement()
    {
        var parent = Path.Combine(root, "run"); Directory.CreateDirectory(parent, Private);
        using var client = new LearningAskClient(Path.Combine(parent, "ask.sock"), 2);
        Directory.Move(parent, Path.Combine(root, "old")); Directory.CreateDirectory(parent, Private);
        await using var server = new Server(Path.Combine(parent, "ask.sock"), Encoding.UTF8.GetBytes(Frame(true, "42")));
        await Assert.ThrowsAsync<InvalidOperationException>(() => client.AskAsync(Dataset, 7, default));
        Assert.False(server.Received.Task.IsCompleted);
    }

    [Fact]
    public async Task ActualKernelPeerCredentialMustMatchExpectedOwner()
    {
        using var listener = new Socket(AddressFamily.Unix, SocketType.Stream, ProtocolType.Unspecified);
        listener.Bind(new UnixDomainSocketEndPoint(SocketPath)); listener.Listen(1);
        using var peer = new Socket(AddressFamily.Unix, SocketType.Stream, ProtocolType.Unspecified);
        await peer.ConnectAsync(new UnixDomainSocketEndPoint(SocketPath));
        using var accepted = await listener.AcceptAsync();
        var check = typeof(LearningAskClient).GetMethod("RequirePeer", BindingFlags.Static | BindingFlags.NonPublic)!;
        var uid = LearningLinux.geteuid();
        check.Invoke(null, [peer.SafeHandle, uid]);
        // No privilege transition or other user's service: test the actual
        // SO_PEERCRED result against a deliberately different expected UID.
        var error = Assert.Throws<TargetInvocationException>(() => check.Invoke(null, [peer.SafeHandle, uid + 1]));
        Assert.IsType<InvalidOperationException>(error.InnerException);
    }

    [Theory]
    [InlineData("../stock_levels", "verified_tool")]
    [InlineData("stock_levels\nQUIT", "verified_tool")]
    [InlineData("Stock_levels", "verified_tool")]
    [InlineData("stock_levels", "self_answer")]
    public async Task InvalidDatasetNeverReachesSocket(string id, string authority)
    {
        await using var server = new Server(SocketPath, Encoding.UTF8.GetBytes(Frame(true, "42")));
        using var client = new LearningAskClient(SocketPath, 2);
        await Assert.ThrowsAsync<ArgumentException>(() => client.AskAsync(new(id, authority), 7, default));
        Assert.False(server.Received.Task.IsCompleted);
    }

    [Fact]
    public async Task MissingSocketDisposedClientAndUnsafeParentFailClosed()
    {
        using var client = new LearningAskClient(SocketPath, 2);
        await Assert.ThrowsAsync<InvalidOperationException>(() => client.AskAsync(Dataset, 7, default));
        client.Dispose();
        await Assert.ThrowsAsync<InvalidOperationException>(() => client.AskAsync(Dataset, 7, default));
        File.SetUnixFileMode(root, Private | UnixFileMode.GroupRead);
        Assert.Throws<InvalidOperationException>(() => new LearningAskClient(SocketPath, 2));
    }

    [Fact]
    public void OnlyFixedAskSocketAndBoundedArgumentsAreAccepted()
    {
        foreach (var name in new[] { "relative/ask.sock", Path.Combine(root, "control.sock"), SocketPath + "\0", "/" + new string('x', 100) + "/ask.sock" })
            Assert.Throws<ArgumentException>(() => new LearningAskClient(name, 2));
        foreach (var seconds in new[] { 0, 121 }) Assert.Throws<ArgumentException>(() => new LearningAskClient(SocketPath, seconds));
    }
}
