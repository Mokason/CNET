using System.Diagnostics;
using System.Runtime.Versioning;
using System.Text;
using System.Text.Json;
using Xunit;

namespace CnetControlPlane.Tests;

[SupportedOSPlatform("linux")]
public sealed class LearningRunCommandTests : IClassFixture<LearningCommandInstallation>
{
    private readonly LearningCommandInstallation installation;
    public LearningRunCommandTests(LearningCommandInstallation installation) { this.installation = installation; }
    private static void ShortPolicy(LearningCommandDeployment deployment, int seconds = 2) => deployment.Put("policy.json", Encoding.UTF8.GetBytes(
        LearningPolicyTests.Valid.Replace("\"tick_seconds\":30", "\"tick_seconds\":1")
            .Replace("\"worker_seconds\":30", "\"worker_seconds\":2").Replace("\"max_probe_gap_seconds\":120", "\"max_probe_gap_seconds\":10")
            .Replace("\"max_run_seconds\":259200", "\"max_run_seconds\":" + seconds)));

    [Fact]
    public async Task ActualRunKeepsDurableHeartbeatAndCompletesOnlyAfterOriginalBudget()
    {
        using var deployment = installation.Deploy(); ShortPolicy(deployment);
        Assert.Equal(0, (await deployment.Command("initialize")).Code); await deployment.StartDaemon();
        var clock = Stopwatch.StartNew();
        var result = await deployment.Command("run");
        Assert.True(result.Code == 0, "LEARNING_RUN_COMMAND_RED run: " + result.Error);
        Assert.True(clock.Elapsed.TotalSeconds >= 2);
        using var status = JsonDocument.Parse((await deployment.Command("status")).Output);
        Assert.Equal("budget_complete", status.RootElement.GetProperty("run_state").GetString());
        Assert.True(status.RootElement.GetProperty("paused").GetBoolean());
        var run = status.RootElement.GetProperty("run");
        Assert.True(run.GetProperty("TickCount").GetInt64() >= 1);
        Assert.True(run.GetProperty("LastNanoseconds").GetInt64() - run.GetProperty("StartNanoseconds").GetInt64() >= 2_000_000_000);
        foreach (var line in result.Output.Split('\n', StringSplitOptions.RemoveEmptyEntries))
        {
            using var entry = JsonDocument.Parse(line);
            Assert.DoesNotContain("72", entry.RootElement.GetProperty("event").GetString()!);
            Assert.DoesNotContain("acceptance", entry.RootElement.GetProperty("event").GetString()!);
        }
        Assert.Equal(2, (await deployment.Command("run")).Code);
        Assert.Equal(2, (await deployment.Command("resume")).Code);
        using var after = JsonDocument.Parse((await deployment.Command("status")).Output);
        Assert.Equal(run.GetRawText(), after.RootElement.GetProperty("run").GetRawText());
    }

    [Fact]
    public async Task BudgetExpiryRollsBackUnacceptedCandidateBeforeCompletion()
    {
        using var deployment = installation.Deploy(); ShortPolicy(deployment);
        Assert.Equal(0, (await deployment.Command("initialize")).Code);
        deployment.Put("work/data/calibration.tsv", Encoding.ASCII.GetBytes(
            "CNET_LOCAL_TABLE_V1\ndataset calibration\nauthority verified_tool\ninput_bits 8\noutput_bits 16\nrows 1\n7\t42\n"));
        await deployment.StartDaemon(); Assert.Equal(0, (await deployment.Command("ask", "calibration", "7")).Code);
        var result = await deployment.Command("run");
        Assert.True(result.Code == 0, "LEARNING_RUN_COMMAND_RED probation expiry: " + result.Error);
        Assert.Contains("activated", result.Output); Assert.Contains("rolled_back", result.Output);
        using var answer = JsonDocument.Parse((await deployment.Command("ask", "calibration", "7")).Output);
        Assert.False(answer.RootElement.GetProperty("verified").GetBoolean());
        using var status = JsonDocument.Parse((await deployment.Command("status")).Output);
        Assert.Equal("budget_complete", status.RootElement.GetProperty("run_state").GetString());
        Assert.Equal(JsonValueKind.Null, status.RootElement.GetProperty("pending_intent").ValueKind);
        Assert.Equal(JsonValueKind.Null, status.RootElement.GetProperty("outstanding_state").ValueKind);
    }

    [Fact]
    public async Task ConcurrentOperatorPauseEndsRunAsFailureAndCannotBeConvertedToCompletion()
    {
        using var deployment = installation.Deploy(); ShortPolicy(deployment, 5);
        Assert.Equal(0, (await deployment.Command("initialize")).Code); await deployment.StartDaemon();
        var pending = deployment.Command("run");
        var observed = false;
        for (var attempt = 0; attempt < 20; attempt++)
        {
            if (pending.IsCompleted) break;
            using var status = JsonDocument.Parse((await deployment.Command("status")).Output);
            if (status.RootElement.GetProperty("run_state").GetString() == "running") { observed = true; break; }
            await Task.Delay(25);
        }
        Assert.True(observed, "LEARNING_RUN_COMMAND_RED running state was never visible");
        Assert.Equal(0, (await deployment.Command("pause")).Code);
        Assert.Equal(2, (await pending).Code);
        using var final = JsonDocument.Parse((await deployment.Command("status")).Output);
        Assert.Equal("failed", final.RootElement.GetProperty("run_state").GetString());
        Assert.Equal("run_paused", final.RootElement.GetProperty("run").GetProperty("LastAction").GetString());
    }
}
