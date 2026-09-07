using System.Globalization;
using System.Runtime.Versioning;
using System.Text;
using System.Text.Json;
using CnetControlPlane.Learning;
using Xunit;
using Xunit.Abstractions;

namespace CnetControlPlane.Tests;

[SupportedOSPlatform("linux")]
public sealed class LearningSymbolWorkloadTests : IClassFixture<LearningCommandInstallation>
{
    private readonly LearningCommandInstallation installation;
    private readonly ITestOutputHelper output;
    public LearningSymbolWorkloadTests(LearningCommandInstallation installation, ITestOutputHelper output)
    { this.installation = installation; this.output = output; }

    [Fact]
    public async Task PinnedUnicodeNamesLearnTwoLiteralLabelCapabilitiesAndCoexistAfterBoundedRun()
    {
        var repository = LearningTestRepository.RequireBuilt(LearningCommandInstallation.NativeNames);
        var corpus = Path.Combine(repository, "data/unicode17");
        var raw = File.ReadAllText(Path.Combine(corpus, "UnicodeData-Latin1.txt"), Encoding.ASCII);
        Assert.Equal("75dfecc13fe9b1202e3f7c787e4e7f2c848c97c8b092dd75b4f6a2b99990cdc4", LearningSymbolFixture.Hash(raw));
        // Independent extraction, not the production decoder, generator or
        // platform Unicode database. Both fields are pinned upstream facts.
        var rows = raw.Split('\n', StringSplitOptions.RemoveEmptyEntries).Select(line => line.Split(';'))
            .Where(row => int.Parse(row[0], NumberStyles.HexNumber, CultureInfo.InvariantCulture) is >= 32 and <= 126)
            .OrderBy(row => row[1].Replace(' ', '_'), StringComparer.Ordinal).ToArray();
        Assert.Equal(95, rows.Length);
        var vocabulary = LearningSymbolFixture.Hash(string.Join('\n', rows.Select(row => row[1].Replace(' ', '_'))) + "\n");
        Assert.Equal("ba3fe8b2440a6065c04e714136c1ff742e7e7ab772902ff4558fe3b2699b6984", vocabulary);
        var datasets = new[] { new LearningDataset("ascii_category", "verified_tool", vocabulary), new LearningDataset("ascii_bidi", "verified_tool", vocabulary) };
        var declared = string.Join(',', datasets.Select(dataset => JsonSerializer.Serialize(new
            { id = dataset.Id, authority = dataset.Authority, symbol_vocabulary_sha256 = vocabulary })));
        var policy = LearningPolicyTests.Valid.Replace("{\"id\":\"calibration\",\"authority\":\"verified_tool\"}", declared)
            .Replace("\"tick_seconds\":30", "\"tick_seconds\":1").Replace("\"max_run_seconds\":259200", "\"max_run_seconds\":30");
        using var deployment = installation.Deploy();
        deployment.Put("policy.json", Encoding.UTF8.GetBytes(policy));
        Assert.Equal(0, (await deployment.Command("initialize")).Code);
        await deployment.StartDaemon();
        foreach (var dataset in datasets)
        {
            var field = dataset.Id == "ascii_category" ? 2 : 4;
            var independent = $"CNET_LOCAL_SYMBOLS_V1\ndataset {dataset.Id}\nauthority verified_tool\ninput_bits 8\noutput_bits 16\nrows 95\n"
                + string.Join('\n', rows.Select(row => row[1].Replace(' ', '_') + "\t" + row[field])) + "\n";
            var approved = File.ReadAllText(Path.Combine(corpus, dataset.Id + ".symbols.tsv"), Encoding.ASCII);
            Assert.Equal(independent, approved);
            deployment.Put(dataset.Id + ".tsv", Encoding.ASCII.GetBytes(approved));
            var imported = await deployment.Command("import", dataset.Id, Path.Combine(deployment.Root, dataset.Id + ".tsv"), LearningSymbolFixture.Hash(approved));
            Assert.True(imported.Code == 0, "UNICODE17_SYMBOL_WORKLOAD_RED source import: " + imported.Error);
            output.WriteLine(imported.Output);
            var before = await deployment.Command("verify", dataset.Id);
            Assert.Equal(2, before.Code);
            using (var receipt = JsonDocument.Parse(before.Output))
            {
                Assert.Equal(95, receipt.RootElement.GetProperty("missing_answers").GetInt32());
                Assert.Equal(95, receipt.RootElement.GetProperty("missing_symbol_answers").GetInt32());
            }
            var miss = await deployment.Command("lookup", dataset.Id, "LATIN_CAPITAL_LETTER_A");
            Assert.Equal(0, miss.Code);
            using var answer = JsonDocument.Parse(miss.Output); Assert.False(answer.RootElement.GetProperty("verified").GetBoolean());
        }
        var run = await deployment.Command("run"); output.WriteLine(run.Output);
        Assert.True(run.Code == 0, "UNICODE17_SYMBOL_WORKLOAD_RED run: " + run.Error);
        var actions = run.Output.Split('\n', StringSplitOptions.RemoveEmptyEntries).Select(line =>
        {
            using var json = JsonDocument.Parse(line);
            return json.RootElement.TryGetProperty("action", out var action) ? action.GetString() : null;
        }).ToArray();
        Assert.Equal(2, actions.Count(action => action == "accepted"));
        string? generation = null;
        foreach (var dataset in datasets)
        {
            var verified = await deployment.Command("verify", dataset.Id); output.WriteLine(verified.Output);
            Assert.True(verified.Code == 0, "UNICODE17_SYMBOL_WORKLOAD_RED live check: " + verified.Error);
            using var receipt = JsonDocument.Parse(verified.Output); var result = receipt.RootElement;
            Assert.True(result.GetProperty("passed").GetBoolean());
            Assert.Equal(95, result.GetProperty("correct_answers").GetInt32());
            Assert.Equal(161, result.GetProperty("correct_abstentions").GetInt32());
            Assert.Equal(95, result.GetProperty("correct_symbol_answers").GetInt32());
            Assert.Equal(1, result.GetProperty("correct_symbol_abstentions").GetInt32());
            Assert.Equal(0, result.GetProperty("wrong_symbol_answers").GetInt32());
            generation ??= result.GetProperty("active_sha256").GetString();
            Assert.Equal(generation, result.GetProperty("active_sha256").GetString());
            var known = await deployment.Command("lookup", dataset.Id, "HYPHEN-MINUS");
            Assert.Equal(0, known.Code);
            using var literal = JsonDocument.Parse(known.Output);
            Assert.Equal(dataset.Id == "ascii_category" ? "Pd" : "ES", literal.RootElement.GetProperty("label").GetString());
        }
        var status = await deployment.Command("status"); output.WriteLine(status.Output);
        Assert.Equal(0, status.Code);
        using var final = JsonDocument.Parse(status.Output);
        Assert.Equal("budget_complete", final.RootElement.GetProperty("run_state").GetString());
        Assert.Equal(2, final.RootElement.GetProperty("jobs").GetInt32());
        Assert.Equal(JsonValueKind.Null, final.RootElement.GetProperty("outstanding_state").ValueKind);
        Assert.Equal(JsonValueKind.Null, final.RootElement.GetProperty("pending_intent").ValueKind);
    }
}
