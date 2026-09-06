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
        if (nativeOutput is null || nativeOutput.Length is < 1 or > 8192)
            throw new ArgumentException("learning_table_evaluation_size");
        if (reference is null || expectedSnapshot is not { Length: 64 }
            || expectedSnapshot.Any(c => c is not (>= '0' and <= '9' or >= 'a' and <= 'f')))
            throw new ArgumentException("learning_table_evaluation_identity");
        // Comparison and receipt identity share this one private input snapshot.
        var snapshot = (byte[])nativeOutput.Clone();
        if (snapshot.Any(b => b is not (>= 32 and <= 126 or 9 or 10)))
            throw new ArgumentException("learning_table_evaluation_ascii");
        var lines = Encoding.ASCII.GetString(snapshot).Split('\n');
        if (lines.Length != 263 || lines[262] != "" || lines[261] != "end"
            || lines[0] != "CNET_TABLE_SNAPSHOT_EVAL_V1"
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
        return new(reference.Dataset, reference.SourceSha256, expectedSnapshot,
            Convert.ToHexString(SHA256.HashData(snapshot)).ToLowerInvariant());
    }
}
