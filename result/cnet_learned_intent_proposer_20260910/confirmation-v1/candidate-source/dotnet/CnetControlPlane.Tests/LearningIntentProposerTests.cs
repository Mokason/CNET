using System.Text.Json;
using CnetControlPlane.Learning;
using Xunit;

namespace CnetControlPlane.Tests;

// Development evidence for an offline learned proposer. Training data is an
// answer-free external spec, never CNET capsule output. Fresh confirmation
// remains WITHHELD. Evaluation wording is a separate family from training.
public sealed class LearningIntentProposerTests
{
    const string UnseenReady = "Kindly recode the glyph 'P' toward small letters.";
    const string UnseenExtra = "Kindly recode the glyph 'P' toward small letters and email it.";
    const string UnseenChoice = "Kindly recode the glyph F or G toward small letters.";
    const string UnseenNegated = "Do not recode the glyph 'P' toward small letters.";
    const string UnseenControl = "Kindly recode the glyph U+000A toward small letters.";
    const string MixedDomain = "Kindly recode the glyph 'P' or U+000A toward small letters.";
    const string MixedFarDomain = "Kindly recode the glyph 'P' or U+0218 toward small letters.";
    const string ExtraClause = "Kindly recode the glyph 'P' toward small letters; erase it.";
    const string ExtraAction = "Kindly recode the glyph 'P' toward small letters, erase it.";
    const string LocaleQualifier = "Kindly recode the glyph 'P' toward small letters under Azeri locale.";

    [Fact]
    public void ProductionGrammarTablesDoNotContainTheHeldOutFrame()
    {
        var root = RepoFile("dotnet/CnetControlPlane/Learning");
        foreach (var name in new[] { "LearningTaskProposal.cs", "LearningCaseRequestSyntax.cs" })
        {
            var text = File.ReadAllText(Path.Combine(root, name));
            Assert.DoesNotContain("recode", text, StringComparison.OrdinalIgnoreCase);
            Assert.DoesNotContain("glyph", text, StringComparison.OrdinalIgnoreCase);
            Assert.DoesNotContain("toward small", text, StringComparison.OrdinalIgnoreCase);
        }
    }

    [Fact]
    public void TrainingSpecDoesNotContainEvaluationWording()
    {
        foreach (var example in LearningIntentSpec.TrainCorpus())
            Assert.False(example.Text.Equals(UnseenReady, StringComparison.Ordinal),
                "TASK_INTENT_RED evaluation sentence leaked into training");
        foreach (var example in LearningIntentSpec.CalibrationCorpus())
            Assert.False(example.Text.Contains("Kindly recode the glyph 'P'", StringComparison.Ordinal),
                "TASK_INTENT_RED Kindly+'P' family leaked into calibration");
    }

    [Fact]
    public void TrainAndCalibrationFamiliesHaveNoNormalizedOverlap()
    {
        var train = LearningIntentSpec.TrainCorpus().Select(example => LearningIntentSpec.Normalize(example.Text)).ToHashSet(StringComparer.Ordinal);
        foreach (var example in LearningIntentSpec.CalibrationCorpus())
        {
            var normalized = LearningIntentSpec.Normalize(example.Text);
            Assert.False(train.Contains(normalized), $"TASK_INTENT_RED overlapping family {example.Text}");
        }
    }

    [Fact]
    public void HeldOutFrameIsReadyWithoutChangingTheScalar()
    {
        var proposal = LearningTaskParser.Propose(UnseenReady);
        Assert.True(proposal.Status == "ready", $"TASK_INTENT_RED {UnseenReady}: {proposal.Status}");
        Assert.Equal("unicode17_lower_latin1", proposal.Dataset);
        Assert.Equal((byte)80, proposal.Key);
    }

    [Theory]
    [InlineData(UnseenExtra, "abstain")]
    [InlineData(UnseenChoice, "clarify")]
    [InlineData(UnseenNegated, "abstain")]
    [InlineData(UnseenControl, "abstain")]
    [InlineData(MixedDomain, "abstain")]
    [InlineData(MixedFarDomain, "abstain")]
    [InlineData(ExtraClause, "abstain")]
    [InlineData(ExtraAction, "abstain")]
    [InlineData(LocaleQualifier, "abstain")]
    [InlineData("Raise 'a' after converting it to lowercase.", "abstain")]
    public void HeldOutSafetyCasesNeverBecomeReady(string text, string expected)
    {
        var proposal = LearningTaskParser.Propose(text);
        Assert.True(proposal.Status == expected, $"TASK_INTENT_RED {text}: {proposal.Status}, expected {expected}");
        Assert.Null(proposal.Dataset);
        Assert.Null(proposal.Key);
    }

    [Fact]
    public void CalibrationSplitCountsActualReadyPredictions()
    {
        var report = LearningIntentProposer.Report;
        Assert.True(report.CalibrationWrongReady == 0, $"TASK_INTENT_RED wrong ready {report.CalibrationWrongReady}");
        Assert.True(report.CalibrationReady >= 8, $"TASK_INTENT_RED predicted-ready {report.CalibrationReady}");
        Assert.True(report.TrainExamples >= 200, $"TASK_INTENT_RED train {report.TrainExamples}");
    }

    [Fact]
    public void ExposedConfirmationRemainsZeroWrongReady()
    {
        var path = RepoFile("result/cnet_herdr_three_heads_20260909/exposed-corpora/hermes-third.json");
        using var doc = JsonDocument.Parse(File.ReadAllText(path));
        var wrong = 0;
        var exact = 0;
        var total = 0;
        foreach (var row in doc.RootElement.GetProperty("cases").EnumerateArray())
        {
            total++;
            var text = row.GetProperty("text").GetString()!;
            var status = row.GetProperty("status").GetString()!;
            var proposal = LearningTaskParser.Propose(text);
            if (proposal.Status == "ready" && status != "ready") wrong++;
            if (proposal.Status == "ready" && status == "ready")
            {
                var operation = row.GetProperty("operation").GetString();
                var key = row.GetProperty("key").GetByte();
                if ((operation == "lower" && proposal.Dataset == "unicode17_lower_latin1"
                    || operation == "upper" && proposal.Dataset == "unicode17_upper_latin1")
                    && proposal.Key == key) exact++;
                else wrong++;
            }
            else if (proposal.Status == status && status != "ready") exact++;
        }
        Assert.True(wrong == 0, $"TASK_INTENT_RED exposed wrong ready {wrong}/{total}, exact {exact}");
    }

    static string RepoFile(string relative)
    {
        for (var dir = new DirectoryInfo(AppContext.BaseDirectory); dir is not null; dir = dir.Parent)
        {
            var candidate = Path.Combine(dir.FullName, relative);
            if (Directory.Exists(candidate) || File.Exists(candidate)) return candidate;
        }
        throw new DirectoryNotFoundException(relative);
    }
}
