using System.Globalization;
using System.Runtime.Versioning;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using CnetControlPlane.Learning;
using Xunit;
using Xunit.Abstractions;

namespace CnetControlPlane.Tests;

[SupportedOSPlatform("linux")]
public sealed class LearningUnicodeWorkloadTests : IClassFixture<LearningCommandInstallation>
{
    private readonly LearningCommandInstallation installation;
    private readonly ITestOutputHelper output;
    public LearningUnicodeWorkloadTests(LearningCommandInstallation installation, ITestOutputHelper output)
    { this.installation = installation; this.output = output; }
    private static string Hash(byte[] bytes) => Convert.ToHexString(SHA256.HashData(bytes)).ToLowerInvariant();

    [Fact]
    public async Task ThreeTruthfulSourceStagesReplaceTwoTablesWhileSymbolicCapsulesRemainVerified()
    {
        var repository = LearningTestRepository.RequireBuilt(LearningCommandInstallation.NativeNames);
        var corpus = Path.Combine(repository, "data/unicode17");
        const string vocabulary = "ba3fe8b2440a6065c04e714136c1ff742e7e7ab772902ff4558fe3b2699b6984";
        var ids = new[] { "unicode17_upper_latin1", "unicode17_lower_latin1", "ascii_category", "ascii_bidi" };
        var datasets = string.Join(',', ids.Select(id => id.StartsWith("ascii_", StringComparison.Ordinal)
            ? JsonSerializer.Serialize(new { id, authority = "verified_tool", symbol_vocabulary_sha256 = vocabulary })
            : JsonSerializer.Serialize(new { id, authority = "verified_tool" })));
        var policy = LearningPolicyTests.Valid.Replace("{\"id\":\"calibration\",\"authority\":\"verified_tool\"}", datasets)
            .Replace("\"tick_seconds\":30", "\"tick_seconds\":1")
            .Replace("\"attempts_per_hour\":4", "\"attempts_per_hour\":8");
        using var deployment = installation.Deploy();
        deployment.Put("policy.json", Encoding.UTF8.GetBytes(policy));
        Assert.Equal(0, (await deployment.Command("initialize")).Code);
        await deployment.StartDaemon();
        ulong previousRevision = 0;
        for (var stage = 0; stage < 3; stage++)
        {
            foreach (var id in stage == 0 ? ids : ids[..2])
            {
                var symbolic = id.StartsWith("ascii_", StringComparison.Ordinal);
                var lines = File.ReadAllLines(Path.Combine(corpus, id + (symbolic ? ".symbols.tsv" : ".tsv")));
                var rows = lines.Skip(6).Take(symbolic || stage == 2 ? 256 : stage == 0 ? 16 : 32).ToArray();
                lines[5] = "rows " + rows.Length.ToString(CultureInfo.InvariantCulture);
                var bytes = Encoding.ASCII.GetBytes(string.Join('\n', lines.Take(6).Concat(rows)) + "\n");
                // Explicit synthetic operator refresh, not real elapsed observation.
                deployment.Put("work/data/" + id + ".tsv", bytes);
                var demand = rows[^1].Split('\t')[0];
                var asked = await deployment.Command(symbolic ? "lookup" : "ask", id, demand);
                Assert.True(asked.Code == 0, "UNICODE_SOAK_STAGES_RED demand: " + asked.Error);
                using var answer = JsonDocument.Parse(asked.Output);
                Assert.False(answer.RootElement.GetProperty("verified").GetBoolean());
            }
            var settled = false;
            for (var attempt = 0; attempt < 60; attempt++)
            {
                var tick = await deployment.Command("tick");
                Assert.True(tick.Code == 0, "UNICODE_SOAK_STAGES_RED tick: " + tick.Error);
                var status = await deployment.Command("status");
                Assert.Equal(0, status.Code);
                using var value = JsonDocument.Parse(status.Output);
                var row = value.RootElement;
                Assert.True(row.GetProperty("jobs").GetInt32() <= 4 + 2 * stage, "UNICODE_SOAK_STAGES_RED unexpected retry");
                settled = row.GetProperty("jobs").GetInt32() == 4 + 2 * stage
                    && row.GetProperty("outstanding_state").ValueKind == JsonValueKind.Null
                    && row.GetProperty("pending_intent").ValueKind == JsonValueKind.Null;
                if (settled) break;
                await Task.Delay(1000);
            }
            Assert.True(settled, "UNICODE_SOAK_STAGES_RED stage deadline");
            ulong? revision = null;
            foreach (var id in ids)
            {
                var verified = await deployment.Command("verify", id);
                Assert.True(verified.Code == 0, "UNICODE_SOAK_STAGES_RED full verification: " + verified.Output + verified.Error);
                output.WriteLine(verified.Output);
                using var receipt = JsonDocument.Parse(verified.Output);
                var current = receipt.RootElement.GetProperty("revision").GetUInt64();
                revision ??= current;
                Assert.Equal(revision.Value, current);
            }
            Assert.True(revision!.Value > previousRevision);
            previousRevision = revision.Value;
        }
    }

