using System.Text;
using CnetControlPlane.Learning;
using Xunit;

namespace CnetControlPlane.Tests;

public sealed class LocalTableReferenceTests
{
    private static readonly LearningDataset Authorized = new("stock_levels", "user_correction");
    private const string Source = "CNET_LOCAL_TABLE_V1\ndataset stock_levels\nauthority user_correction\ninput_bits 8\noutput_bits 16\nrows 3\n0\t120\n7\t42\n19\t0\n";

    [Fact]
    public void EveryKeyHasAnExplicitAnswerOrAbstention()
    {
        var table = LocalTableReference.Parse(Encoding.ASCII.GetBytes(Source), Authorized);
        Assert.Equal("stock_levels", table.Dataset);
        Assert.Equal("user_correction", table.Authority);
        Assert.Equal("fa7d597f3dd9467eb506e294cd0701d498cd3d769eb673afec91b874d3a56375", table.SourceSha256);
        Assert.Equal(3, table.Values.Count);
        Assert.Equal(256, table.ExpectedByKey.Count);
        for (var key = 0; key < 256; key++)
        {
            ushort? expected = key switch { 0 => 120, 7 => 42, 19 => 0, _ => null };
            Assert.Equal(expected, table.ExpectedFor((byte)key));
            Assert.Equal(expected, table.ExpectedByKey[key]);
        }
    }

    [Theory]
    [InlineData("rows 3", "rows 03")]
    [InlineData("rows 3", "rows 0")]
    [InlineData("rows 3", "rows 4")]
    [InlineData("rows 3", "rows 257")]
    [InlineData("rows 3", "rows +3")]
    [InlineData("rows 3", "rows 3 ")]
    [InlineData("rows 3", "rows\t3")]
    [InlineData("0\t120", "00\t120")]
    [InlineData("0\t120", "0\t0120")]
    [InlineData("0\t120", "0 120")]
    [InlineData("0\t120", " 0\t120")]
    [InlineData("0\t120", "0\t120 ")]
    [InlineData("0\t120", "0\t120\textra")]
    [InlineData("0\t120", "0\t120\t")]
    [InlineData("0\t120", "0\t1e2")]
    [InlineData("7\t42", "256\t42")]
    [InlineData("7\t42", "7\t65536")]
    [InlineData("7\t42", "7\t-1")]
    [InlineData("7\t42", "7\t4294967296")]
    [InlineData("19\t0", "7\t0")]
    [InlineData("19\t0", "6\t0")]
    [InlineData("input_bits 8", "input_bits 16")]
    [InlineData("output_bits 16", "output_bits 8")]
    [InlineData("authority user_correction", "authority tier_a")]
    [InlineData("dataset stock_levels", "dataset other")]
    [InlineData("dataset stock_levels", "dataset  stock_levels")]
    [InlineData("CNET_LOCAL_TABLE_V1", "CNET_LOCAL_TABLE_V2")]
    [InlineData("rows 3\n", "# comment\nrows 3\n")]
    public void NoncanonicalSourcesRefuse(string from, string to) =>
        Assert.Throws<ArgumentException>(() => LocalTableReference.Parse(
            Encoding.ASCII.GetBytes(Source.Replace(from, to)), Authorized));

    [Fact]
    public void NewlinesAndByteBoundsRefuseBeforeDecoding()
    {
        foreach (var malformed in new[] { Source.TrimEnd('\n'), Source + "\n", Source.Replace("\n", "\r\n"), "\ufeff" + Source, Source + "\0" })
            Assert.Throws<ArgumentException>(() => LocalTableReference.Parse(Encoding.UTF8.GetBytes(malformed), Authorized));
        var oversized = Enumerable.Repeat((byte)0xff, 4097).ToArray();
        Assert.Equal("local_table_size", Assert.Throws<ArgumentException>(() => LocalTableReference.Parse(oversized, Authorized)).Message);
        Assert.Equal("local_table_size", Assert.Throws<ArgumentException>(() => LocalTableReference.Parse([], Authorized)).Message);
        Assert.Equal("local_table_size", Assert.Throws<ArgumentException>(() => LocalTableReference.Parse(null!, Authorized)).Message);
    }

    [Fact]
    public void FullDomainAllowsBoundaryValuesAndVerifiedToolAuthority()
    {
        var text = "CNET_LOCAL_TABLE_V1\ndataset calibration\nauthority verified_tool\ninput_bits 8\noutput_bits 16\nrows 256\n"
            + string.Concat(Enumerable.Range(0, 256).Select(key => $"{key}\t{key * 257}\n"));
        var table = LocalTableReference.Parse(Encoding.ASCII.GetBytes(text), new("calibration", "verified_tool"));
        Assert.Equal(256, table.Values.Count);
        Assert.Equal((ushort)0, table.ExpectedFor(0));
        Assert.Equal(ushort.MaxValue, table.ExpectedFor(255));
        for (var key = 0; key < 256; key++) Assert.Equal((ushort)(key * 257), table.ExpectedByKey[key]);
    }

    [Fact]
    public void SourceSnapshotAndAllExposedCollectionsAreImmutable()
    {
        var source = Encoding.ASCII.GetBytes(Source);
        var table = LocalTableReference.Parse(source, Authorized);
        var hash = table.SourceSha256;
        Array.Fill(source, (byte)0);
        Assert.Equal((ushort)42, table.ExpectedFor(7));
        Assert.Equal(hash, table.SourceSha256);
        var values = Assert.IsAssignableFrom<IDictionary<byte, ushort>>(table.Values);
        Assert.Throws<NotSupportedException>(() => values[7] = 65535);
        var expected = Assert.IsAssignableFrom<IList<ushort?>>(table.ExpectedByKey);
        Assert.Throws<NotSupportedException>(() => expected[7] = null);
        var changed = LocalTableReference.Parse(Encoding.ASCII.GetBytes(Source.Replace("7\t42", "7\t43")), Authorized);
        Assert.NotEqual(hash, changed.SourceSha256);
    }

    [Theory]
    [InlineData("stock_levels", "verified_tool")]
    [InlineData("Stock_levels", "user_correction")]
    [InlineData("../stock_levels", "user_correction")]
    [InlineData("stock_levels", "tier_a")]
    [InlineData("stock_levels", "")]
    [InlineData("", "user_correction")]
    public void ExplicitOwnerAuthorityMustMatch(string dataset, string authority) =>
        Assert.Throws<ArgumentException>(() => LocalTableReference.Parse(
            Encoding.ASCII.GetBytes(Source), new(dataset, authority)));

    [Fact]
    public void DatasetIdentifierBoundaryIsCanonical()
    {
        var longest = "a" + new string('_', 30);
        var source = Source.Replace("stock_levels", longest);
        Assert.Equal(longest, LocalTableReference.Parse(Encoding.ASCII.GetBytes(source), new(longest, "user_correction")).Dataset);
        Assert.Throws<ArgumentException>(() => LocalTableReference.Parse(
            Encoding.ASCII.GetBytes(Source.Replace("stock_levels", longest + "a")), new(longest + "a", "user_correction")));
    }

    [Fact]
    public void EveryDisallowedByteRefusesInsteadOfReplacementDecoding()
    {
        foreach (var value in Enumerable.Range(0, 256).Where(value => value is not (>= 32 and <= 126 or 9 or 10)))
        {
            var source = Encoding.ASCII.GetBytes(Source);
            source[0] = (byte)value;
            Assert.Equal("local_table_ascii", Assert.Throws<ArgumentException>(
                () => LocalTableReference.Parse(source, Authorized)).Message);
        }
    }
}
