using System.Globalization;
using System.Net.Sockets;
using System.Runtime.Versioning;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using CnetControlPlane.Learning;
using Xunit;

namespace CnetControlPlane.Tests;

[SupportedOSPlatform("linux")]
public sealed class LearningControlClientTests : IDisposable
{
    private readonly string root = Directory.CreateTempSubdirectory("cnet-control-client-").FullName;
    private const UnixFileMode PrivateDirectory = UnixFileMode.UserRead | UnixFileMode.UserWrite | UnixFileMode.UserExecute;
    private readonly LearningRuntime runtime;
    private string SocketPath => Path.Combine(root, "control.sock");
    private static string H(int value) => value.ToString("x64");
    private static ControlStatus Initial(ulong revision = 1) => new(true, revision, H(1), null, null, true, ControlReason.Ok);
    private static string Frame(ControlStatus status) => $"{(status.Ok ? "OK" : "ERR")} revision={status.Revision.ToString(CultureInfo.InvariantCulture)} active={status.Active ?? "-"} rollback={status.Rollback ?? "-"} staged={status.Staged ?? "-"} durable={(status.Durable ? 1 : 0)} reason={status.Reason switch { ControlReason.Ok => "ok", ControlReason.Refused => "refused", _ => "durability_uncertain" }}\n";

