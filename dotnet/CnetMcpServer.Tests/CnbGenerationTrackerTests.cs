using System;
using System.IO;
using System.Security.Cryptography;
using CnetMcpServer;
using Xunit;

namespace CnetMcpServer.Tests;

/* RED marker: MCP_CNB_IDENTITY_RED */
public sealed class CnbGenerationTrackerTests
{
    [Fact]
    public void Same_Size_Atomic_Replacement_Is_A_Content_Change()
    {
        string root = Path.Combine(Path.GetTempPath(), $"cnet-cnb-id-{Guid.NewGuid():N}");
        Directory.CreateDirectory(root);
        string live = Path.Combine(root, "live.cnb");
        string next = Path.Combine(root, "live.cnb.next");
        try
        {
            File.WriteAllBytes(live, new byte[] { 1, 2, 3, 4 });
            var tracker = new CnbGenerationTracker(live);
            Assert.Equal(CnbGenerationChange.Initial,
                tracker.Observe(out CnbFileIdentity first));
            tracker.MarkApplied(first);

            File.WriteAllBytes(next, new byte[] { 4, 3, 2, 1 });
            File.Move(next, live, overwrite: true);
            File.SetLastWriteTimeUtc(live, DateTime.UtcNow.AddSeconds(2));

            Assert.Equal(CnbGenerationChange.ContentChanged,
                tracker.Observe(out CnbFileIdentity replacement));
            Assert.Equal(first.Length, replacement.Length);
            Assert.NotEqual(first.Sha256, replacement.Sha256);
            Assert.Equal(CnbGenerationChange.ContentChanged,
                tracker.Observe(out CnbFileIdentity pending));
            Assert.Equal(replacement, pending);
            tracker.MarkApplied(replacement);
            Assert.Equal(CnbGenerationChange.Current,
                tracker.Observe(out CnbFileIdentity applied));
            Assert.Equal(replacement, applied);
        }
        finally
        {
            Directory.Delete(root, recursive: true);
        }
    }

    [Fact]
    public void Same_Content_Metadata_Change_Advances_The_Applied_Stamp()
    {
        string path = Path.Combine(Path.GetTempPath(), $"cnet-cnb-touch-{Guid.NewGuid():N}.cnb");
        try
        {
            File.WriteAllBytes(path, new byte[] { 8, 6, 7, 5, 3, 0, 9 });
            var tracker = new CnbGenerationTracker(path);
            Assert.Equal(CnbGenerationChange.Initial,
                tracker.Observe(out CnbFileIdentity first));
            tracker.MarkApplied(first);

            File.SetLastWriteTimeUtc(path, DateTime.UtcNow.AddSeconds(2));
            Assert.Equal(CnbGenerationChange.MetadataOnly,
                tracker.Observe(out CnbFileIdentity touched));
            tracker.MarkApplied(touched);
            Assert.Equal(CnbGenerationChange.Current,
                tracker.Observe(out CnbFileIdentity current));
            Assert.Equal(touched, current);
        }
        finally
        {
            File.Delete(path);
        }
    }

    [Fact]
    public void Replacement_During_Read_Retries_And_Returns_One_Stable_Generation()
    {
        string root = Path.Combine(Path.GetTempPath(), $"cnet-cnb-race-{Guid.NewGuid():N}");
        Directory.CreateDirectory(root);
        string live = Path.Combine(root, "live.cnb");
        string next = Path.Combine(root, "live.cnb.next");
        byte[] oldBytes = { 1, 1, 1, 1, 1, 1, 1, 1 };
        byte[] newBytes = { 2, 2, 2, 2, 2, 2, 2, 2 };
        try
        {
            File.WriteAllBytes(live, oldBytes);
            File.WriteAllBytes(next, newBytes);
            DateTime replacementTime = DateTime.UtcNow.AddSeconds(2);

            bool ok = CnbFileIdentityReader.TryReadStable(
                live, out CnbFileIdentity identity, attempt =>
                {
                    if (attempt != 0) return;
                    File.Move(next, live, overwrite: true);
                    File.SetLastWriteTimeUtc(live, replacementTime);
                });

            Assert.True(ok);
            Assert.Equal(Convert.ToHexString(SHA256.HashData(newBytes)), identity.Sha256);
        }
        finally
        {
            Directory.Delete(root, recursive: true);
        }
    }
}
