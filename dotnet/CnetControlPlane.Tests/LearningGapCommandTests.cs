using System.Runtime.Versioning;
using System.Text.Json;
using Xunit;

namespace CnetControlPlane.Tests;

[SupportedOSPlatform("linux")]
public sealed class LearningGapCommandTests : IClassFixture<LearningCommandInstallation>
{
    private readonly LearningCommandInstallation installation;
    public LearningGapCommandTests(LearningCommandInstallation installation) => this.installation = installation;

    [Fact]
    public async Task GroupsRecurringMissesWithoutMixingOriginsOrWritingTheLedger()
    {
        using var deployment = installation.Deploy();
        Assert.Equal(0, (await deployment.Command("initialize")).Code);
        await deployment.StartDaemon();
        foreach (var (id, origin) in new[] { ('a', "synthetic"), ('b', "synthetic"), ('c', "unreviewed") })
            Assert.Equal(0, (await deployment.Command("observe", "calibration", "7", origin, new string(id, 32))).Code);
        var path = Path.Combine(deployment.Root, "work/ledger.sqlite");
        var before = File.ReadAllBytes(path);
        var response = await deployment.Command("gaps", "1");
        Assert.True(response.Code == 0, "TASK_GAPS_RED grouped inbox command missing: " + response.Error);
        using var result = JsonDocument.Parse(response.Output);
        var report = result.RootElement.GetProperty("report");
        Assert.Equal(3, report.GetProperty("Observations").GetInt32());
        Assert.Equal(2, report.GetProperty("TotalGroups").GetInt32());
        Assert.True(report.GetProperty("Truncated").GetBoolean());
        Assert.False(report.GetProperty("TrainingEligible").GetBoolean());
        var group = Assert.Single(report.GetProperty("Groups").EnumerateArray());
        Assert.Equal("synthetic", group.GetProperty("Origin").GetString());
        Assert.Equal(2, group.GetProperty("Requests").GetInt32());
        Assert.Equal(2, group.GetProperty("PendingApprovals").GetInt32());
        Assert.Equal("awaiting_evidence", group.GetProperty("LatestState").GetString());
        Assert.Equal(JsonValueKind.Null, report.GetProperty("EstimatedLearningSeconds").ValueKind);
        Assert.Equal(before, File.ReadAllBytes(path));
        foreach (var limit in new[] { "0", "33", "01", "-1" })
            Assert.Equal(2, (await deployment.Command("gaps", limit)).Code);
        Assert.Equal(before, File.ReadAllBytes(path));
    }
}
