using System;
using System.IO;
using System.Text;
using CNET.Cce.Interop;

namespace CNET.Cce;

/// <summary>
/// Result of a native QGKP compression artifact creation.
/// All fields are populated from the real native inspection path —
/// never from a manifest-only sidecar.
/// </summary>
public sealed record CnetCompressionResult
{
    /// <summary>Path to the .qgkp artifact on disk.</summary>
    public string ArtifactPath { get; init; } = "";

    /// <summary>Size of the artifact in bytes.</summary>
    public long ArtifactSize { get; init; }

    /// <summary>FNV-1a hash of the source GGUF payload (from native inspection).</summary>
    public string SourceIntegrityHash { get; init; } = "";

    /// <summary>Size of the original source model in bytes.</summary>
    public long SourceSize { get; init; }

    /// <summary>
    /// Source bytes divided by artifact bytes. QGKP v3 is lossless packaging,
    /// so this is normally less than one because of the envelope header; it is
    /// not a model-compression ratio.
    /// </summary>
    public double StorageRatio { get; init; }

    /// <summary>Native artifact kind. This implementation is lossless packaging.</summary>
    public string ArtifactKind { get; init; } = "lossless_qgkp_envelope";

    /// <summary>The requested target label, retained as provenance only.</summary>
    public string RequestedTargetSize { get; init; } = "";

    /// <summary>False: QGKP v3 does not quantize or otherwise change model weights.</summary>
    public bool TargetSizeApplied { get; init; }

    /// <summary>Strategy label from the native artifact format.</summary>
    public string Strategy { get; init; } = "";

    /// <summary>Native QGKP inspection result (version, flags, metadata).</summary>
    public QgkpInspection Inspection { get; init; } = new();

    /// <summary>Quality/probe evidence: the materialized GGUF round-trip check.</summary>
    public QualityProbe Probe { get; init; } = new();

    /// <summary>True only after native inspection and a byte-identical materialization probe.</summary>
    public bool Verified { get; init; }
}

public sealed record QgkpInspection
{
    public uint Version { get; init; }
    public ulong HeaderBytes { get; init; }
    public ulong Flags { get; init; }
    public ulong PayloadBytes { get; init; }
    public ulong PayloadHash { get; init; }
    public string Architecture { get; init; } = "";
    public string Quantization { get; init; } = "";
    public int NLayer { get; init; }
    public int Hidden { get; init; }
    public int ContextLength { get; init; }
}

public sealed record QualityProbe
{
    /// <summary>True when the materialized GGUF is byte-identical to the source.</summary>
    public bool RoundTripVerified { get; init; }

    /// <summary>Path to the materialized GGUF (temp file, may be deleted).</summary>
    public string MaterializedPath { get; init; } = "";

    /// <summary>SHA-256 of the source file (deterministic integrity identifier).</summary>
    public string SourceSha256 { get; init; } = "";
}

/// <summary>
/// Managed wrapper over the native QGKP compression envelope.
/// Creates real CNET compression artifacts via cce_qgkp_pack_gguf and
/// validates them via cce_qgkp_inspect + cce_qgkp_materialize_gguf.
/// </summary>
public static class CnetCompression
{
    // QGKP magic: 0x504b4751 ("QGKP" in little-endian)
    public const uint QgkpMagic = 0x504b4751u;
    public const uint QgkpVersionEnvelope = 3u;
    public const ulong QgkpHeaderBytes = 4096u;

    // Flags from cce_qgkp.h
    public const ulong FlagEmbeddedGguf = 1u << 0;
    public const ulong FlagPacketTrits = 1u << 1;
    public const ulong FlagMixedQuant = 1u << 2;

