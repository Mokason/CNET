using System.Globalization;
using System.Net.Sockets;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;
using static CnetControlPlane.Learning.LearningLinux;

namespace CnetControlPlane.Learning;

internal sealed class LearningAskResult
{
    public bool Verified { get; }
    public ushort? Value { get; }
    public string? Text { get; }
    private LearningAskResult(bool verified, ushort? value, string? text = null) { Verified = verified; Value = value; Text = text; }

    internal static LearningAskResult Parse(byte[] bytes) => ParseCore(bytes, symbolic: false);
    internal static LearningAskResult ParseSymbol(byte[] bytes) => ParseCore(bytes, symbolic: true);

    private static LearningAskResult ParseCore(byte[] bytes, bool symbolic)
    {
        if (bytes is null || bytes.Length is < 1 or > 16384) throw new InvalidOperationException("learning_ask_reply_invalid");
        var frame = (byte[])bytes.Clone();
        if (frame[^1] != '\n' || frame.AsSpan(0, frame.Length - 1).ContainsAny((byte)'\n', (byte)'\r'))
            throw new InvalidOperationException("learning_ask_reply_invalid");
        try
        {
            _ = new UTF8Encoding(false, true).GetCharCount(frame);
            using var document = JsonDocument.Parse(frame.AsMemory(0, frame.Length - 1), new JsonDocumentOptions { MaxDepth = 4 });
            CheckTree(document.RootElement);
            var top = document.RootElement;
            if (top.ValueKind != JsonValueKind.Object || top.GetProperty("ok").ValueKind != JsonValueKind.True
                || top.GetProperty("teacher").ValueKind != JsonValueKind.False)
                throw new InvalidOperationException();
            var verified = top.GetProperty("verified").GetBoolean();
            if (top.GetProperty("miss").GetBoolean() == verified
                || top.GetProperty("source").GetString() != (verified ? "LOCAL" : "CNET")
                || top.GetProperty("skill").GetString() != (verified ? "capsule_core" : "capsule_refusal"))
                throw new InvalidOperationException();
            var answer = top.GetProperty("answer").GetString();
            if (verified)
            {
                if (symbolic)
                {
                    if (!LocalSymbolReference.IsLabel(answer)) throw new InvalidOperationException();
                    return new(true, null, answer); // Literal text, including numeric-looking labels.
                }
                if (answer is not { Length: >= 1 and <= 5 } || answer.Length > 1 && answer[0] == '0'
                    || answer.Any(c => c is < '0' or > '9')
                    || !ushort.TryParse(answer, NumberStyles.None, CultureInfo.InvariantCulture, out var value))
                    throw new InvalidOperationException();
                return new(true, value);
            }
            // This is native refusal syntax, not an instruction or a label.
            // CnetCapsuleCoreReply.reason is a fixed 160-byte C buffer.
            if (answer is not { Length: >= 10 and <= 168 } || !answer.StartsWith("ABSTAIN: ", StringComparison.Ordinal)
                || answer[9..].Any(c => c is not (>= 'a' and <= 'z' or '_')))
                throw new InvalidOperationException();
            return new(false, null);
        }
        catch (Exception exception) when (exception is JsonException or InvalidOperationException or ArgumentException or KeyNotFoundException)
        { throw new InvalidOperationException("learning_ask_reply_invalid"); }
    }

    private static void CheckTree(JsonElement element)
    {
        if (element.ValueKind == JsonValueKind.Object)
        {
            var names = new HashSet<string>(StringComparer.Ordinal);
            foreach (var property in element.EnumerateObject())
            {
                if (!names.Add(property.Name)) throw new InvalidOperationException();
                CheckTree(property.Value);
            }
        }
        else if (element.ValueKind == JsonValueKind.Array)
            foreach (var child in element.EnumerateArray()) CheckTree(child);
        else if (element.ValueKind == JsonValueKind.String)
            _ = element.GetString(); // Validate even ignored diagnostic strings.
    }
}

