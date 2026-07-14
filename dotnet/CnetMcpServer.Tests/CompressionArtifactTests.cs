using System;
using System.IO;
using System.Text.Json;
using CNET.Cce;
using CnetMcpServer;
using Xunit;

namespace CnetMcpServer.Tests;

/// <summary>
/// TDD for native compression artifact creation via the MCP CompressModel tool.
///
/// Acceptance: CompressModel must produce a REAL native QGKP artifact (not
/// only a JSON manifest), with a recognized magic, verifiable via the native
/// inspection path. It must fail honestly for missing/unsupported inputs.
/// </summary>
public class CompressionArtifactTests
{
    /// <summary>
    /// Deterministic minimal GGUF fixture: valid GGUF magic + minimal header.
    /// No network or private model required — QGKP envelopes any GGUF payload
    /// byte-exact, so a tiny synthetic GGUF exercises the full native path.
    /// </summary>
    private static string CreateGgufFixture(string dir)
    {
        string path = Path.Combine(dir, "tiny_fixture.gguf");
        // GGUF magic (little-endian 'GGUF' = 0x46554747) + version 3 + minimal tensor count
        var bytes = new byte[64];
        // magic: "GGUF" in little-endian uint32
        bytes[0] = 0x47; bytes[1] = 0x47; bytes[2] = 0x55; bytes[3] = 0x46;
        // version: 3 (little-endian uint32)
        bytes[4] = 3; bytes[5] = 0; bytes[6] = 0; bytes[7] = 0;
        // tensor_count: 0
        // metadata_kv_count: 0
        // remaining bytes are padding/payload (QGKP envelopes arbitrary GGUF content)
        // Add some deterministic payload beyond the header
        for (int i = 8; i < 64; i++)
            bytes[i] = (byte)(i * 31 & 0xFF);
        File.WriteAllBytes(path, bytes);
        return path;
    }

    [Fact]
    public void CompressModel_Produces_Real_Native_QGKP_Artifact()
    {
        string dir = Path.Combine(Path.GetTempPath(), $"cnet_compress_{Guid.NewGuid():N}");
        Directory.CreateDirectory(dir);

        try
        {
            string ggufPath = CreateGgufFixture(dir);

            // We need a basePath that points to a real .cnb for SoulHost init.
            // Use the repo's fixture if available, otherwise create a dummy
            // path — CompressModel's native compression does not require the
            // soul host to be loaded for the QGKP path.
            string basePath = Environment.GetEnvironmentVariable("CNET_BASE_PATH")
                ?? "/home/marble/AI/CNET/soul_gemma4v2_final.cnb";

            CnetTools tools;
            try
            {
                tools = new CnetTools(basePath, dir, dir);
            }
            catch
            {
                // If the soul host can't open (no .cnb in test env), use a
                // throwaway path — CompressModel should work independently.
                tools = new CnetTools(Path.Combine(dir, "dummy.cnb"), dir, dir);
            }

            string result = tools.CompressModel(ggufPath, "1.6bit", "");

            // Must NOT be a manifest-only result
            Assert.DoesNotContain("manifest only", result);
            Assert.DoesNotContain("manifest_only", result);

            // Must report a real artifact path
            Assert.Contains(".qgkp", result);

            // The result must be JSON with structured fields
            Assert.True(result.StartsWith("{") || result.Contains("\"artifact_path\""),
                $"Result should be JSON or contain artifact_path field. Got: {result.Substring(0, Math.Min(200, result.Length))}");

            // Parse the JSON result
            using var doc = JsonDocument.Parse(result);
            var root = doc.RootElement;

            // Must have artifact_path pointing to a real file
            Assert.True(root.TryGetProperty("artifact_path", out var artPathEl),
                "Missing artifact_path field");
            string artifactPath = artPathEl.GetString()!;
            Assert.True(File.Exists(artifactPath),
                $"Artifact file does not exist: {artifactPath}");
            Assert.StartsWith(Path.GetFullPath(dir), Path.GetFullPath(artifactPath));

            // Artifact must have QGKP magic bytes (0x504b4751 = "QGKP" LE)
            byte[] artifactBytes = File.ReadAllBytes(artifactPath);
            Assert.True(artifactBytes.Length >= 4,
                "Artifact too small to contain magic");
            Assert.Equal(0x51, artifactBytes[0]); // 'Q'
            Assert.Equal(0x47, artifactBytes[1]); // 'G'
            Assert.Equal(0x4B, artifactBytes[2]); // 'K'
            Assert.Equal(0x50, artifactBytes[3]); // 'P'

            // Must report artifact size
            Assert.True(root.TryGetProperty("artifact_size", out var sizeEl),
                "Missing artifact_size field");
            long artifactSize = sizeEl.GetInt64();
            Assert.True(artifactSize > 4096,
                $"Artifact size {artifactSize} should be > 4096 (QGKP header alone is 4096)");

            // Must report a source integrity identifier (hash)
            Assert.True(root.TryGetProperty("source_integrity_hash", out var hashEl),
                "Missing source_integrity_hash field");
            string integrityHash = hashEl.GetString()!;
            Assert.False(string.IsNullOrEmpty(integrityHash),
                "source_integrity_hash must not be empty");

            // QGKP v3 is honest lossless packaging, not target-bit compression.
            Assert.Equal("packaged_losslessly", root.GetProperty("status").GetString());
            Assert.Equal("lossless_qgkp_envelope", root.GetProperty("artifact_kind").GetString());
            Assert.False(root.GetProperty("target_size_applied").GetBoolean());
            Assert.True(root.TryGetProperty("packaging", out var packagingEl),
                "Missing packaging field");
            Assert.True(packagingEl.ValueKind == JsonValueKind.Object,
                "packaging should be an object");
            Assert.True(root.GetProperty("quality_probe").GetProperty("round_trip_verified").GetBoolean());
            Assert.False(root.GetProperty("runtime").GetProperty("directly_loadable_by_hermes").GetBoolean());

            // Must report that native inspection validated the artifact
            Assert.True(root.TryGetProperty("native_inspection", out var inspEl),
                "Missing native_inspection field");
            Assert.True(inspEl.ValueKind == JsonValueKind.Object,
                "native_inspection should be an object");
            Assert.True(inspEl.TryGetProperty("version", out var verEl),
                "native_inspection should have version");
            Assert.Equal(3u, verEl.GetUInt32()); // CCE_QGKP_VERSION_ENVELOPE = 3

            // Must report a quality/probe evidence field
            Assert.True(root.TryGetProperty("quality_probe", out var probeEl),
                "Missing quality_probe field");
            Assert.True(probeEl.ValueKind == JsonValueKind.Object,
                "quality_probe should be an object");
        }
        finally
        {
            if (Directory.Exists(dir))
                Directory.Delete(dir, true);
        }
    }

