using System.Runtime.Versioning;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using CnetControlPlane.Learning;
using Xunit;

namespace CnetControlPlane.Tests;

[SupportedOSPlatform("linux")]
public sealed class LearningTaskCommandTests : IClassFixture<LearningCommandInstallation>
{
    private readonly LearningCommandInstallation installation;
    public LearningTaskCommandTests(LearningCommandInstallation installation) => this.installation = installation;
    private const string Request = "11111111111111111111111111111111";
    private const string Source = "CNET_LOCAL_TABLE_V1\ndataset calibration\nauthority verified_tool\ninput_bits 8\noutput_bits 16\nrows 2\n0\t0\n7\t42\n";
    private static JsonDocument Receipt((int Code, string Output, string Error) result)
    {
        Assert.True(result.Code == 0, "TASK_EXPERIENCE_RED expected supported task command: " + result.Error);
        return JsonDocument.Parse(result.Output);
    }

    [Fact]
    public async Task ObserveDoesNotLearnAndApprovedExternalCorrectionCreatesDemandExactlyOnce()
    {
        using var deployment = installation.Deploy();
        using var initialized = Receipt(await deployment.Command("initialize"));
        await deployment.StartDaemon();
        using var miss = Receipt(await deployment.Command("observe", "calibration", "7", "synthetic", Request));
        Assert.Equal("awaiting_evidence", miss.RootElement.GetProperty("experience").GetProperty("State").GetString());
        using (var work = LearningLedger.Open(Path.Combine(deployment.Root, "work"),
                   LearningPolicy.Parse(Encoding.UTF8.GetBytes(LearningPolicyTests.Valid)), new LearningClock()))
            Assert.Equal((0L, 0L), work.Demand("calibration", 7));
        using var idle = Receipt(await deployment.Command("tick"));
        Assert.Equal("idle", idle.RootElement.GetProperty("action").GetString());
        using var replay = Receipt(await deployment.Command("observe", "calibration", "7", "synthetic", Request));
        Assert.True(replay.RootElement.GetProperty("replayed").GetBoolean());
        Assert.Equal(2, (await deployment.Command("observe", "calibration", "8", "synthetic", Request)).Code);
        var bytes = Encoding.ASCII.GetBytes(Source);
        var hash = Convert.ToHexString(SHA256.HashData(bytes)).ToLowerInvariant();
        deployment.Put("approved.tsv", bytes);
        using var imported = Receipt(await deployment.Command("import", "calibration", Path.Combine(deployment.Root, "approved.tsv"), hash));
        Assert.Equal(2, (await deployment.Command("approve", Request, new string('0', 64))).Code);
        using var approval = Receipt(await deployment.Command("approve", Request, hash));
        using var duplicate = Receipt(await deployment.Command("approve", Request, hash));
        using (var work = LearningLedger.Open(Path.Combine(deployment.Root, "work"),
                   LearningPolicy.Parse(Encoding.UTF8.GetBytes(LearningPolicyTests.Valid)), new LearningClock()))
            Assert.Equal((1L, 1L), work.Demand("calibration", 7));
        using var acquired = Receipt(await deployment.Command("tick"));
        Assert.Equal("activated", acquired.RootElement.GetProperty("action").GetString());
        using var verified = Receipt(await deployment.Command("verify", "calibration"));
        using var answer = Receipt(await deployment.Command("observe", "calibration", "7", "synthetic", new string('2', 32)));
        var experience = answer.RootElement.GetProperty("experience");
        Assert.Equal("verified", experience.GetProperty("State").GetString());
        Assert.Equal(42, experience.GetProperty("Value").GetInt32());
        Assert.Equal(42, experience.GetProperty("Expected").GetInt32());
        Assert.Equal(hash, experience.GetProperty("SourceSha256").GetString());
        using var inbox = Receipt(await deployment.Command("inbox", "0", "10"));
        Assert.Equal(2, inbox.RootElement.GetProperty("experiences").GetArrayLength());
    }
}