    public LearningControlClientTests()
    {
        File.SetUnixFileMode(root, PrivateDirectory);
        var installed = Path.Combine(root, "runtime");
        Directory.CreateDirectory(installed, PrivateDirectory);
        var repo = new DirectoryInfo(AppContext.BaseDirectory);
        while (repo is not null && !File.Exists(Path.Combine(repo.FullName, "bin/cnet_capsulectl"))) repo = repo.Parent;
        Assert.NotNull(repo);
        var hashes = new Dictionary<string, string>();
        foreach (var name in new[] { "cnet_table_capsule", "cnet_table_verify", "cnet_learning_snapshot", "cnet_capsulectl", "cnetd", "libcnet_capsule_core.so" })
        {
            // The actual CLI loads the actual copied core library via $ORIGIN.
            // The other four fixed copies satisfy inventory and never execute.
            var source = name is "cnet_capsulectl" or "libcnet_capsule_core.so"
                ? Path.Combine(repo!.FullName, "bin", name) : "/usr/bin/true";
            var target = Path.Combine(installed, name);
            File.Copy(source, target);
            File.SetUnixFileMode(target, name.EndsWith(".so", StringComparison.Ordinal)
                ? UnixFileMode.UserRead : UnixFileMode.UserRead | UnixFileMode.UserExecute);
            hashes[name] = Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(target))).ToLowerInvariant();
        }
        runtime = LearningRuntime.Load(installed, JsonSerializer.SerializeToUtf8Bytes(new { schema_version = 1, files = hashes }));
    }
    public void Dispose() { runtime.Dispose(); Directory.Delete(root, recursive: true); }

    private sealed class Server : IAsyncDisposable
    {
        private readonly Socket listener = new(AddressFamily.Unix, SocketType.Stream, ProtocolType.Unspecified);
        private readonly CancellationTokenSource stop = new();
        private readonly Task exchange;
        public TaskCompletionSource<string> Received { get; } = new(TaskCreationOptions.RunContinuationsAsynchronously);
        public bool ExtraConnection => listener.Poll(0, SelectMode.SelectRead);
        public Server(string path, string response, bool hold = false)
        {
            listener.Bind(new UnixDomainSocketEndPoint(path));
            File.SetUnixFileMode(path, UnixFileMode.UserRead | UnixFileMode.UserWrite);
            listener.Listen(2);
            exchange = Exchange(Encoding.ASCII.GetBytes(response), hold);
        }
        private async Task Exchange(byte[] response, bool hold)
        {
            try
            {
                using var peer = await listener.AcceptAsync(stop.Token);
                using var stream = new NetworkStream(peer, ownsSocket: false);
                var bytes = new byte[160];
                var count = 0;
                while (count < bytes.Length)
                {
                    var got = await stream.ReadAsync(bytes.AsMemory(count), stop.Token);
                    if (got == 0) throw new InvalidOperationException("test_control_request_eof");
                    count += got;
                    if (bytes[count - 1] == '\n') break;
                }
                Received.TrySetResult(Encoding.ASCII.GetString(bytes, 0, count));
                await stream.WriteAsync(response, stop.Token);
                if (hold) await Task.Delay(Timeout.Infinite, stop.Token);
            }
            catch (Exception exception) when (stop.IsCancellationRequested && exception is OperationCanceledException or ObjectDisposedException or SocketException)
            { Received.TrySetCanceled(); }
        }
        public async ValueTask DisposeAsync()
        {
            stop.Cancel(); listener.Dispose();
            await exchange;
            stop.Dispose();
        }
    }

    [Fact]
    public async Task StatusUsesActualOwnerCheckedNativeCliAndReturnsExactFrame()
    {
        var expected = Initial();
        await using var server = new Server(SocketPath, Frame(expected));
        var client = new LearningControlClient(runtime, SocketPath, 2);
        Assert.Equal(expected, await client.StatusAsync(default));
        Assert.Equal("STATUS\n", await server.Received.Task);
        Assert.False(server.ExtraConnection);
    }

    [Theory]
    [InlineData("stage")]
    [InlineData("activate")]
    [InlineData("rollback")]
    [InlineData("discard")]
    public async Task OnlyExactPersistedOperationIsSentOnce(string kind)
    {
        var before = Initial();
        LearningIntent intent;
        switch (kind)
        {
            case "stage": intent = new(1, 2, kind, NativeControlProtocol.FormatStage(1, "candidate"), before, before with { Staged = H(3) }); break;
            case "activate":
                before = before with { Staged = H(3) };
                intent = new(1, 2, kind, NativeControlProtocol.FormatActivate(1, "stable_token", H(3)), before,
                    before with { Revision = 2, Active = H(3), Rollback = H(1), Staged = null }); break;
            case "rollback":
                before = before with { Rollback = H(3) };
                intent = new(1, 2, kind, NativeControlProtocol.FormatRollback(1, "stable_token"), before,
                    before with { Revision = 2, Active = H(3), Rollback = H(1) }); break;
            default:
                before = before with { Staged = H(3) };
                intent = new(1, 2, kind, NativeControlProtocol.FormatDiscard(1, H(3)), before, before with { Staged = null }); break;
        }
        await using var server = new Server(SocketPath, Frame(intent.Expected));
        var client = new LearningControlClient(runtime, SocketPath, 2);
        Assert.Equal(intent.Expected, await client.SendAsync(intent, default));
        Assert.Equal(intent.Request, await server.Received.Task);
        Assert.False(server.ExtraConnection);
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task RefusalAndOkWithoutDurabilityAreReturnedWithoutInference(bool refused)
    {
        var expected = refused ? Initial() with { Ok = false, Reason = ControlReason.Refused } : Initial() with { Durable = false };
        await using var server = new Server(SocketPath, Frame(expected));
        Assert.Equal(expected, await new LearningControlClient(runtime, SocketPath, 2).StatusAsync(default));
    }

    private static LearningIntent StageIntent() => new(1, 2, "stage", "STAGE 1 1 candidate\n", Initial(), Initial() with { Staged = H(3) });

    [Fact]
    public async Task MalformedIntentsRefuseBeforeRuntimeAccessOrProcessLaunch()
    {
        var valid = StageIntent();
        var client = new LearningControlClient(runtime, SocketPath, 2);
        runtime.Dispose(); // Any attempted runtime access would give outcome_unknown, not argument refusal.
        foreach (var invalid in new[]
        {
            valid with { Id = 0 }, valid with { JobId = -1 }, valid with { Kind = "unload" }, valid with { Kind = "status" },
            valid with { Request = "UNLOAD 1 token\n" }, valid with { Request = "STATUS\n" },
            valid with { Request = "STAGE 1 0 candidate\n" }, valid with { Request = "STAGE 01 1 candidate\n" },
            valid with { Request = "STAGE 2 1 candidate\n" }, valid with { Request = "STAGE 1 1 ../candidate\n" },
            valid with { Request = "STAGE 1 1 " + new string('a', 64) + "\n" },
            valid with { Request = "STAGE 1 1 candidate\nSTATUS\n" }, valid with { Request = "STAGE 1 1 candidate\r\n" },
            valid with { Request = "STAGE 1 1 candidate\0\n" }, valid with { Request = "STAGE 1  1 candidate\n" },
            valid with { Request = "STAGE 1 1 candidate" }, valid with { Request = "STAGE 1 1 candidate \n" },
            valid with { Before = valid.Before with { Durable = false } },
            valid with { Before = valid.Before with { Ok = false, Reason = ControlReason.Refused } },
            valid with { Before = valid.Before with { Active = null } },
            valid with { Before = valid.Before with { Active = new string('x', 64) } },
            valid with { Before = valid.Before with { Revision = 0 } },
            valid with { Before = valid.Before with { Staged = H(4) } },
            valid with { Expected = valid.Expected with { Durable = false } },
            valid with { Expected = valid.Expected with { Active = H(9) } },
            valid with { Expected = valid.Expected with { Revision = 2 } },
            valid with { Expected = valid.Expected with { Rollback = H(9) } },
            valid with { Expected = valid.Expected with { Staged = null } },
            valid with { Expected = valid.Expected with { Reason = (ControlReason)99 } },
            valid with { Before = null! }, valid with { Expected = null! }, valid with { Request = null! }, null!
        }) Assert.Equal("learning_control_intent", (await Assert.ThrowsAsync<ArgumentException>(() => client.SendAsync(invalid, default))).Message);
    }

    [Theory]
    [InlineData("activate")]
    [InlineData("rollback")]
    [InlineData("discard")]
    public async Task InvalidMutationTransformsNeverLaunch(string kind)
    {
        var before = Initial() with { Staged = kind == "rollback" ? null : H(3), Rollback = kind == "rollback" ? H(3) : null };
        var request = kind switch
        {
            "activate" => NativeControlProtocol.FormatActivate(1, "token", H(3)),
            "rollback" => NativeControlProtocol.FormatRollback(1, "token"),
            _ => NativeControlProtocol.FormatDiscard(1, H(3))
        };
        var expected = kind == "discard" ? before with { Staged = null } :
            before with { Revision = 2, Active = H(3), Rollback = H(1), Staged = null };
        var intent = new LearningIntent(1, 2, kind, request, before, expected);
        var client = new LearningControlClient(runtime, SocketPath, 2);
        runtime.Dispose();
        foreach (var invalid in new[]
        {
            intent with { Request = request.Replace("token", "bad/token").Replace(H(3), H(4)) },
            intent with { Expected = expected with { Revision = 9 } },
            intent with { Expected = expected with { Active = H(9) } },
            intent with { Expected = expected with { Rollback = H(9) } },
            intent with { Expected = expected with { Staged = H(9) } },
            intent with { Before = before with { Staged = kind == "rollback" ? H(9) : null } },
            intent with { Before = before with { Rollback = null, Staged = null } }
        }) await Assert.ThrowsAsync<ArgumentException>(() => client.SendAsync(invalid, default));
    }

    [Theory]
    [InlineData("stage")]
    [InlineData("discard")]
    public async Task NonincrementingOperationsAllowUInt64MaximumRevision(string kind)
    {
        var before = Initial(ulong.MaxValue) with { Staged = kind == "discard" ? H(3) : null };
        var request = kind == "stage" ? NativeControlProtocol.FormatStage(before.Revision, new string('s', 63)) :
            NativeControlProtocol.FormatDiscard(before.Revision, H(3));
        var expected = before with { Staged = kind == "stage" ? H(3) : null };
        var intent = new LearningIntent(1, 2, kind, request, before, expected);
        await using var server = new Server(SocketPath, Frame(expected));
        Assert.Equal(expected, await new LearningControlClient(runtime, SocketPath, 2).SendAsync(intent, default));
        Assert.Equal(request, await server.Received.Task);
    }

    [Theory]
    [InlineData("activate")]
    [InlineData("rollback")]
    public async Task IncrementingOperationsRefuseUInt64MaximumBeforeLaunch(string kind)
    {
        var before = Initial(ulong.MaxValue) with { Staged = kind == "activate" ? H(3) : null, Rollback = kind == "rollback" ? H(3) : null };
        var intent = new LearningIntent(1, 2, kind, $"{kind.ToUpperInvariant()} 18446744073709551615 token{(kind == "activate" ? " " + H(3) : "")}\n",
            before, before with { Revision = 1, Active = H(3), Rollback = H(1), Staged = null });
        await Assert.ThrowsAsync<ArgumentException>(() => new LearningControlClient(runtime, SocketPath, 2).SendAsync(intent, default));
    }

    [Theory]
    [InlineData("garbage")]
    [InlineData("extra_line")]
    [InlineData("oversized")]
    [InlineData("empty")]
    [InlineData("malformed_err")]
    public async Task NativeTransportAndParseFailuresAreUnknownWithoutRetryOrDiagnosticLeak(string kind)
    {
        var response = kind switch
        {
            "garbage" => "untrusted instruction text\n", "extra_line" => Frame(Initial()) + "STATUS\n",
            "oversized" => new string('x', 399), "empty" => "", _ => Frame(Initial()).Replace("OK ", "ERR ")
        };
        await using var server = new Server(SocketPath, response);
        var error = await Assert.ThrowsAsync<InvalidOperationException>(() => new LearningControlClient(runtime, SocketPath, 2).SendAsync(StageIntent(), default));
        Assert.Equal("learning_control_outcome_unknown", error.Message);
        Assert.Null(error.InnerException);
        Assert.Equal(StageIntent().Request, await server.Received.Task);
        Assert.False(server.ExtraConnection);
    }

    [Fact]
    public async Task ValidUnexpectedReplyIsReturnedForLedgerReconciliation()
    {
        var reply = Initial(7) with { Active = H(9) };
        await using var server = new Server(SocketPath, Frame(reply));
        Assert.Equal(reply, await new LearningControlClient(runtime, SocketPath, 2).SendAsync(StageIntent(), default));
        Assert.False(server.ExtraConnection);
    }

    [Fact]
    public async Task MissingSocketAndModifiedRuntimeAreUnknown()
    {
        var client = new LearningControlClient(runtime, SocketPath, 2);
        Assert.Equal("learning_control_outcome_unknown", (await Assert.ThrowsAsync<InvalidOperationException>(() => client.StatusAsync(default))).Message);
        var library = Path.Combine(root, "runtime/libcnet_capsule_core.so");
        File.SetUnixFileMode(library, UnixFileMode.UserRead | UnixFileMode.UserWrite);
        File.WriteAllBytes(library, [0]);
        File.SetUnixFileMode(library, UnixFileMode.UserRead);
        await using var server = new Server(SocketPath, Frame(Initial()));
        Assert.Equal("learning_control_outcome_unknown", (await Assert.ThrowsAsync<InvalidOperationException>(() => client.StatusAsync(default))).Message);
        Assert.False(server.Received.Task.IsCompleted);
    }

    [Fact]
    public async Task DeadlineRequiresEofWithoutRetryOrOutcomeInference()
    {
        await using var server = new Server(SocketPath, Frame(Initial()), hold: true);
        var error = await Assert.ThrowsAsync<InvalidOperationException>(() => new LearningControlClient(runtime, SocketPath, 1).SendAsync(StageIntent(), default));
        Assert.Equal("learning_control_outcome_unknown", error.Message);
        Assert.False(server.ExtraConnection);
    }

    [Fact]
    public async Task CancellationAfterSendDoesNotBecomeAnOutcomeOrTriggerRetry()
    {
        using var cancellation = new CancellationTokenSource();
        await using var server = new Server(SocketPath, "", hold: true);
        var execution = new LearningControlClient(runtime, SocketPath, 10).SendAsync(StageIntent(), cancellation.Token);
        Assert.Equal(StageIntent().Request, await server.Received.Task.WaitAsync(TimeSpan.FromSeconds(3)));
        cancellation.Cancel();
        await Assert.ThrowsAnyAsync<OperationCanceledException>(() => execution);
        Assert.False(server.ExtraConnection);
    }

    [Theory]
    [InlineData("relative", 1)]
    [InlineData("/tmp/control\0.sock", 1)]
    [InlineData("/tmp/control.sock", 0)]
    [InlineData("/tmp/control.sock", 61)]
    public void InvalidConstructorArgumentsRefuse(string path, int seconds) =>
        Assert.Equal("learning_control_arguments", Assert.Throws<ArgumentException>(() => new LearningControlClient(runtime, path, seconds)).Message);

    [Fact]
    public void UnixSocketBoundCountsUtf8BytesAndNullRuntimeRefuses()
    {
        Assert.Throws<ArgumentException>(() => new LearningControlClient(runtime, "/" + new string('é', 54), 1));
        Assert.Throws<ArgumentException>(() => new LearningControlClient(runtime, "/" + new string('a', 107), 1));
        Assert.Throws<ArgumentException>(() => new LearningControlClient(null!, SocketPath, 1));
    }
}
