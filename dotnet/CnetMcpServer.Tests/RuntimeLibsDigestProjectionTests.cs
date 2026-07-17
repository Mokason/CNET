using System;
using System.IO;
using System.Text.Json;
using CNET.Cce;
using CnetMcpServer;
using Xunit;

namespace CnetMcpServer.Tests;

/// <summary>
/// Focused TDD for full artifact and v5 linked-runtime provenance projection
/// through SoulHost, OracleDescriptor, and ListOracles JSON. Uses the REAL native .cnb fixture
/// built by `make soul_host_test` (CNET_KEEP_TEST_BASE=1), which seals an
/// Oracle descriptor with runtime_libs_digest = 0xC1B5C0DE.
/// </summary>
public sealed class RuntimeLibsDigestProjectionTests : IDisposable
{
    private readonly string _basePath;
    private readonly string _tempDir;

    public RuntimeLibsDigestProjectionTests()
    {
        // The native soul_host_test fixture writes tmp_soul_host.cnb in the
        // repo root when CNET_KEEP_TEST_BASE=1. It contains one Oracle
        // descriptor ("unified_teacher") with runtime_libs_digest = 0xC1B5C0DE.
        // Search upward from the test output directory to find it.
        _basePath = FindFixture("tmp_soul_host.cnb", AppContext.BaseDirectory);
        _tempDir = Path.Combine(Path.GetTempPath(), $"cnet_rtlibs_{Guid.NewGuid():N}");
        Directory.CreateDirectory(_tempDir);
    }

    public void Dispose()
    {
        if (Directory.Exists(_tempDir))
            Directory.Delete(_tempDir, true);
    }

    private static string FindFixture(string filename, string startDir)
    {
        string? dir = Path.GetFullPath(startDir).TrimEnd(Path.DirectorySeparatorChar);
        for (int i = 0; i < 10 && dir != null; i++)
        {
            string candidate = Path.Combine(dir, filename);
            if (File.Exists(candidate)) return candidate;
            dir = Path.GetDirectoryName(dir);
        }
        return Path.Combine(startDir, filename);  // not found — returns a non-existent path
    }

    [Fact]
    public void SoulHost_OracleDescriptor_Projects_RuntimeLibsDigest_From_Native()
    {
        if (!File.Exists(_basePath))
        {
            // Skip when the native fixture hasn't been built yet — but only
            // after we've confirmed the type surface exists (see below).
            Assert.True(File.Exists(_basePath),
                "tmp_soul_host.cnb fixture missing — run `make soul_host_test` first");
        }

        using var host = new SoulHost(_basePath);
        var oracles = host.Oracles();
        Assert.Single(oracles);
        var desc = oracles[0];
        Assert.Equal("unified_teacher", desc.Name);
        Assert.Equal(
            "a0a1a2a3a4a5a6a7a8a9aaabacadaeafb0b1b2b3b4b5b6b7b8b9babbbcbdbebf",
            desc.ArtifactSha256);
        // The exact persisted v5 linked-runtime provenance digest.
        Assert.Equal(0xC1B5C0DEUL, desc.RuntimeLibsDigest);
    }

    [Fact]
    public void ListOracles_JSON_Exposes_RuntimeLibsDigest_As_Hex()
    {
        if (!File.Exists(_basePath))
        {
            Assert.True(File.Exists(_basePath),
                "tmp_soul_host.cnb fixture missing — run `make soul_host_test` first");
        }

        var tools = new CnetTools(_basePath, _tempDir, _tempDir);
        string json = tools.ListOracles();
        using var doc = JsonDocument.Parse(json);
        var oracles = doc.RootElement.GetProperty("oracles");
        Assert.Equal(1, oracles.GetArrayLength());
        var first = oracles[0];
        Assert.Equal(
            "a0a1a2a3a4a5a6a7a8a9aaabacadaeafb0b1b2b3b4b5b6b7b8b9babbbcbdbebf",
            first.GetProperty("artifactSha256").GetString());
        Assert.True(first.TryGetProperty("runtimeLibsDigest", out var rtField),
            "ListOracles JSON must include runtimeLibsDigest");
        string hex = rtField.GetString()!;
        Assert.StartsWith("0x", hex);
        // 0x + 16 lowercase hex chars
        Assert.Equal(18, hex.Length);
        // 0xC1B5C0DE as a 64-bit value formatted x16
        Assert.Equal("0x00000000c1b5c0de", hex);
    }

    [Fact]
    public void OracleDescriptor_Record_Has_RuntimeLibsDigest_Field()
    {
        // Type-surface check: the record must have the new field, even
        // without the native fixture. This ensures the managed binding
        // was extended.
        var descriptor = new OracleDescriptor(
            "test", "builtin",
            1UL, 2UL, 3UL, 4UL, 5UL, 6UL,
            "a0a1a2a3a4a5a6a7a8a9aaabacadaeafb0b1b2b3b4b5b6b7b8b9babbbcbdbebf",
            7UL);
        Assert.Equal(
            "a0a1a2a3a4a5a6a7a8a9aaabacadaeafb0b1b2b3b4b5b6b7b8b9babbbcbdbebf",
            descriptor.ArtifactSha256);
        Assert.Equal(7UL, descriptor.RuntimeLibsDigest);

        var legacy = new OracleDescriptor(
            "legacy", "builtin",
            1UL, 2UL, 3UL, 4UL, 5UL, 6UL);
        Assert.Equal(new string('0', 64), legacy.ArtifactSha256);
        Assert.Equal(0UL, legacy.RuntimeLibsDigest);
    }
}