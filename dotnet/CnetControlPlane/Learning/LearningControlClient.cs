using System.Globalization;
using System.Text;

namespace CnetControlPlane.Learning;

/// <summary>
/// Executes only the attested native owner CLI. It does not authorize a job,
/// retry an operation, resolve its ledger intent, or turn STATUS into proof of
/// activation. The caller owns runtime/ledger identity pins and reconciliation.
/// Native CLI code retains the private socket ancestry and peer-identity checks.
/// </summary>
internal sealed class LearningControlClient
{
    private readonly LearningRuntime runtime;
    private readonly string socket;
    private readonly int seconds;
    private readonly ILearningClock? clock;
    public LearningControlClient(LearningRuntime runtime, string absoluteSocket, int seconds, ILearningClock? clock = null)
    {
        if (runtime is null || string.IsNullOrEmpty(absoluteSocket) || !Path.IsPathFullyQualified(absoluteSocket)
            || absoluteSocket.Contains('\0') || seconds is < 1 or > 60)
            throw new ArgumentException("learning_control_arguments");
        try
        {
            if (new UTF8Encoding(false, true).GetByteCount(absoluteSocket) > 107)
                throw new ArgumentException("learning_control_arguments");
        }
        catch (EncoderFallbackException) { throw new ArgumentException("learning_control_arguments"); }
        this.runtime = runtime; socket = absoluteSocket; this.seconds = seconds; this.clock = clock;
    }

    public Task<ControlStatus> StatusAsync(CancellationToken cancellation) => Execute(NativeControlProtocol.FormatStatus(), cancellation);
    public Task<ControlStatus> SendAsync(LearningIntent intent, CancellationToken cancellation)
    {
        ValidateIntent(intent); // No runtime verification or process launch before validation.
        return Execute(intent.Request, cancellation);
    }

    private static void ValidateStatus(ControlStatus status)
    {
        if (status is null || !status.Ok || !status.Durable || status.Reason != ControlReason.Ok || status.Active is not { Length: 64 }
            || status.Rollback is { Length: not 64 } || status.Staged is { Length: not 64 })
            throw new ArgumentException("learning_control_intent");
        var frame = $"OK revision={status.Revision.ToString(CultureInfo.InvariantCulture)} active={status.Active} "
            + $"rollback={status.Rollback ?? "-"} staged={status.Staged ?? "-"} durable=1 reason=ok\n";
        if (NativeControlProtocol.Parse(Encoding.ASCII.GetBytes(frame)) != status)
            throw new ArgumentException("learning_control_intent");
    }

    private static void ValidateIntent(LearningIntent intent)
    {
        try
        {
            if (intent is null || intent.Id < 1 || intent.JobId < 1 || intent.Request is not { Length: >= 1 and <= 159 }
                || intent.Request[^1] != '\n' || intent.Request.AsSpan(0, intent.Request.Length - 1).ContainsAnyExceptInRange(' ', '~'))
                throw new ArgumentException("learning_control_intent");
            ValidateStatus(intent.Before); ValidateStatus(intent.Expected);
            var fields = intent.Request[..^1].Split(' ');
            var before = intent.Before;
            string canonical;
            ControlStatus expected;
            switch (intent.Kind)
            {
                case "stage" when fields.Length == 4 && before.Staged is null && intent.Expected.Staged is not null:
                    canonical = NativeControlProtocol.FormatStage(before.Revision, fields[3]);
                    expected = before with { Staged = intent.Expected.Staged };
                    break;
                case "activate" when fields.Length == 4 && before.Staged is not null:
                    canonical = NativeControlProtocol.FormatActivate(before.Revision, fields[2], before.Staged);
                    expected = before with { Revision = checked(before.Revision + 1), Active = before.Staged, Rollback = before.Active, Staged = null };
                    break;
                case "rollback" when fields.Length == 3 && before.Rollback is not null && before.Staged is null:
                    canonical = NativeControlProtocol.FormatRollback(before.Revision, fields[2]);
                    expected = before with { Revision = checked(before.Revision + 1), Active = before.Rollback, Rollback = before.Active };
                    break;
                case "discard" when fields.Length == 3 && before.Staged is not null:
                    canonical = NativeControlProtocol.FormatDiscard(before.Revision, before.Staged);
                    expected = before with { Staged = null };
                    break;
                default: throw new ArgumentException("learning_control_intent");
            }
            if (intent.Request != canonical || intent.Expected != expected)
                throw new ArgumentException("learning_control_intent");
        }
        catch (Exception exception) when (exception is ArgumentException or OverflowException)
        {
            throw new ArgumentException("learning_control_intent");
        }
    }

    private async Task<ControlStatus> Execute(string request, CancellationToken cancellation)
    {
        cancellation.ThrowIfCancellationRequested();
        try
        {
            var executable = runtime.PathFor(LearningNativeCommand.Control);
            var arguments = new List<string> { socket };
            arguments.AddRange(request[..^1].Split(' '));
            var child = await LearningChild.RunAsync(executable, arguments, Path.GetDirectoryName(executable)!,
                new Dictionary<string, string>(), seconds, 398, cancellation, clock).ConfigureAwait(false);
            if (!child.Stderr.IsEmpty || child.ExitCode is not (0 or 1))
                throw new InvalidOperationException("learning_control_outcome_unknown");
            var reply = NativeControlProtocol.Parse(child.Stdout.ToArray());
            if (reply.Ok != (child.ExitCode == 0)) throw new InvalidOperationException("learning_control_outcome_unknown");
            return reply; // Durable=false and valid ERR remain explicit, unchanged observations.
        }
        catch (OperationCanceledException) { throw; }
        catch (Exception exception) when (exception is InvalidOperationException or ArgumentException or IOException or UnauthorizedAccessException)
        {
            // Never expose native diagnostic text or claim an operation did not
            // happen: bytes might already have reached the owner control socket.
            throw new InvalidOperationException("learning_control_outcome_unknown");
        }
    }
}
