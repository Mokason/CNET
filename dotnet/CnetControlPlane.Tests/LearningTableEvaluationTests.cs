using System.Globalization;
using System.Reflection;
using System.Security.Cryptography;
using System.Text;
using CnetControlPlane.Learning;
using Xunit;

namespace CnetControlPlane.Tests;

public sealed class LearningTableEvaluationTests
{
    private static readonly string Snapshot = new('a', 64);
    private const string Source = "CNET_LOCAL_TABLE_V1\ndataset calibration\nauthority verified_tool\ninput_bits 8\noutput_bits 16\nrows 3\n0\t0\n7\t65535\n255\t42\n";
    private static LocalTableReference Reference() => LocalTableReference.Parse(Encoding.ASCII.GetBytes(Source), new("calibration", "verified_tool"));
    private static string Report()
    {
        var result = new StringBuilder("CNET_TABLE_SNAPSHOT_EVAL_V1\nsnapshot_sha256 " + Snapshot +
            "\ndataset calibration\nsource_sha256 " + Reference().SourceSha256 + "\nresults 256\n");
        for (var key = 0; key < 256; key++)
        {
            var answer = key switch { 0 => "1\t0", 7 => "1\t65535", 255 => "1\t42", _ => "0\t-" };
            result.Append(key.ToString(CultureInfo.InvariantCulture)).Append('\t').Append(answer).Append('\n');
        }
        return result.Append("end\n").ToString();
    }

    private static LearningTableEvaluation Parse(string report) =>
        LearningTableEvaluation.Parse(Encoding.ASCII.GetBytes(report), Reference(), Snapshot);

    [Fact]
    public void ReceiptBindsEveryAnswerAndExactSourceSnapshotAndOutputBytes()
    {
        var bytes = Encoding.ASCII.GetBytes(Report());
        var receipt = LearningTableEvaluation.Parse(bytes, Reference(), Snapshot);
        Assert.Equal("calibration", receipt.Dataset);
        Assert.Equal(Reference().SourceSha256, receipt.SourceSha256);
        Assert.Equal(Snapshot, receipt.CandidateSha256);
        Assert.Equal(Convert.ToHexString(SHA256.HashData(bytes)).ToLowerInvariant(), receipt.ReceiptSha256);
    }

    [Theory]
    [InlineData("0\t1\t0\n", "0\t0\t-\n")]
    [InlineData("1\t0\t-\n", "1\t1\t0\n")]
    public void ACorrectZeroAnswerAndAbstentionAreNotInterchangeable(string correct, string wrong) =>
        Assert.Throws<ArgumentException>(() => Parse(Report().Replace("\n" + correct, "\n" + wrong)));

    [Fact]
    public void ReceiptRetainsNoCallerMutableInputAndCannotBeConstructedOrChanged()
    {
        var bytes = Encoding.ASCII.GetBytes(Report());
        var source = Encoding.ASCII.GetBytes(Source);
        var reference = LocalTableReference.Parse(source, new("calibration", "verified_tool"));
        var receipt = LearningTableEvaluation.Parse(bytes, reference, Snapshot);
        var receiptHash = receipt.ReceiptSha256;
        var sourceHash = receipt.SourceSha256;
        Array.Fill(bytes, (byte)0);
        Array.Fill(source, (byte)0);
        Assert.Equal(receiptHash, receipt.ReceiptSha256);
        Assert.Equal(sourceHash, receipt.SourceSha256);
        Assert.Empty(typeof(LearningTableEvaluation).GetConstructors(BindingFlags.Public | BindingFlags.Instance));
        Assert.All(typeof(LearningTableEvaluation).GetConstructors(BindingFlags.NonPublic | BindingFlags.Instance), constructor => Assert.True(constructor.IsPrivate));
        Assert.All(typeof(LearningTableEvaluation).GetProperties(), property => Assert.Null(property.SetMethod));
        Assert.True(typeof(LearningTableEvaluation).IsSealed);
    }

    [Theory]
    [InlineData("CNET_TABLE_SNAPSHOT_EVAL_V1", "CNET_TABLE_SNAPSHOT_EVAL_V2")]
    [InlineData("dataset calibration", "dataset other")]
    [InlineData("results 256", "results 255")]
    [InlineData("results 256", "results 0256")]
    [InlineData("results 256", "results +256")]
    [InlineData("results 256", "results\t256")]
    [InlineData("results 256", "results  256")]
    [InlineData("7\t1\t65535\n", "7\t1\t65536\n")]
    [InlineData("7\t1\t65535\n", "7\t1\t65534\n")]
    [InlineData("7\t1\t65535\n", "07\t1\t65535\n")]
    [InlineData("7\t1\t65535\n", "7\t01\t65535\n")]
    [InlineData("7\t1\t65535\n", "7\t1\t065535\n")]
    [InlineData("7\t1\t65535\n", "7\t1\t+65535\n")]
    [InlineData("7\t1\t65535\n", "7\t1\t-1\n")]
    [InlineData("7\t1\t65535\n", "7\t1\t6.5535e4\n")]
    [InlineData("7\t1\t65535\n", "7\t1\t4294967296\n")]
    [InlineData("7\t1\t65535\n", "7\t1\t65535 \n")]
    [InlineData("7\t1\t65535\n", "7\t1\t65535\textra\n")]
    [InlineData("7\t1\t65535\n", "7 1 65535\n")]
    [InlineData("1\t0\t-\n", "1\t0\t0\n")]
    [InlineData("1\t0\t-\n", "1\t1\t-\n")]
    [InlineData("1\t0\t-\n", "1\t2\t-\n")]
    [InlineData("end\n", "END\n")]
    [InlineData("end\n", "end \n")]
    public void UnknownOrNoncanonicalFieldsAndWrongAnswersRefuse(string before, string after) =>
        Assert.Throws<ArgumentException>(() => Parse(Report().Replace(before, after)));

