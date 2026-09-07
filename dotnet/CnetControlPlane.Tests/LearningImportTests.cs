using System.Runtime.Versioning;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using CnetControlPlane.Learning;
using Xunit;

namespace CnetControlPlane.Tests;

[SupportedOSPlatform("linux")]
public sealed class LearningImportTests : IClassFixture<LearningCommandInstallation>
{
    private readonly LearningCommandInstallation installation;
    public LearningImportTests(LearningCommandInstallation installation) { this.installation = installation; }
    private const string Source = "CNET_LOCAL_TABLE_V1\ndataset calibration\nauthority verified_tool\ninput_bits 8\noutput_bits 16\nrows 2\n0\t0\n7\t42\n";
    private static string Hash(byte[] bytes) => Convert.ToHexString(SHA256.HashData(bytes)).ToLowerInvariant();
    [DllImport("libc", SetLastError = true)] private static extern int link(string existing, string target);
    private static byte[] Intake(LearningCommandDeployment deployment, string text = Source)
    {
        var bytes = Encoding.ASCII.GetBytes(text);
        deployment.Put("approved.tsv", bytes);
        return bytes;
    }

    [Fact]
    public async Task ExplicitPinnedImportFeedsActualAcquisitionWithoutOverwritingOrCreatingDemand()
    {
        using var deployment = installation.Deploy();
        Assert.Equal(0, (await deployment.Command("initialize")).Code);
        var bytes = Intake(deployment); var path = Path.Combine(deployment.Root, "approved.tsv");
        var imported = await deployment.Command("import", "calibration", path, Hash(bytes));
        Assert.True(imported.Code == 0, "LEARNING_IMPORT_RED approved initial evidence refused: " + imported.Error);
        Assert.Equal(bytes, File.ReadAllBytes(Path.Combine(deployment.Root, "work/data/calibration.tsv")));
        using (var receipt = JsonDocument.Parse(imported.Output))
        {
            Assert.Equal("learning_source_imported", receipt.RootElement.GetProperty("event").GetString());
            Assert.Equal(Hash(bytes), receipt.RootElement.GetProperty("source_sha256").GetString());
            Assert.Equal(2, receipt.RootElement.GetProperty("rows").GetInt32());
        }
        Assert.Equal(2, (await deployment.Command("import", "calibration", path, Hash(bytes))).Code);
        Assert.Equal(bytes, File.ReadAllBytes(Path.Combine(deployment.Root, "work/data/calibration.tsv")));
        await deployment.StartDaemon();
        using (var idle = JsonDocument.Parse((await deployment.Command("tick")).Output))
            Assert.Equal("idle", idle.RootElement.GetProperty("action").GetString());
        Assert.Equal(0, (await deployment.Command("ask", "calibration", "7")).Code);
        Assert.Equal(0, (await deployment.Command("tick")).Code);
        Assert.Equal(0, (await deployment.Command("verify", "calibration")).Code);
    }

    [Theory]
    [InlineData(0)]
    [InlineData(1)]
    public async Task ImportCountsItsOwnBytesAtTheExactPolicyBoundary(int overflow)
    {
        using var deployment = installation.Deploy();
        var policyBytes = Encoding.UTF8.GetBytes(LearningPolicyTests.Valid.Replace("\"max_storage_mib\":256", "\"max_storage_mib\":16"));
        deployment.Put("policy.json", policyBytes);
        Assert.Equal(0, (await deployment.Command("initialize")).Code);
        var bytes = Intake(deployment);
        deployment.Put("work/retained.bin", []);
        var limit = 16L * 1024 * 1024;
        long used;
        using (var ledger = LearningLedger.Open(Path.Combine(deployment.Root, "work"), LearningPolicy.Parse(policyBytes), new LearningClock()))
            used = ledger.MeasureStorage(limit).Bytes;
        using (var retained = new FileStream(Path.Combine(deployment.Root, "work/retained.bin"), FileMode.Open))
            retained.SetLength(limit - used - bytes.Length + overflow);
        var imported = await deployment.Command("import", "calibration", Path.Combine(deployment.Root, "approved.tsv"), Hash(bytes));
        Assert.True(imported.Code == (overflow == 0 ? 0 : 2), "LEARNING_IMPORT_QUOTA_RED exact-fit/overflow accounting: " + imported.Error);
        var target = Path.Combine(deployment.Root, "work/data/calibration.tsv");
        Assert.Equal(overflow == 0, File.Exists(target));
        if (overflow == 0) Assert.Equal(bytes, File.ReadAllBytes(target));
        else Assert.Contains("learning_storage_byte_limit", imported.Error);
    }

    [Theory]
    [InlineData("hash")]
    [InlineData("hash_format")]
    [InlineData("authority")]
    [InlineData("dataset")]
    [InlineData("format")]
    [InlineData("large")]
    [InlineData("mode")]
    [InlineData("symlink")]
    [InlineData("hardlink")]
    [InlineData("relative")]
    [InlineData("noncanonical")]
    [InlineData("owner")]
    [InlineData("quota")]
    public async Task UnapprovedOrUnsafeEvidenceRefusesBeforePublication(string fault)
    {
        using var deployment = installation.Deploy();
        if (fault == "quota") deployment.Put("policy.json", Encoding.UTF8.GetBytes(LearningPolicyTests.Valid.Replace("\"max_storage_mib\":256", "\"max_storage_mib\":16")));
        Assert.Equal(0, (await deployment.Command("initialize")).Code);
        var text = fault switch
        {
            "authority" => Source.Replace("verified_tool", "tier_a"),
            "format" => Source.Replace("7\t42", "7\t042"),
            "large" => new string('x', 4097),
            _ => Source
        };
        var bytes = Intake(deployment, text); var path = Path.Combine(deployment.Root, "approved.tsv");
        if (fault == "mode") File.SetUnixFileMode(path, UnixFileMode.UserRead | UnixFileMode.UserWrite | UnixFileMode.GroupRead);
        if (fault == "symlink") { File.Move(path, Path.Combine(deployment.Root, "original.tsv")); File.CreateSymbolicLink(path, "original.tsv"); }
        if (fault == "hardlink") Assert.Equal(0, link(path, Path.Combine(deployment.Root, "alias.tsv")));
        if (fault == "quota")
        {
            deployment.Put("work/retained.bin", []);
            using var retained = new FileStream(Path.Combine(deployment.Root, "work/retained.bin"), FileMode.Open);
            retained.SetLength(16 * 1024 * 1024);
        }
        using var work = LearningFiles.Open(Path.Combine(deployment.Root, "work"));
        using var owner = fault == "owner" ? work.AcquireLock("owner.lock") : null;
        var refused = await deployment.Command("import", fault == "dataset" ? "not_authorized" : "calibration",
            fault switch { "relative" => "approved.tsv", "noncanonical" => deployment.Root + "/./approved.tsv", _ => path },
            fault switch { "hash" => new string('0', 64), "hash_format" => Hash(bytes).ToUpperInvariant(), _ => Hash(bytes) });
        Assert.Equal(2, refused.Code);
        Assert.DoesNotContain("learning_command_usage", refused.Error);
        Assert.False(File.Exists(Path.Combine(deployment.Root, "work/data/calibration.tsv")));
        Assert.Empty(Directory.EnumerateFileSystemEntries(Path.Combine(deployment.Root, "work/data")));
        using var status = JsonDocument.Parse((await deployment.Command("status")).Output);
        Assert.Equal(0, status.RootElement.GetProperty("jobs").GetInt64());
        Assert.False(status.RootElement.GetProperty("paused").GetBoolean());
    }
}