    [Fact]
    public void CompressModel_Fails_Honestly_For_Missing_Model()
    {
        string dir = Path.Combine(Path.GetTempPath(), $"cnet_compress_fail_{Guid.NewGuid():N}");
        Directory.CreateDirectory(dir);

        try
        {
            string basePath = Path.Combine(dir, "dummy.cnb");
            var tools = new CnetTools(basePath, dir, dir);

            string missingPath = Path.Combine(dir, "does_not_exist.gguf");
            string result = tools.CompressModel(missingPath, "1.6bit", "");

            // Must explicitly report failure, not success
            Assert.Contains("refused", result.ToLowerInvariant());
            Assert.DoesNotContain("ready_for_native_compression", result);
            Assert.DoesNotContain("\"artifact_path\"", result);
        }
        finally
        {
            if (Directory.Exists(dir))
                Directory.Delete(dir, true);
        }
    }

    [Fact]
    public void CompressModel_Fails_Honestly_For_Non_GGUF_Input()
    {
        string dir = Path.Combine(Path.GetTempPath(), $"cnet_compress_nongguf_{Guid.NewGuid():N}");
        Directory.CreateDirectory(dir);

        try
        {
            // Create a file that is NOT a valid GGUF (wrong magic)
            string nonGgufPath = Path.Combine(dir, "not_gguf.bin");
            File.WriteAllBytes(nonGgufPath, new byte[] { 0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x01, 0x02, 0x03 });

            string basePath = Path.Combine(dir, "dummy.cnb");
            var tools = new CnetTools(basePath, dir, dir);

            string result = tools.CompressModel(nonGgufPath, "1.6bit", "");

            // Must report failure for non-GGUF input
            Assert.Contains("refused", result.ToLowerInvariant());
            Assert.DoesNotContain("\"artifact_path\"", result);
        }
        finally
        {
            if (Directory.Exists(dir))
                Directory.Delete(dir, true);
        }
    }

    [Fact]
    public void CompressModel_Fails_For_Empty_Model_Path()
    {
        string dir = Path.Combine(Path.GetTempPath(), $"cnet_compress_empty_{Guid.NewGuid():N}");
        Directory.CreateDirectory(dir);

        try
        {
            string basePath = Path.Combine(dir, "dummy.cnb");
            var tools = new CnetTools(basePath, dir, dir);

            string result = tools.CompressModel("", "1.6bit", "");
            Assert.Contains("refused", result.ToLowerInvariant());
        }
        finally
        {
            if (Directory.Exists(dir))
                Directory.Delete(dir, true);
        }
    }

    [Fact]
    public void ExpandContext_Does_Not_Falsely_Claim_Window_Expansion()
    {
        string dir = Path.Combine(Path.GetTempPath(), $"cnet_context_{Guid.NewGuid():N}");
        Directory.CreateDirectory(dir);
        try
        {
            var tools = new CnetTools(Path.Combine(dir, "dummy.cnb"), dir, dir);
            string result = tools.ExpandContext("test", 8192);
            Assert.Contains("does not increase the context window", result);
            Assert.DoesNotContain("128K effective", result);
        }
        finally
        {
            if (Directory.Exists(dir))
                Directory.Delete(dir, true);
        }
    }
}