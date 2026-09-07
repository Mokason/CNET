using System.Collections.ObjectModel;
using System.Security.Cryptography;
using System.Text;

namespace CnetControlPlane.Learning;

/// <summary>
/// Independent finite reference for canonical table bytes. A missing input is
/// explicit abstention, including when another input's correct value is zero.
/// This validates format/owner authorization, not the truth of supplied labels.
/// </summary>
public sealed class LocalTableReference
{
    public string Dataset { get; }
    public string Authority { get; }
    public string SourceSha256 { get; }
    public LocalSymbolReference? Symbols { get; }
    public IReadOnlyDictionary<byte, ushort> Values { get; }
    /// <summary>Exactly 256 entries: index is the uint8 input; null means abstain.</summary>
    public IReadOnlyList<ushort?> ExpectedByKey { get; }
    public ushort? ExpectedFor(byte key) => ExpectedByKey[key];

    private LocalTableReference(LearningDataset authorized, byte[] source,
        Dictionary<byte, ushort> values, LocalSymbolReference? symbols = null)
    {
        Dataset = authorized.Id;
        Authority = authorized.Authority;
        SourceSha256 = Convert.ToHexString(SHA256.HashData(source)).ToLowerInvariant();
        Values = new ReadOnlyDictionary<byte, ushort>(values);
        Symbols = symbols;
        var expected = new ushort?[256];
        foreach (var (key, value) in values) expected[key] = value;
        ExpectedByKey = Array.AsReadOnly(expected);
    }

    private static bool Identifier(string? text) => text is { Length: >= 1 and <= 31 }
        && text[0] is >= 'a' and <= 'z'
        && text.All(c => c is >= 'a' and <= 'z' or >= '0' and <= '9' or '_');

    private static int Decimal(ReadOnlySpan<char> text, int maximum)
    {
        if (text.Length is < 1 or > 5 || text.Length > 1 && text[0] == '0')
            throw new ArgumentException("local_table_decimal");
        var number = 0;
        foreach (var digit in text)
        {
            if (digit is < '0' or > '9') throw new ArgumentException("local_table_decimal");
            number = number * 10 + digit - '0';
            if (number > maximum) throw new ArgumentException("local_table_decimal");
        }
        return number;
    }

    public static LocalTableReference Parse(byte[] bytes, LearningDataset authorized)
    {
        // Reject the byte size before cloning, decoding, splitting or hashing.
        if (bytes is null || bytes.Length is < 1 or > 4096)
            throw new ArgumentException("local_table_size");
        if (authorized is null || !Identifier(authorized.Id)
            || authorized.Authority is not ("user_correction" or "verified_tool")
            || authorized.SymbolVocabularySha256 is not null && !LearningPolicy.IsHash(authorized.SymbolVocabularySha256))
            throw new ArgumentException("local_table_authority");
        // The mapping and hash must share a private snapshot of caller bytes.
        var source = (byte[])bytes.Clone();
        if (source.Any(b => b is not (>= 32 and <= 126 or 9 or 10)))
            throw new ArgumentException("local_table_ascii");
        var lines = Encoding.ASCII.GetString(source).Split('\n');
        var symbolic = authorized.SymbolVocabularySha256 is not null;
        if (lines.Length < 8 || lines[^1] != "" || lines[0] != (symbolic ? "CNET_LOCAL_SYMBOLS_V1" : "CNET_LOCAL_TABLE_V1")
            || lines[1] != "dataset " + authorized.Id
            || lines[2] != "authority " + authorized.Authority
            || lines[3] != "input_bits 8" || lines[4] != "output_bits 16"
            || !lines[5].StartsWith("rows ", StringComparison.Ordinal))
            throw new ArgumentException("local_table_header");
        var count = Decimal(lines[5].AsSpan(5), 256);
        if (count == 0 || lines.Length != count + 7)
            throw new ArgumentException("local_table_row_count");
        var values = new Dictionary<byte, ushort>(count);
        if (symbolic)
        {
            var symbols = new LocalSymbolReference(lines, count, authorized.SymbolVocabularySha256!);
            for (var key = 0; key < count; key++) values.Add((byte)key, (ushort)key);
            return new LocalTableReference(authorized, source, values, symbols);
        }
        var previous = -1;
        for (var row = 0; row < count; row++)
        {
            var fields = lines[row + 6].Split('\t');
            if (fields.Length != 2) throw new ArgumentException("local_table_row");
            var key = Decimal(fields[0], 255);
            var value = Decimal(fields[1], 65535);
            if (key <= previous) throw new ArgumentException("local_table_key_order");
            values.Add((byte)key, (ushort)value);
            previous = key;
        }
        return new LocalTableReference(authorized, source, values);
    }
}