    [Fact]
    public async Task OfficialExplicitCaseChangesAreAcquiredAcceptedAndCoexistWithExactAbstention()
    {
        var repository = LearningTestRepository.RequireBuilt(LearningCommandInstallation.NativeNames);
        var corpus = Path.Combine(repository, "data/unicode17");
        var rawPath = Path.Combine(corpus, "UnicodeData-Latin1.txt");
        Assert.True(File.Exists(rawPath), "UNICODE17_LEARNING_RED pinned real-source workload missing");
        var raw = File.ReadAllBytes(rawPath);
        Assert.Equal("75dfecc13fe9b1202e3f7c787e4e7f2c848c97c8b092dd75b4f6a2b99990cdc4", Hash(raw));
        var rows = Encoding.ASCII.GetString(raw).Split('\n', StringSplitOptions.RemoveEmptyEntries)
            .Select(line => line.Split(';')).ToArray();
        Assert.Equal(256, rows.Length);
        for (var key = 0; key < 256; key++)
        {
            Assert.Equal(15, rows[key].Length);
            Assert.Equal(key.ToString("X4", CultureInfo.InvariantCulture), rows[key][0]);
        }
        using var deployment = installation.Deploy();
        var policy = LearningPolicyTests.Valid.Replace("\"id\":\"calibration\",\"authority\":\"verified_tool\"",
            "\"id\":\"unicode17_upper_latin1\",\"authority\":\"verified_tool\"},{\"id\":\"unicode17_lower_latin1\",\"authority\":\"verified_tool\"")
            .Replace("\"tick_seconds\":30", "\"tick_seconds\":1")
            .Replace("\"max_run_seconds\":259200", "\"max_run_seconds\":30");
        deployment.Put("policy.json", Encoding.UTF8.GetBytes(policy));
        Assert.Equal(0, (await deployment.Command("initialize")).Code);
        await deployment.StartDaemon();
        var references = new List<LocalTableReference>();
        foreach (var (kind, field, count, demand) in new[] { ("upper", 12, 58, "97"), ("lower", 13, 56, "65") })
        {
            var id = "unicode17_" + kind + "_latin1";
            // Independent extraction: never call the Python converter or runtime Unicode casing.
            var expected = rows.Where(row => row[field].Length != 0).Select(row =>
                int.Parse(row[0], NumberStyles.HexNumber, CultureInfo.InvariantCulture).ToString(CultureInfo.InvariantCulture) + "\t" +
                int.Parse(row[field], NumberStyles.HexNumber, CultureInfo.InvariantCulture).ToString(CultureInfo.InvariantCulture)).ToArray();
            Assert.Equal(count, expected.Length);
            var independent = Encoding.ASCII.GetBytes($"CNET_LOCAL_TABLE_V1\ndataset {id}\nauthority verified_tool\ninput_bits 8\noutput_bits 16\nrows {count}\n" + string.Join('\n', expected) + "\n");
            var approved = File.ReadAllBytes(Path.Combine(corpus, id + ".tsv"));
            Assert.Equal(independent, approved);
            references.Add(LocalTableReference.Parse(independent, new(id, "verified_tool")));
            if (kind == "upper")
            {
                Assert.Equal((ushort)924, references[^1].ExpectedFor(181)); // Micro sign -> Greek capital mu.
                Assert.Equal((ushort)376, references[^1].ExpectedFor(255)); // Y with diaeresis crosses Latin-1.
            }
            Assert.Null(references[^1].ExpectedFor(223)); // No explicit simple change for sharp-s.
            deployment.Put(id + ".tsv", approved);
            var imported = await deployment.Command("import", id, Path.Combine(deployment.Root, id + ".tsv"), Hash(approved));
            Assert.True(imported.Code == 0, "UNICODE17_LEARNING_RED import: " + imported.Error);
            output.WriteLine(imported.Output);
            var before = await deployment.Command("verify", id);
            Assert.Equal(2, before.Code);
            using (var missing = JsonDocument.Parse(before.Output))
            {
                Assert.Equal(count, missing.RootElement.GetProperty("missing_answers").GetInt32());
                Assert.Equal(0, missing.RootElement.GetProperty("wrong_answers").GetInt32());
            }
            var asked = await deployment.Command("ask", id, demand);
            Assert.Equal(0, asked.Code);
            using var answer = JsonDocument.Parse(asked.Output);
            Assert.False(answer.RootElement.GetProperty("verified").GetBoolean());
        }
        var run = await deployment.Command("run");
        output.WriteLine(run.Output);
        Assert.True(run.Code == 0, "UNICODE17_LEARNING_RED autonomous run: " + run.Error);
        var actions = run.Output.Split('\n', StringSplitOptions.RemoveEmptyEntries).Select(line =>
        {
            using var entry = JsonDocument.Parse(line);
            return entry.RootElement.TryGetProperty("action", out var action) ? action.GetString() : null;
        }).ToArray();
        Assert.Equal(2, actions.Count(action => action == "accepted"));
        var status = await deployment.Command("status");
        Assert.Equal(0, status.Code); output.WriteLine(status.Output);
        using (var settled = JsonDocument.Parse(status.Output))
        {
            Assert.Equal("budget_complete", settled.RootElement.GetProperty("run_state").GetString());
            Assert.Equal(2, settled.RootElement.GetProperty("jobs").GetInt64());
            Assert.Equal(JsonValueKind.Null, settled.RootElement.GetProperty("outstanding_state").ValueKind);
            Assert.Equal(JsonValueKind.Null, settled.RootElement.GetProperty("pending_intent").ValueKind);
        }
        using var runtime = LearningRuntime.Load(Path.Combine(deployment.Root, "native"), File.ReadAllBytes(Path.Combine(deployment.Root, "runtime.json")));
        var control = new LearningControlClient(runtime, Path.Combine(deployment.Root, "ipc/control.sock"), 30);
        using var client = new LearningAskClient(Path.Combine(deployment.Root, "ipc/ask.sock"), 30);
        var finalGeneration = await control.StatusAsync(default);
        foreach (var reference in references)
        {
            var verified = await deployment.Command("verify", reference.Dataset);
            Assert.True(verified.Code == 0, "UNICODE17_LEARNING_RED final live sweep: " + verified.Error);
            output.WriteLine(verified.Output);
            using var receipt = JsonDocument.Parse(verified.Output);
            Assert.Equal(finalGeneration.Active, receipt.RootElement.GetProperty("active_sha256").GetString());
            Assert.Equal(finalGeneration.Revision, receipt.RootElement.GetProperty("revision").GetUInt64());
            var dataset = new LearningDataset(reference.Dataset, "verified_tool");
            var independent = await LearningLiveVerification.ObserveAsync(dataset, () => reference, control.StatusAsync,
                (key, stop) => client.AskAsync(dataset, key, stop), 30, new LearningClock());
            Assert.True(independent.Passed);
            Assert.Equal(reference.Values.Count, independent.CorrectAnswers);
            Assert.Equal(256 - reference.Values.Count, independent.CorrectAbstentions);
            Assert.Equal(finalGeneration.Active, independent.ActiveSha256);
            Assert.Equal(finalGeneration.Revision, independent.Revision);
            output.WriteLine(JsonSerializer.Serialize(new { @event = "unicode17_independent_observation", dataset = reference.Dataset,
                contract = "explicit_case_change_only_empty_field_abstains", raw_sha256 = Hash(raw), observation = independent }));
        }
        Assert.Equal(finalGeneration, await control.StatusAsync(default));
    }
}
