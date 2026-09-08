using System.Runtime.Versioning;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using CnetControlPlane.Learning;
using Xunit;

namespace CnetControlPlane.Tests;

[SupportedOSPlatform("linux")]
public sealed class LearningNaturalTaskCommandTests : IClassFixture<LearningCommandInstallation>
{
    private readonly LearningCommandInstallation installation;
    public LearningNaturalTaskCommandTests(LearningCommandInstallation installation) => this.installation = installation;
    private static JsonDocument Receipt((int Code, string Output, string Error) result)
    {
        Assert.True(result.Code == 0, "NATURAL_TASK_RED supported bounded request: " + result.Error);
        return JsonDocument.Parse(result.Output);
    }
    private static string Id(char value) => new(value, 32);
    private static string Hash(byte[] bytes) => Convert.ToHexString(SHA256.HashData(bytes)).ToLowerInvariant();

    [Fact]
    public async Task ApprovedMicroSignCorrectionEnablesDifferentPhrasingWithoutLosingLowercase()
    {
        var repository = LearningTestRepository.RequireBuilt(LearningCommandInstallation.NativeNames);
        var corpus = Path.Combine(repository, "data/unicode17");
        Assert.Equal("75dfecc13fe9b1202e3f7c787e4e7f2c848c97c8b092dd75b4f6a2b99990cdc4",
            Hash(File.ReadAllBytes(Path.Combine(corpus, "UnicodeData-Latin1.txt"))));
        using var deployment = installation.Deploy();
        var policyText = LearningPolicyTests.Valid.Replace("\"id\":\"calibration\",\"authority\":\"verified_tool\"",
            "\"id\":\"unicode17_upper_latin1\",\"authority\":\"verified_tool\"},{\"id\":\"unicode17_lower_latin1\",\"authority\":\"verified_tool\"")
            .Replace("\"tick_seconds\":30", "\"tick_seconds\":1");
        deployment.Put("policy.json", Encoding.UTF8.GetBytes(policyText));
        using var initialized = Receipt(await deployment.Command("initialize"));
        // Clarification and OOD refusal do not require a native daemon or create observations.
        using var clarification = Receipt(await deployment.Command("task", "synthetic", Id('a'), "uppercase 65"));
        Assert.Equal("clarify", clarification.RootElement.GetProperty("proposal").GetProperty("Status").GetString());
        using var unrelated = Receipt(await deployment.Command("task", "synthetic", Id('b'), "What is tomorrow's weather?"));
        Assert.Equal("abstain", unrelated.RootElement.GetProperty("proposal").GetProperty("Status").GetString());
        using var empty = Receipt(await deployment.Command("inbox", "0", "100"));
        Assert.Equal(0, empty.RootElement.GetProperty("experiences").GetArrayLength());
        await deployment.StartDaemon(); // Teacher and self-answer disabled by the private fixture.
        using var initial = Receipt(await deployment.Command("task", "synthetic", Id('c'), "What's the uppercase of µ?"));
        Assert.Equal("awaiting_evidence", initial.RootElement.GetProperty("experience").GetProperty("State").GetString());
        using var idle = Receipt(await deployment.Command("tick"));
        Assert.Equal("idle", idle.RootElement.GetProperty("action").GetString());

        foreach (var (kind, request, text) in new[] { ("lower", Id('d'), "convert 'A' to lowercase"), ("upper", Id('c'), "What's the uppercase of µ?") })
        {
            var dataset = "unicode17_" + kind + "_latin1";
            var approved = File.ReadAllBytes(Path.Combine(corpus, dataset + ".tsv"));
            if (kind == "lower")
            {
                using var gap = Receipt(await deployment.Command("task", "synthetic", request, text));
                Assert.Equal("awaiting_evidence", gap.RootElement.GetProperty("experience").GetProperty("State").GetString());
            }
            deployment.Put(dataset + ".tsv", approved);
            using var imported = Receipt(await deployment.Command("import", dataset, Path.Combine(deployment.Root, dataset + ".tsv"), Hash(approved)));
            using var approval = Receipt(await deployment.Command("approve", request, Hash(approved)));
            var accepted = false;
            for (var attempt = 0; attempt < 30; attempt++)
            {
                using var tick = Receipt(await deployment.Command("tick"));
                if (tick.RootElement.GetProperty("action").GetString() == "accepted") { accepted = true; break; }
                await Task.Delay(1000);
            }
            Assert.True(accepted, "NATURAL_TASK_RED capsule probation did not settle");
            using var verified = Receipt(await deployment.Command("verify", dataset));
            Assert.True(verified.RootElement.GetProperty("passed").GetBoolean());
        }
        using var learned = Receipt(await deployment.Command("task", "synthetic", Id('e'), "Please convert 'µ' to uppercase."));
        var result = learned.RootElement.GetProperty("experience");
        Assert.Equal("verified", result.GetProperty("State").GetString());
        Assert.Equal(924, result.GetProperty("Value").GetInt32());
        Assert.Equal(924, result.GetProperty("Expected").GetInt32());
        using var preserved = Receipt(await deployment.Command("task", "synthetic", Id('f'), "What is the lowercase of A?"));
        Assert.Equal(97, preserved.RootElement.GetProperty("experience").GetProperty("Value").GetInt32());
        using var uncovered = Receipt(await deployment.Command("task", "synthetic", Id('1'), "uppercase ß"));
        Assert.Equal("abstain", uncovered.RootElement.GetProperty("experience").GetProperty("State").GetString());
        foreach (var dataset in new[] { "unicode17_upper_latin1", "unicode17_lower_latin1" })
        {
            using var verified = Receipt(await deployment.Command("verify", dataset));
            Assert.Equal(256, verified.RootElement.GetProperty("checked_keys").GetInt32());
            Assert.True(verified.RootElement.GetProperty("passed").GetBoolean());
        }
        using var work = LearningLedger.Open(Path.Combine(deployment.Root, "work"),
            LearningPolicy.Parse(Encoding.UTF8.GetBytes(policyText)), new LearningClock());
        Assert.Equal(2, work.JobCount);
        Assert.All(work.Experiences(0, 100), experience => Assert.Equal("synthetic", experience.Origin));
    }
}