/// <summary>
/// Numeric or explicitly vocabulary-pinned symbolic observations, never owner operations.
/// The caller authorizes the dataset and compares against an independent source.
/// Calls and disposal are serialized by the owner. Parent identity is retained;
/// same-UID peer checks are not protection against a malicious same-UID owner.
/// Native ask handling may update its ordinary transient dialog/telemetry state.
/// </summary>
internal sealed class LearningAskClient : IDisposable
{
    private readonly LearningFiles files;
    private readonly int seconds;
    private readonly ILearningClock clock;
    private bool disposed;
    public LearningAskClient(string absoluteAskSocket, int seconds, ILearningClock? clock = null)
    {
        if (string.IsNullOrEmpty(absoluteAskSocket) || !Path.IsPathFullyQualified(absoluteAskSocket)
            || Path.GetFileName(absoluteAskSocket) != "ask.sock" || absoluteAskSocket.Contains('\0') || seconds is < 1 or > 120)
            throw new ArgumentException("learning_ask_arguments");
        try
        {
            if (new UTF8Encoding(false, true).GetByteCount(absoluteAskSocket) > 107) throw new ArgumentException();
        }
        catch (ArgumentException) { throw new ArgumentException("learning_ask_arguments"); }
        this.clock = clock ?? new LearningClock(); this.seconds = seconds;
        try { files = LearningFiles.Open(Path.GetDirectoryName(absoluteAskSocket)!); }
        catch (Exception exception) when (exception is ArgumentException or InvalidOperationException or IOException or UnauthorizedAccessException)
        { throw new InvalidOperationException("learning_ask_socket_refused"); }
    }

    public async Task<LearningAskResult> AskAsync(LearningDataset authorized, byte key, CancellationToken cancellation)
    {
        RequireDataset(authorized);
        return await AskCoreAsync("data " + authorized.Id + " " + key.ToString(CultureInfo.InvariantCulture), false, cancellation).ConfigureAwait(false);
    }

    public async Task<LearningAskResult> AskSymbolAsync(LearningDataset authorized, string token, CancellationToken cancellation)
    {
        RequireDataset(authorized);
        if (!LearningPolicy.IsHash(authorized.SymbolVocabularySha256)) throw new ArgumentException("learning_ask_dataset");
        if (!LocalSymbolReference.IsKey(token)) throw new ArgumentException("learning_ask_symbol");
        return await AskCoreAsync("symbol " + authorized.Id + " " + token, true, cancellation).ConfigureAwait(false);
    }

    private static void RequireDataset(LearningDataset authorized)
    {
        if (authorized is null || !LearningPolicy.IsId(authorized.Id)
            || authorized.Authority is not ("verified_tool" or "user_correction"))
            throw new ArgumentException("learning_ask_dataset");
    }

