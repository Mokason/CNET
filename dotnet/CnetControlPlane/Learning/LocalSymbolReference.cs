using System.Collections.ObjectModel;
using System.Globalization;
using System.Security.Cryptography;
using System.Text;

namespace CnetControlPlane.Learning;

/// <summary>Exact ordinal-ASCII vocabulary and literal labels, never executable text.
/// The policy pins token-LF bytes so persistent byte-key demand cannot change meaning.</summary>
public sealed class LocalSymbolReference
{
    public IReadOnlyList<string> Keys { get; }
    public IReadOnlyList<string> Labels { get; }
    public string VocabularySha256 { get; }
    public string UnknownToken { get; }
    private readonly IReadOnlyDictionary<string, byte> ordinals;
    public bool TryEncode(string token, out byte key) => ordinals.TryGetValue(token, out key);
    public string? LabelFor(ushort value) => value < Labels.Count ? Labels[value] : null;
    public static bool IsKey(string? text) => text is { Length: >= 1 and <= 48 }
        && text.All(c => c is >= 'a' and <= 'z' or >= 'A' and <= 'Z' or >= '0' and <= '9' or '_' or '.' or ':' or '-');
    internal static bool IsLabel(string? text) => text is { Length: >= 1 and <= 128 }
        && text.All(c => c is >= ' ' and <= '~');

    internal LocalSymbolReference(string[] lines, int count, string vocabulary)
    {
        var keys = new string[count]; var labels = new string[count];
        var mapping = new Dictionary<string, byte>(StringComparer.Ordinal);
        for (var row = 0; row < count; row++)
        {
            var fields = lines[row + 6].Split('\t');
            if (fields.Length != 2 || !IsKey(fields[0]) || !IsLabel(fields[1]))
                throw new ArgumentException("local_symbol_row");
            if (row != 0 && StringComparer.Ordinal.Compare(keys[row - 1], fields[0]) >= 0)
                throw new ArgumentException("local_symbol_key_order");
            keys[row] = fields[0]; labels[row] = fields[1]; mapping.Add(keys[row], (byte)row);
        }
        VocabularySha256 = Convert.ToHexString(SHA256.HashData(Encoding.ASCII.GetBytes(string.Join('\n', keys) + "\n"))).ToLowerInvariant();
        if (VocabularySha256 != vocabulary) throw new ArgumentException("local_symbol_vocabulary_changed");
        Keys = Array.AsReadOnly(keys); Labels = Array.AsReadOnly(labels);
        ordinals = new ReadOnlyDictionary<string, byte>(mapping);
        // 257 fixed valid candidates guarantee one absent token for <=256 rows.
        UnknownToken = Enumerable.Range(0, 257).Select(i => "cnet_unknown_" + i.ToString(CultureInfo.InvariantCulture))
            .First(token => !mapping.ContainsKey(token));
    }
}
