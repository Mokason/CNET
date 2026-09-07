using System.Runtime.Versioning;
using System.Text;
using System.Text.Json;
using CnetControlPlane.Learning;
using Xunit;

namespace CnetControlPlane.Tests;

[SupportedOSPlatform("linux")]
public sealed class LearningSymbolCommandTests : IClassFixture<LearningCommandInstallation>
{
    private readonly LearningCommandInstallation installation;
    public LearningSymbolCommandTests(LearningCommandInstallation installation) { this.installation = installation; }

    private async Task<LearningCommandDeployment> Start()
    {
        var deployment = installation.Deploy();
        try
        {
            deployment.Put("policy.json", Encoding.UTF8.GetBytes(LearningSymbolFixture.Policy.Replace("\"tick_seconds\":30", "\"tick_seconds\":1")));
            Assert.Equal(0, (await deployment.Command("initialize")).Code);
            deployment.Put("work/data/calibration.tsv", Encoding.ASCII.GetBytes(LearningSymbolFixture.Source));
            await deployment.StartDaemon();
            return deployment;
        }
        catch { deployment.Dispose(); throw; }
    }

    private static async Task ExpectAction(LearningCommandDeployment deployment, string expected)
    {
        // Use the provisioned one-second cadence, never relax probe count or
        // bypass the durable too-soon gate to speed up an acceptance test.
        if (expected is "probation" or "accepted") await Task.Delay(1100);
        var tick = await deployment.Command("tick");
        Assert.True(tick.Code == 0, "LEARNING_SYMBOL_COMMAND_RED tick failed: " + tick.Error);
        using var result = JsonDocument.Parse(tick.Output);
        Assert.Equal(expected, result.RootElement.GetProperty("action").GetString());
    }

    [Fact]
    public async Task LiteralLookupLearnsProbesAndRefreshesLabelsWithoutReassigningDemand()
    {
        using var deployment = await Start();
        var missing = await deployment.Command("lookup", "calibration", "ALPHA");
        Assert.True(missing.Code == 0, "LEARNING_SYMBOL_LOOKUP_RED symbolic command unavailable: " + missing.Error);
        using (var result = JsonDocument.Parse(missing.Output)) Assert.False(result.RootElement.GetProperty("verified").GetBoolean());
        await ExpectAction(deployment, "activated");
        await ExpectAction(deployment, "probation"); await ExpectAction(deployment, "probation"); await ExpectAction(deployment, "accepted");
        var learned = await deployment.Command("lookup", "calibration", "ALPHA");
        Assert.Equal(0, learned.Code);
        using (var result = JsonDocument.Parse(learned.Output))
        {
            Assert.True(result.RootElement.GetProperty("verified").GetBoolean());
            Assert.Equal("first label", result.RootElement.GetProperty("label").GetString());
        }
        var verification = await deployment.Command("verify", "calibration");
        Assert.True(verification.Code == 0, verification.Error);
        using (var result = JsonDocument.Parse(verification.Output))
        {
            Assert.True(result.RootElement.GetProperty("passed").GetBoolean());
            Assert.Equal(2, result.RootElement.GetProperty("correct_symbol_answers").GetInt32());
            Assert.Equal(1, result.RootElement.GetProperty("correct_symbol_abstentions").GetInt32());
            Assert.Equal(254, result.RootElement.GetProperty("correct_abstentions").GetInt32());
        }
        var unknown = await deployment.Command("lookup", "calibration", "MISSING");
        Assert.Equal(0, unknown.Code);
        using (var result = JsonDocument.Parse(unknown.Output)) Assert.False(result.RootElement.GetProperty("verified").GetBoolean());
        Assert.Equal(2, (await deployment.Command("lookup", "calibration", "ALPHA extra")).Code);
        // Same vocabulary keeps ordinal demand stable; independent label edits
        // invalidate the resident source-bound unit and trigger a replacement.
        deployment.Put("work/data/calibration.tsv", Encoding.ASCII.GetBytes(LearningSymbolFixture.Source.Replace("first label", "  updated \\\" label  ")));
        var stale = await deployment.Command("lookup", "calibration", "ALPHA");
        Assert.Equal(0, stale.Code);
        using (var result = JsonDocument.Parse(stale.Output)) Assert.False(result.RootElement.GetProperty("verified").GetBoolean());
        await ExpectAction(deployment, "activated");
        await ExpectAction(deployment, "probation"); await ExpectAction(deployment, "probation"); await ExpectAction(deployment, "accepted");
        var refreshed = await deployment.Command("lookup", "calibration", "ALPHA");
        Assert.Equal(0, refreshed.Code);
        using (var result = JsonDocument.Parse(refreshed.Output)) Assert.Equal("  updated \\\" label  ", result.RootElement.GetProperty("label").GetString());
        // Existing demand must never transfer to a renamed symbol.
        deployment.Put("work/data/calibration.tsv", Encoding.ASCII.GetBytes(LearningSymbolFixture.Source.Replace("ALPHA", "AALPHA")));
        Assert.Equal(2, (await deployment.Command("lookup", "calibration", "AALPHA")).Code);
        await ExpectAction(deployment, "evidence_unavailable");
        using var status = JsonDocument.Parse((await deployment.Command("status")).Output);
        Assert.Equal(2, status.RootElement.GetProperty("jobs").GetInt32());
    }

    [Fact]
    public async Task UnknownSymbolDoesNotCreateOrdinalDemandAndVocabularyDriftDuringProbationRollsBack()
    {
        using var deployment = await Start();
        var unknown = await deployment.Command("lookup", "calibration", "MISSING");
        Assert.True(unknown.Code == 0, "LEARNING_SYMBOL_LOOKUP_RED unknown command unavailable: " + unknown.Error);
        await ExpectAction(deployment, "idle");
        Assert.Equal(0, (await deployment.Command("lookup", "calibration", "BETA")).Code);
        await ExpectAction(deployment, "activated");
        deployment.Put("work/data/calibration.tsv", Encoding.ASCII.GetBytes(LearningSymbolFixture.Source.Replace("BETA", "DELTA")));
        await ExpectAction(deployment, "rolled_back");
    }
}