    private async Task<LearningAskResult> AskCoreAsync(string query, bool symbolic, CancellationToken cancellation)
    {
        cancellation.ThrowIfCancellationRequested();
        Socket? socket = null;
        using var stop = CancellationTokenSource.CreateLinkedTokenSource(cancellation);
        Task<byte[]>? exchange = null;
        try
        {
            if (disposed) throw new InvalidOperationException();
            socket = new Socket(AddressFamily.Unix, SocketType.Stream, ProtocolType.Unspecified);
            var start = clock.Now;
            if (string.IsNullOrEmpty(start.Boot) || start.Nanoseconds < 0 || start.Nanoseconds > long.MaxValue - seconds * 1_000_000_000L)
                throw new InvalidOperationException();
            var last = start.Nanoseconds;
            void Deadline()
            {
                cancellation.ThrowIfCancellationRequested();
                var now = clock.Now;
                if (now.Boot != start.Boot || now.Nanoseconds < last || now.Nanoseconds - start.Nanoseconds >= seconds * 1_000_000_000L)
                    throw new InvalidOperationException();
                last = now.Nanoseconds;
            }
            files.AssertPathIdentity();
            using var parent = Handle(open(files.FullPath, DirectoryFlag | NoFollow | CloseExec, 0));
            var directory = Inspect(parent);
            if ((directory.Mode & 0xf000) != 0x4000 || (directory.Mode & 0xfff) != 0x1c0 || directory.Owner != geteuid())
                throw new InvalidOperationException();
            files.AssertPathIdentity();
            var before = RequireSocket(InspectAt(parent, "ask.sock", false)!.Value);
            var path = $"/proc/self/fd/{parent.DangerousGetHandle().ToInt64()}/ask.sock";
            // Both entry points validate every query character before transport.
            var request = Encoding.UTF8.GetBytes("{\"op\":\"ask\",\"q\":\"" + query + "\"}\n");
            Deadline();
            exchange = Exchange(socket, path, request, () =>
            {
                RequirePeer(socket.SafeHandle, before.Owner);
                if (!before.Equals(RequireSocket(InspectAt(parent, "ask.sock", false)!.Value))) throw new InvalidOperationException();
                files.AssertPathIdentity();
                Deadline(); // No request bytes before peer/path/deadline checks.
            }, stop.Token);
            while (!exchange.IsCompleted)
            {
                Deadline();
                await Task.WhenAny(exchange, Task.Delay(20, cancellation)).ConfigureAwait(false);
            }
            var bytes = await exchange.ConfigureAwait(false);
            Deadline();
            if (!before.Equals(RequireSocket(InspectAt(parent, "ask.sock", false)!.Value))) throw new InvalidOperationException();
            files.AssertPathIdentity();
            var result = symbolic ? LearningAskResult.ParseSymbol(bytes) : LearningAskResult.Parse(bytes);
            Deadline();
            return result;
        }
        catch (OperationCanceledException) when (cancellation.IsCancellationRequested) { throw; }
        catch (Exception exception) when (exception is InvalidOperationException or ArgumentException or IOException or SocketException or UnauthorizedAccessException or OverflowException)
        { throw new InvalidOperationException("learning_ask_observation_unknown"); }
        finally
        {
            stop.Cancel(); socket?.Dispose();
            if (exchange is not null)
            {
                // Cancellation/disposal aborts pending Socket operations; retain
                // no unobserved transport task and never wait on remote EOF here.
                try { await exchange.ConfigureAwait(false); }
                catch (Exception exception) when (exception is OperationCanceledException or SocketException or ObjectDisposedException or InvalidOperationException or IOException) { }
            }
        }
    }

    private static Stat RequireSocket(Stat value) => (value.Mode & 0xf000) == 0xc000
        && (value.Mode & 0xfff) == 0x180 && value.Owner == geteuid() && value.Links == 1
        ? value : throw new InvalidOperationException();

    [StructLayout(LayoutKind.Sequential)]
    private struct Peer { public int Pid; public uint Uid; public uint Gid; }
    // Linux x86-64 UAPI: SOL_SOCKET=1, SO_PEERCRED=17, struct ucred=12 bytes.
    [DllImport("libc", SetLastError = true)]
    private static extern int getsockopt(SafeSocketHandle socket, int level, int option, out Peer peer, ref uint length);
    private static void RequirePeer(SafeSocketHandle socket, uint owner)
    {
        uint length = 12;
        if (getsockopt(socket, 1, 17, out var peer, ref length) != 0 || length != 12 || peer.Pid <= 0 || peer.Uid != owner)
            throw new InvalidOperationException();
    }

    private static async Task<byte[]> Exchange(Socket socket, string path, byte[] request, Action connected, CancellationToken stop)
    {
        await socket.ConnectAsync(new UnixDomainSocketEndPoint(path), stop).ConfigureAwait(false);
        connected();
        var sent = 0;
        while (sent < request.Length)
        {
            var count = await socket.SendAsync(request.AsMemory(sent), SocketFlags.None, stop).ConfigureAwait(false);
            if (count == 0) throw new InvalidOperationException();
            sent += count;
        }
        socket.Shutdown(SocketShutdown.Send);
        var buffer = new byte[16385]; var used = 0;
        while (true)
        {
            var count = await socket.ReceiveAsync(buffer.AsMemory(used), SocketFlags.None, stop).ConfigureAwait(false);
            if (count == 0) return buffer.AsSpan(0, used).ToArray();
            used += count;
            if (used > 16384) throw new InvalidOperationException();
            var newline = buffer.AsSpan(0, used).IndexOf((byte)'\n');
            if (newline >= 0 && newline != used - 1) throw new InvalidOperationException();
        }
    }
    public void Dispose() { disposed = true; files.Dispose(); }
}