    /// <summary>
    /// Create a native QGKP v3 lossless package from a GGUF model.
    /// Calls cce_qgkp_pack_gguf, validates via cce_qgkp_inspect, and commits
    /// atomically only after cce_qgkp_materialize_gguf proves byte identity.
    /// </summary>
    /// <param name="ggufPath">Path to the source GGUF model file.</param>
    /// <param name="outputDir">Directory for the .qgkp artifact.</param>
    /// <param name="targetSize">Target size label (e.g. "1.6bit").</param>
    /// <param name="options">Extra compression options.</param>
    /// <returns>Compression result with native verification evidence.</returns>
    /// <exception cref="FileNotFoundException">Source GGUF does not exist.</exception>
    /// <exception cref="InvalidOperationException">Native compression or validation failed.</exception>
    public static CnetCompressionResult Compress(
        string ggufPath, string outputDir,
        string targetSize = "1.6bit", string options = "")
    {
        if (string.IsNullOrWhiteSpace(ggufPath))
            throw new ArgumentException("ggufPath is required", nameof(ggufPath));
        if (!File.Exists(ggufPath))
            throw new FileNotFoundException($"Source model not found: {ggufPath}", ggufPath);

        // Verify GGUF magic before attempting native compression
        byte[] magicBytes = new byte[4];
        using (var fs = File.OpenRead(ggufPath))
        {
            if (fs.Read(magicBytes, 0, 4) < 4 ||
                magicBytes[0] != 0x47 || magicBytes[1] != 0x47 ||
                magicBytes[2] != 0x55 || magicBytes[3] != 0x46)
            {
                throw new InvalidOperationException(
                    $"Source file is not a valid GGUF (magic mismatch): {ggufPath}");
            }
        }

        if (string.IsNullOrWhiteSpace(outputDir))
            throw new ArgumentException("outputDir is required", nameof(outputDir));
        outputDir = Path.GetFullPath(outputDir);
        Directory.CreateDirectory(outputDir);

        string modelStem = Path.GetFileNameWithoutExtension(
            Path.GetFileName(ggufPath.TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar)));
        if (string.IsNullOrWhiteSpace(modelStem)) modelStem = "cnet_model";
        string safeStem = SanitizeFileStem(modelStem);
        string qgkpPath = Path.Combine(outputDir, safeStem + ".qgkp");
        string tempQGKPPath = Path.Combine(outputDir,
            $".{safeStem}.{Guid.NewGuid():N}.qgkp.partial");
        string materializedPath = Path.Combine(outputDir,
            $".{safeStem}.{Guid.NewGuid():N}.restored.gguf.partial");

        // QGKP v3 is a lossless envelope. A target-size request is retained
        // as provenance but must never be presented as applied quantization.
        const string strategy = "lossless_qgkp_envelope";

        // Build native metadata
        var meta = new CceNative.CceQgkpMetadata
        {
            Flags = FlagEmbeddedGguf,
            Architecture = EncodeString("cnet_generic", 64),
            Quantization = EncodeString(strategy, 32),
            NLayer = 0,
            Hidden = 0,
            ContextLength = 0,
            Reserved = 0
        };

