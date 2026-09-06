using System.Text;
using CnetControlPlane.Learning;
using Xunit;

namespace CnetControlPlane.Tests;

public sealed class NativeControlProtocolTests
{
    private static readonly string Digest = new('a', 64);
    private static string Reply(string disposition = "OK", string revision = "1",
        string durable = "1", string reason = "ok") =>
        $"{disposition} revision={revision} active={Digest} rollback=- staged=- durable={durable} reason={reason}\n";
    private static ControlStatus Parse(string text) => NativeControlProtocol.Parse(Encoding.UTF8.GetBytes(text));

    [Fact]
    public void SuccessfulStatusHasExactTypedFields()
    {
        var status = Parse(Reply());
        Assert.Equal(new ControlStatus(true, 1, Digest, null, null, true, ControlReason.Ok), status);
        Assert.Equal(ulong.MaxValue, Parse(Reply(revision: "18446744073709551615")).Revision);
    }

    [Theory]
    [InlineData("OK", "0", "ok", true, false, ControlReason.Ok)]
    [InlineData("ERR", "1", "refused", false, true, ControlReason.Refused)]
    [InlineData("ERR", "0", "refused", false, false, ControlReason.Refused)]
    [InlineData("ERR", "0", "durability_uncertain", false, false, ControlReason.DurabilityUncertain)]
    internal void SuccessAndDurabilityAreIndependent(string disposition, string durable,
        string reason, bool ok, bool expectedDurable, ControlReason expectedReason)
    {
        var status = Parse(Reply(disposition, durable: durable, reason: reason));
        Assert.Equal(ok, status.Ok);
        Assert.Equal(expectedDurable, status.Durable);
        Assert.Equal(expectedReason, status.Reason);
    }

    [Fact]
    public void OnlyExplicitFixedCommandShapesAreFormatted()
    {
        Assert.Equal("STATUS\n", NativeControlProtocol.FormatStatus());
        Assert.Equal("STAGE 7 1 reviewed-set\n", NativeControlProtocol.FormatStage(7, "reviewed-set"));
        Assert.Equal($"ACTIVATE 7 operation_1 {Digest}\n", NativeControlProtocol.FormatActivate(7, "operation_1", Digest));
        Assert.Equal("ROLLBACK 8 undo_1\n", NativeControlProtocol.FormatRollback(8, "undo_1"));
        Assert.Equal($"DISCARD 7 {Digest}\n", NativeControlProtocol.FormatDiscard(7, Digest));
    }

    [Fact]
    public void SizeIsRejectedBeforeAsciiDecoding()
    {
        Assert.Equal("native_control_size", Assert.Throws<ArgumentException>(
            () => NativeControlProtocol.Parse(new byte[399])).Message);
        Assert.Throws<ArgumentException>(() => NativeControlProtocol.Parse([]));
        Assert.Throws<ArgumentException>(() => NativeControlProtocol.Parse(null!));
    }

    [Theory]
    [InlineData("OK", "1", "refused")]
    [InlineData("OK", "0", "refused")]
    [InlineData("OK", "0", "durability_uncertain")]
    [InlineData("ERR", "1", "durability_uncertain")]
    [InlineData("ERR", "0", "ok")]
    [InlineData("ERR", "1", "ok")]
    public void InconsistentFieldsCannotTurnUncertaintyIntoSuccess(string disposition, string durable, string reason) =>
        Assert.Throws<ArgumentException>(() => Parse(Reply(disposition, durable: durable, reason: reason)));

    [Theory]
    [InlineData("0")]
    [InlineData("01")]
    [InlineData("")]
    [InlineData("+1")]
    [InlineData("-1")]
    [InlineData("1.0")]
    [InlineData("1e2")]
    [InlineData("18446744073709551616")]
    [InlineData("999999999999999999999999999999999999999")]
    public void StatusRevisionsAreCanonicalUnsigned64(string revision) =>
        Assert.Throws<ArgumentException>(() => Parse(Reply(revision: revision)));

    [Fact]
    public void FramingIsSingleExactAsciiLineWithNoUnknownOrReorderedFields()
    {
        var valid = Reply();
        var malformed = new[]
        {
            valid.TrimEnd('\n'), valid + "\n", valid + "ignored\n", valid.Replace("\n", "\r\n"),
            valid.Replace("active=", "active=\0"), valid.Replace("OK", "ＯＫ"),
            valid.Replace("OK", "ok"), valid.Replace("OK", "YES"),
            valid.Replace("OK ", " OK "), valid.Replace("OK ", "OK  "),
            valid.Replace("OK ", "OK\t"), valid.Replace("reason=ok", "reason=ok "),
            valid.Replace("staged=- ", ""), valid.Replace("staged=-", "staged=- unknown=1"),
            valid.Replace("rollback=- staged=-", "staged=- rollback=-"),
            valid.Replace("staged=-", "rollback=-"), valid.Replace("revision=", "Revision="),
            valid.Replace("durable=1", "durable=true"), valid.Replace("durable=1", "durable=2"),
            valid.Replace("durable=1", "durable=01"), valid.Replace("reason=ok", "reason=success"),
        };
        foreach (var source in malformed) Assert.Throws<ArgumentException>(() => Parse(source));
        var invalidByte = Encoding.ASCII.GetBytes(valid);
        invalidByte[0] = 0xff;
        Assert.Throws<ArgumentException>(() => NativeControlProtocol.Parse(invalidByte));
    }

