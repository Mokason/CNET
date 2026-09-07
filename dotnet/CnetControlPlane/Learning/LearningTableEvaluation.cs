using System.Globalization;
using System.Security.Cryptography;
using System.Text;

namespace CnetControlPlane.Learning;

/// <summary>
/// A complete finite-table comparison against the independent owner-authorized
/// reference, not a worker PASS claim. The caller must separately require child
/// exit zero, unchanged source identity before/after execution, and binding of
/// the expected snapshot to the actual native staged view before activation.
/// This factory performs no filesystem, process, or native calls.
/// </summary>
internal sealed class LearningTableEvaluation
{
    public string Dataset { get; }
    public string SourceSha256 { get; }
    /// <summary>The evaluated snapshot hash, not an inferred active-set digest.</summary>
    public string CandidateSha256 { get; }
    public string ReceiptSha256 { get; }

    private LearningTableEvaluation(string dataset, string source, string candidate, string receipt)
    { Dataset = dataset; SourceSha256 = source; CandidateSha256 = candidate; ReceiptSha256 = receipt; }

    public static LearningTableEvaluation Parse(byte[] nativeOutput, LocalTableReference reference, string expectedSnapshot)
    {
        // Check the external byte bound before clone, decode, splitting or hash.
        if (nativeOutput is null || nativeOutput.Length < 1 || nativeOutput.Length > (reference?.Symbols is null ? 8192 : 16384))
            throw new ArgumentException("learning_table_evaluation_size");
        if (reference is null || expectedSnapshot is not { Length: 64 }
            || expectedSnapshot.Any(c => c is not (>= '0' and <= '9' or >= 'a' and <= 'f')))
            throw new ArgumentException("learning_table_evaluation_identity");
        // Comparison and receipt identity share this one private input snapshot.
        var snapshot = (byte[])nativeOutput.Clone();
        if (snapshot.Any(b => b is not (>= 32 and <= 126 or 9 or 10)))
            throw new ArgumentException("learning_table_evaluation_ascii");
        var lines = Encoding.ASCII.GetString(snapshot).Split('\n');
        var symbols = reference.Symbols;
        var end = symbols is null ? 261 : 263 + symbols.Keys.Count;
        if (lines.Length != end + 2 || lines[end + 1] != "" || lines[end] != "end"
            || lines[0] != (symbols is null ? "CNET_TABLE_SNAPSHOT_EVAL_V1" : "CNET_SYMBOL_SNAPSHOT_EVAL_V1")
            || lines[1] != "snapshot_sha256 " + expectedSnapshot
            || lines[2] != "dataset " + reference.Dataset
            || lines[3] != "source_sha256 " + reference.SourceSha256
            || lines[4] != "results 256")
            throw new ArgumentException("learning_table_evaluation_frame");
        for (var key = 0; key < 256; key++)
        {
            var value = reference.ExpectedFor((byte)key);
            var expected = key.ToString(CultureInfo.InvariantCulture) +
                (value.HasValue ? "\t1\t" + value.Value.ToString(CultureInfo.InvariantCulture) : "\t0\t-");
            // Exact comparison to the independently derived canonical row also
            // rejects leading zeros, overflow, extra fields and reordered keys.
            if (lines[key + 5] != expected)
                throw new ArgumentException("learning_table_evaluation_answer");
        }
        if (symbols is not null)
        {
            if (lines[261] != "symbols " + symbols.Keys.Count.ToString(CultureInfo.InvariantCulture)
                || lines[end - 1] != "unknown " + symbols.UnknownToken + "\t0\t-")
                throw new ArgumentException("learning_symbol_evaluation_frame");
            // The encoded identity task alone cannot attest token selection or
            // label rendering. Require observations from actual staged ASK.
            for (var row = 0; row < symbols.Keys.Count; row++)
                if (lines[262 + row] != symbols.Keys[row] + "\t1\t" + symbols.Labels[row])
                    throw new ArgumentException("learning_symbol_evaluation_answer");
        }
        return new(reference.Dataset, reference.SourceSha256, expectedSnapshot,
            Convert.ToHexString(SHA256.HashData(snapshot)).ToLowerInvariant());
    }
}