    [Fact]
    public void SourceAndSnapshotIdentityMustMatchInFull()
    {
        var report = Report();
        Assert.Throws<ArgumentException>(() => Parse(report.Replace(Snapshot, new string('b', 64))));
        Assert.Throws<ArgumentException>(() => Parse(report.Replace(Snapshot, Snapshot.ToUpperInvariant())));
        Assert.Throws<ArgumentException>(() => Parse(report.Replace(Reference().SourceSha256, new string('c', 64))));
        Assert.Throws<ArgumentException>(() => Parse(report.Replace(Reference().SourceSha256, Reference().SourceSha256[..63])));
        var changedSource = LocalTableReference.Parse(Encoding.ASCII.GetBytes(Source.Replace("255\t42", "255\t43")), new("calibration", "verified_tool"));
        Assert.Throws<ArgumentException>(() => LearningTableEvaluation.Parse(Encoding.ASCII.GetBytes(report), changedSource, Snapshot));
    }

    [Fact]
    public void ExactRowsInOrderAndNoTrailingOutputAreRequired()
    {
        var report = Report();
        foreach (var malformed in new[]
        {
            report.Replace("1\t0\t-\n2\t0\t-\n", "2\t0\t-\n1\t0\t-\n"),
            report.Replace("\n2\t0\t-\n", "\n1\t0\t-\n"),
            report.Replace("\n255\t1\t42\n", "\n"),
            report.Replace("end\n", "256\t0\t-\nend\n"),
            report.Replace("results 256\n", "unexpected 1\nresults 256\n"),
            report.Replace("snapshot_sha256 " + Snapshot + "\ndataset calibration\n", "dataset calibration\nsnapshot_sha256 " + Snapshot + "\n"),
            report.TrimEnd('\n'), report + "\n", report + "PASS\n", report.Replace("\n", "\r\n"), report + "\0"
        }) Assert.Throws<ArgumentException>(() => Parse(malformed));
    }

    [Fact]
    public void BoundsAreCheckedBeforeCloneOrAsciiDecode()
    {
        foreach (var bytes in new byte[][] { [], new byte[8193], null! })
            Assert.Equal("learning_table_evaluation_size", Assert.Throws<ArgumentException>(() =>
                LearningTableEvaluation.Parse(bytes, Reference(), Snapshot)).Message);
        var nonAscii = Encoding.ASCII.GetBytes(Report());
        nonAscii[0] = 255;
        Assert.Equal("learning_table_evaluation_ascii", Assert.Throws<ArgumentException>(() =>
            LearningTableEvaluation.Parse(nonAscii, Reference(), Snapshot)).Message);
    }

    [Theory]
    [InlineData("")]
    [InlineData("aaa")]
    [InlineData(null)]
    public void ExpectedSnapshotMustBeAFullLowercaseHash(string? digest) =>
        Assert.Throws<ArgumentException>(() => LearningTableEvaluation.Parse(Encoding.ASCII.GetBytes(Report()), Reference(), digest!));

    [Fact]
    public void EveryCoveredKeyIncludingBothNumericBoundariesCanPass()
    {
        var source = "CNET_LOCAL_TABLE_V1\ndataset calibration\nauthority verified_tool\ninput_bits 8\noutput_bits 16\nrows 256\n";
        source += string.Concat(Enumerable.Range(0, 256).Select(key => FormattableString.Invariant($"{key}\t{key * 257}\n")));
        var reference = LocalTableReference.Parse(Encoding.ASCII.GetBytes(source), new("calibration", "verified_tool"));
        var report = "CNET_TABLE_SNAPSHOT_EVAL_V1\nsnapshot_sha256 " + Snapshot + "\ndataset calibration\nsource_sha256 " + reference.SourceSha256 + "\nresults 256\n";
        report += string.Concat(Enumerable.Range(0, 256).Select(key => FormattableString.Invariant($"{key}\t1\t{key * 257}\n"))) + "end\n";
        Assert.Equal(reference.SourceSha256, LearningTableEvaluation.Parse(Encoding.ASCII.GetBytes(report), reference, Snapshot).SourceSha256);
    }
}