        try
        {
            int rc = CceNative.QgkpPackGguf(ggufPath, tempQGKPPath, ref meta);
            if (rc != 0 || !File.Exists(tempQGKPPath))
                throw new InvalidOperationException(
                    $"Native QGKP package failed (rc={rc}) for {ggufPath}");

            var info = new CceNative.CceQgkpInfo();
            rc = CceNative.QgkpInspect(tempQGKPPath, ref info);
            if (rc != 0 || info.Version != QgkpVersionEnvelope ||
                info.HeaderBytes != QgkpHeaderBytes)
                throw new InvalidOperationException(
                    $"Native QGKP inspection failed (rc={rc}) for {tempQGKPPath}");

            byte[] artifactMagic = new byte[4];
            using (var af = File.OpenRead(tempQGKPPath))
            {
                if (af.Read(artifactMagic, 0, artifactMagic.Length) != artifactMagic.Length)
                    throw new InvalidOperationException("QGKP artifact is shorter than its magic.");
            }
            if (artifactMagic[0] != 0x51 || artifactMagic[1] != 0x47 ||
                artifactMagic[2] != 0x4B || artifactMagic[3] != 0x50)
                throw new InvalidOperationException(
                    $"Artifact does not have QGKP magic: {tempQGKPPath}");

            rc = CceNative.QgkpMaterializeGguf(tempQGKPPath, materializedPath);
            if (rc != 0 || !File.Exists(materializedPath) ||
                !FilesEqual(ggufPath, materializedPath))
                throw new InvalidOperationException(
                    $"Native QGKP round-trip verification failed (rc={rc}) for {tempQGKPPath}");

            long sourceSize = new FileInfo(ggufPath).Length;
            long artifactSize = new FileInfo(tempQGKPPath).Length;
            string sourceSha256 = ComputeSha256(ggufPath);

            // Same-directory replacement is atomic on the supported filesystem.
            File.Move(tempQGKPPath, qgkpPath, overwrite: true);

            return new CnetCompressionResult
            {
                ArtifactPath = qgkpPath,
                ArtifactSize = artifactSize,
                SourceIntegrityHash = $"0x{info.PayloadHash:x16}",
                SourceSize = sourceSize,
                StorageRatio = artifactSize > 0 ? (double)sourceSize / artifactSize : 0,
                ArtifactKind = strategy,
                RequestedTargetSize = targetSize,
                TargetSizeApplied = false,
                Strategy = strategy,
                Verified = true,
                Inspection = new QgkpInspection
                {
                    Version = info.Version,
                    HeaderBytes = info.HeaderBytes,
                    Flags = info.Flags,
                    PayloadBytes = info.PayloadBytes,
                    PayloadHash = info.PayloadHash,
                    Architecture = DecodeString(info.Architecture),
                    Quantization = DecodeString(info.Quantization),
                    NLayer = info.NLayer,
                    Hidden = info.Hidden,
                    ContextLength = info.ContextLength
                },
                Probe = new QualityProbe
                {
                    RoundTripVerified = true,
                    MaterializedPath = "",
                    SourceSha256 = sourceSha256
                }
            };
        }
        finally
        {
            try { if (File.Exists(tempQGKPPath)) File.Delete(tempQGKPPath); } catch { }
            try { if (File.Exists(materializedPath)) File.Delete(materializedPath); } catch { }
        }
    }

    /// <summary>Inspect a QGKP artifact without full decompression.</summary>
    public static QgkpInspection Inspect(string qgkpPath)
    {
        if (!File.Exists(qgkpPath))
            throw new FileNotFoundException($"QGKP artifact not found: {qgkpPath}", qgkpPath);

        var info = new CceNative.CceQgkpInfo();
        int rc = CceNative.QgkpInspect(qgkpPath, ref info);
        if (rc != 0)
            throw new InvalidOperationException(
                $"Native QGKP inspect failed (rc={rc}) for {qgkpPath}");

        return new QgkpInspection
        {
            Version = info.Version,
            HeaderBytes = info.HeaderBytes,
            Flags = info.Flags,
            PayloadBytes = info.PayloadBytes,
            PayloadHash = info.PayloadHash,
            Architecture = DecodeString(info.Architecture),
            Quantization = DecodeString(info.Quantization),
            NLayer = info.NLayer,
            Hidden = info.Hidden,
            ContextLength = info.ContextLength
        };
    }

    // ---- helpers ----


    private static string SanitizeFileStem(string value)
    {
        var chars = value.ToCharArray();
        for (int i = 0; i < chars.Length; ++i)
        {
            if (!(char.IsLetterOrDigit(chars[i]) || chars[i] == '-' || chars[i] == '_' || chars[i] == '.'))
                chars[i] = '_';
        }
        return new string(chars);
    }

    private static byte[] EncodeString(string value, int size)
    {
        var bytes = new byte[size];
        var src = Encoding.UTF8.GetBytes(value);
        Array.Copy(src, bytes, Math.Min(src.Length, size - 1));
        return bytes;
    }

    private static string DecodeString(byte[] bytes)
    {
        int len = Array.IndexOf(bytes, (byte)0);
        if (len < 0) len = bytes.Length;
        return Encoding.UTF8.GetString(bytes, 0, len);
    }

    private static bool FilesEqual(string a, string b)
    {
        using var fa = File.OpenRead(a);
        using var fb = File.OpenRead(b);
        if (fa.Length != fb.Length) return false;
        var bufA = new byte[64 * 1024];
        var bufB = new byte[64 * 1024];
        while (true)
        {
            int na = fa.Read(bufA, 0, bufA.Length);
            int nb = fb.Read(bufB, 0, bufB.Length);
            if (na != nb) return false;
            if (na == 0) return true;
            for (int i = 0; i < na; i++)
                if (bufA[i] != bufB[i]) return false;
        }
    }

    private static string ComputeSha256(string path)
    {
        using var sha = System.Security.Cryptography.SHA256.Create();
        using var stream = File.OpenRead(path);
        var hash = sha.ComputeHash(stream);
        return BitConverter.ToString(hash).Replace("-", "").ToLowerInvariant();
    }
}