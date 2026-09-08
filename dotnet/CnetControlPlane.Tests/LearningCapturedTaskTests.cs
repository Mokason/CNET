using System.Runtime.Versioning;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using CnetControlPlane.Learning;
using Xunit;
using Xunit.Abstractions;

namespace CnetControlPlane.Tests;

[SupportedOSPlatform("linux")]
public sealed class LearningCapturedTaskTests : IClassFixture<LearningCommandInstallation>
{
    private readonly LearningCommandInstallation installation;
    private readonly ITestOutputHelper output;
    public LearningCapturedTaskTests(LearningCommandInstallation installation, ITestOutputHelper output)
    { this.installation = installation; this.output = output; }
    private static string Hash(byte[] bytes) => Convert.ToHexString(SHA256.HashData(bytes)).ToLowerInvariant();
    private JsonDocument Receipt((int Code, string Output, string Error) result)
    {
        Assert.True(result.Code == 0, "CAPTURE_TASK_NATIVE expected success: " + result.Error + result.Output);
        output.WriteLine(result.Output);
        return JsonDocument.Parse(result.Output);
    }

    [Fact]
    public async Task SyntheticCaptureLinksRealOfflineLearningAndRetainsMissingObservationsWithoutEligibility()
    {
        var repository = LearningTestRepository.RequireBuilt(LearningCommandInstallation.NativeNames);
        var corpus = Path.Combine(repository, "data/unicode17");
        var ids = new[] { "unicode17_upper_latin1", "unicode17_lower_latin1", "ascii_category", "ascii_bidi" };
        const string vocabulary = "ba3fe8b2440a6065c04e714136c1ff742e7e7ab772902ff4558fe3b2699b6984";
        var datasets = string.Join(',', ids.Select(id => id.StartsWith("ascii_", StringComparison.Ordinal)
            ? JsonSerializer.Serialize(new { id, authority = "verified_tool", symbol_vocabulary_sha256 = vocabulary })
            : JsonSerializer.Serialize(new { id, authority = "verified_tool" })));
        var policyText = LearningPolicyTests.Valid.Replace("{\"id\":\"calibration\",\"authority\":\"verified_tool\"}", datasets)
            .Replace("\"tick_seconds\":30", "\"tick_seconds\":1").Replace("\"max_run_seconds\":259200", "\"max_run_seconds\":30");
        using var deployment = installation.DeployTrusted();
        deployment.Put("policy.json", Encoding.UTF8.GetBytes(policyText));
        deployment.Put("UnicodeData-Latin1.txt", File.ReadAllBytes(Path.Combine(corpus, "UnicodeData-Latin1.txt")));
        using var initialized = Receipt(await deployment.Command("initialize"));
        var pins = new Dictionary<string, string>();
        foreach (var id in ids)
        {
            var bytes = File.ReadAllBytes(Path.Combine(corpus, id + (id.StartsWith("ascii_", StringComparison.Ordinal) ? ".symbols.tsv" : ".tsv")));
            deployment.Put(id + ".tsv", bytes);
            using var imported = Receipt(await deployment.Command("import", id, Path.Combine(deployment.Root, id + ".tsv"), Hash(bytes)));
            pins.Add(id, Hash(File.ReadAllBytes(Path.Combine(deployment.Root, "work/data", id + ".tsv"))));
        }
        var config = JsonSerializer.SerializeToUtf8Bytes(new { schema_version = 2, dotnet = LearningCommandInstallation.Dotnet,
            managed_sha256 = deployment.ManagedHash, native_sha256 = Hash(File.ReadAllBytes(Path.Combine(deployment.Root, "runtime.json"))),
            policy_sha256 = Hash(Encoding.UTF8.GetBytes(policyText)), sources = pins });
        deployment.Put("bridge.json", config);
        await deployment.StartDaemon();
        var owner = deployment.Command("run"); // Real bounded owner; never synthesize ticks or renew its budget.
        try
        {
            for (var attempt = 0; attempt < 100; attempt++)
            {
                using var status = Receipt(await deployment.Command("status"));
                if (status.RootElement.GetProperty("run_state").GetString() == "running") break;
                await Task.Delay(20);
            }
            Task<(int Code, string Output, string Error)> Rehearse(string stage) => LearningCommandInstallation.Execute(
                Environment.GetEnvironmentVariable("CNET_TASK_PYTHON") ?? "/usr/bin/python3",
                [Path.Combine(repository, "tools/discord_capture/task_native_rehearsal.py"), deployment.Root, Hash(config), stage], repository, true);
            using var before = Receipt(await Rehearse("before"));
            using (var ledger = LearningLedger.OpenReadOnly(Path.Combine(deployment.Root, "work"), LearningPolicy.Parse(Encoding.UTF8.GetBytes(policyText))))
            {
                Assert.Equal(0, ledger.JobCount);
                Assert.Equal((0L, 0L), ledger.Demand(ids[0], 181));
                Assert.Equal((0L, 0L), ledger.Demand(ids[1], 65));
                Assert.Equal(2, ledger.Experiences(0, 10).Count);
            }
            foreach (var (number, dataset) in new[] { ("1", ids[0]), ("2", ids[1]) })
            {
                var request = before.RootElement.GetProperty("requests").GetProperty(number).GetString()!;
                using var approved = Receipt(await deployment.Command("approve", request, pins[dataset]));
            }
            var settled = false;
            for (var attempt = 0; attempt < 100; attempt++)
            {
                using var status = Receipt(await deployment.Command("status"));
                var row = status.RootElement;
                settled = row.GetProperty("jobs").GetInt32() == 2
                    && row.GetProperty("outstanding_state").ValueKind == JsonValueKind.Null
                    && row.GetProperty("pending_intent").ValueKind == JsonValueKind.Null;
                if (settled) break;
                await Task.Delay(150);
            }
            Assert.True(settled, "CAPTURE_TASK_NATIVE two approved jobs did not settle within the original budget");
            using var after = Receipt(await Rehearse("after"));
            foreach (var dataset in ids[..2])
            {
                using var verified = Receipt(await deployment.Command("verify", dataset));
                Assert.True(verified.RootElement.GetProperty("passed").GetBoolean());
                Assert.Equal(256, verified.RootElement.GetProperty("checked_keys").GetInt32());
            }
            var completed = await owner;
            Assert.True(completed.Code == 0, completed.Error + completed.Output);
            using var terminal = Receipt(await Rehearse("terminal"));
            Assert.True(terminal.RootElement.GetProperty("read_only").GetBoolean());
        }
        finally { await owner; } // Reap the bounded private owner before its fixture is removed.
    }
}