    [Fact]
    public void OptionalIdentitiesAreNullOrFullLowercaseHex()
    {
        var all = Reply(revision: "18446744073709551615").Replace("rollback=- staged=-", $"rollback={Digest} staged={Digest}");
        Assert.Equal(Digest, Parse(all).Rollback);
        Assert.Equal(Digest, Parse(all).Staged);
        Assert.Null(Parse(Reply().Replace($"active={Digest}", "active=-")).Active);
        foreach (var bad in new[] { "", Digest[..63], Digest + "a", Digest.ToUpperInvariant(), new string('g', 64), "--" })
            foreach (var field in new[] { "active", "rollback", "staged" })
            {
                var original = field == "active" ? Digest : "-";
                Assert.Throws<ArgumentException>(() => Parse(Reply().Replace($"{field}={original}", $"{field}={bad}")));
            }
    }

    [Fact]
    public void FormatterPreservesUint64AndIncrementBoundaries()
    {
        const string maximum = "18446744073709551615";
        Assert.Equal($"STAGE {maximum} 1 set\n", NativeControlProtocol.FormatStage(ulong.MaxValue, "set"));
        Assert.Equal($"DISCARD {maximum} {Digest}\n", NativeControlProtocol.FormatDiscard(ulong.MaxValue, Digest));
        Assert.Equal($"ACTIVATE 18446744073709551614 token {Digest}\n",
            NativeControlProtocol.FormatActivate(ulong.MaxValue - 1, "token", Digest));
        Assert.Throws<ArgumentException>(() => NativeControlProtocol.FormatActivate(ulong.MaxValue, "token", Digest));
        Assert.Throws<ArgumentException>(() => NativeControlProtocol.FormatRollback(ulong.MaxValue, "token"));
        Assert.Throws<ArgumentException>(() => NativeControlProtocol.FormatStage(0, "set"));
        Assert.Throws<ArgumentException>(() => NativeControlProtocol.FormatActivate(0, "token", Digest));
        Assert.Throws<ArgumentException>(() => NativeControlProtocol.FormatRollback(0, "token"));
        Assert.Throws<ArgumentException>(() => NativeControlProtocol.FormatDiscard(0, Digest));
    }

    [Theory]
    [InlineData("")]
    [InlineData("../set")]
    [InlineData("set.name")]
    [InlineData("two words")]
    [InlineData("x\nSTATUS")]
    [InlineData("x\0")]
    [InlineData("Å")]
    public void FormatterCannotInjectCommandsOrPaths(string identifier)
    {
        Assert.Throws<ArgumentException>(() => NativeControlProtocol.FormatStage(1, identifier));
        Assert.Throws<ArgumentException>(() => NativeControlProtocol.FormatActivate(1, identifier, Digest));
        Assert.Throws<ArgumentException>(() => NativeControlProtocol.FormatRollback(1, identifier));
    }

    [Fact]
    public void FormatterRejectsInvalidDigestsAndIdentifierSizes()
    {
        foreach (var invalid in new[] { "", "-", Digest.ToUpperInvariant(), Digest[..63], Digest + "a", "x\nSTATUS" })
        {
            Assert.Throws<ArgumentException>(() => NativeControlProtocol.FormatActivate(1, "token", invalid));
            Assert.Throws<ArgumentException>(() => NativeControlProtocol.FormatDiscard(1, invalid));
        }
        var largest = new string('x', 63);
        Assert.Equal($"STAGE 1 1 {largest}\n", NativeControlProtocol.FormatStage(1, largest));
        Assert.Equal($"ROLLBACK 1 _Aa09-\n", NativeControlProtocol.FormatRollback(1, "_Aa09-"));
        Assert.Throws<ArgumentException>(() => NativeControlProtocol.FormatStage(1, largest + "x"));
        Assert.Throws<ArgumentException>(() => NativeControlProtocol.FormatActivate(1, largest + "x", Digest));
        Assert.Throws<ArgumentException>(() => NativeControlProtocol.FormatRollback(1, largest + "x"));
        Assert.Throws<ArgumentException>(() => NativeControlProtocol.FormatStage(1, null!));
        Assert.Throws<ArgumentException>(() => NativeControlProtocol.FormatActivate(1, "token", null!));
    }
}
