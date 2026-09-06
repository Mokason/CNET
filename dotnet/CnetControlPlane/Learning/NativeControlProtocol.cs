using System.Globalization;
using System.Text;

namespace CnetControlPlane.Learning;

internal enum ControlReason { Ok, Refused, DurabilityUncertain }
internal sealed record ControlStatus(bool Ok, ulong Revision, string? Active,
    string? Rollback, string? Staged, bool Durable, ControlReason Reason);

internal static class NativeControlProtocol
{
    private static string Identifier(string text) => text is { Length: >= 1 and <= 63 }
        && text.All(c => c is >= 'a' and <= 'z' or >= 'A' and <= 'Z' or >= '0' and <= '9' or '_' or '-')
        ? text : throw new ArgumentException("native_control_identifier");

    private static string Digest(string text) => text is { Length: 64 }
        && text.All(c => c is >= 'a' and <= 'f' or >= '0' and <= '9')
        ? text : throw new ArgumentException("native_control_digest");

    private static string Revision(ulong revision, bool increments = false)
    {
        if (revision == 0 || increments && revision == ulong.MaxValue)
            throw new ArgumentException("native_control_revision");
        return revision.ToString(CultureInfo.InvariantCulture);
    }

    private static string Field(string token, string prefix) => token.StartsWith(prefix, StringComparison.Ordinal)
        ? token[prefix.Length..] : throw new ArgumentException("native_control_fields");
    private static string? OptionalDigest(string value) => value == "-" ? null : Digest(value);

    /// <summary>
    /// Parses the complete EOF-delimited native CLI output, including one final
    /// LF. Transport must bound reads too. Ok does not imply Durable; even a
    /// successful STATUS can expose a frozen, uncertain native publication.
    /// </summary>
    internal static ControlStatus Parse(byte[] bytes)
    {
        // tools/cnet_capsulectl.c refuses once 399 bytes have been received.
        if (bytes is null || bytes.Length is < 1 or > 398)
            throw new ArgumentException("native_control_size");
        var frame = (byte[])bytes.Clone();
        if (frame[^1] != '\n' || frame.AsSpan(0, frame.Length - 1).ContainsAnyExceptInRange((byte)32, (byte)126))
            throw new ArgumentException("native_control_ascii_frame");
        var fields = Encoding.ASCII.GetString(frame, 0, frame.Length - 1).Split(' ');
        if (fields.Length != 7 || fields[0] is not ("OK" or "ERR"))
            throw new ArgumentException("native_control_fields");
        var revisionText = Field(fields[1], "revision=");
        if (revisionText.Length is < 1 or > 20 || revisionText[0] == '0'
            || !revisionText.All(c => c is >= '0' and <= '9')
            || !ulong.TryParse(revisionText, NumberStyles.None, CultureInfo.InvariantCulture, out var revision))
            throw new ArgumentException("native_control_revision");
        var active = OptionalDigest(Field(fields[2], "active="));
        var rollback = OptionalDigest(Field(fields[3], "rollback="));
        var staged = OptionalDigest(Field(fields[4], "staged="));
        var durable = Field(fields[5], "durable=") switch
        {
            "0" => false,
            "1" => true,
            _ => throw new ArgumentException("native_control_durable"),
        };
        var reason = Field(fields[6], "reason=") switch
        {
            "ok" => ControlReason.Ok,
            "refused" => ControlReason.Refused,
            "durability_uncertain" => ControlReason.DurabilityUncertain,
            _ => throw new ArgumentException("native_control_reason"),
        };
        var ok = fields[0] == "OK";
        if (ok != (reason == ControlReason.Ok) || reason == ControlReason.DurabilityUncertain && durable)
            throw new ArgumentException("native_control_inconsistent_status");
        return new ControlStatus(ok, revision, active, rollback, staged, durable, reason);
    }

    // Formats only approved operation shapes; no arbitrary command entry point.
    internal static string FormatStatus() => "STATUS\n";
    internal static string FormatStage(ulong revision, string set) =>
        $"STAGE {Revision(revision)} 1 {Identifier(set)}\n";
    internal static string FormatActivate(ulong revision, string token, string digest) =>
        $"ACTIVATE {Revision(revision, true)} {Identifier(token)} {Digest(digest)}\n";
    internal static string FormatRollback(ulong revision, string token) =>
        $"ROLLBACK {Revision(revision, true)} {Identifier(token)}\n";
    internal static string FormatDiscard(ulong revision, string digest) =>
        $"DISCARD {Revision(revision)} {Digest(digest)}\n";
}
