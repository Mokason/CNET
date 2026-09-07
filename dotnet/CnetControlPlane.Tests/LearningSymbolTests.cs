using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using CnetControlPlane.Learning;
using Xunit;

namespace CnetControlPlane.Tests;

internal static class LearningSymbolFixture
{
    internal const string Source = "CNET_LOCAL_SYMBOLS_V1\ndataset calibration\nauthority verified_tool\ninput_bits 8\noutput_bits 16\nrows 2\nALPHA\tfirst label\nBETA\tsecond label\n";
    internal static string Hash(string text) => Convert.ToHexString(SHA256.HashData(Encoding.ASCII.GetBytes(text))).ToLowerInvariant();
    internal static string Vocabulary => Hash("ALPHA\nBETA\n");
    internal static string Policy => LearningPolicyTests.Valid.Replace("\"authority\":\"verified_tool\"",
        "\"authority\":\"verified_tool\",\"symbol_vocabulary_sha256\":\"" + Vocabulary + "\"");
    internal static LearningDataset Dataset => LearningPolicy.Parse(Encoding.UTF8.GetBytes(Policy)).Datasets.Single();
    internal static LocalTableReference Reference(string source = Source) => LocalTableReference.Parse(Encoding.ASCII.GetBytes(source), Dataset);
    internal static byte[] Evaluation(LocalTableReference reference, string snapshot)
    {
        var text = new StringBuilder("CNET_SYMBOL_SNAPSHOT_EVAL_V1\nsnapshot_sha256 " + snapshot + "\ndataset " + reference.Dataset
            + "\nsource_sha256 " + reference.SourceSha256 + "\nresults 256\n");
        for (var key = 0; key < 256; key++) text.Append(key).Append(reference.ExpectedFor((byte)key) is { } value ? "\t1\t" + value : "\t0\t-").Append('\n');
        text.Append("symbols ").Append(reference.Symbols!.Keys.Count).Append('\n');
        for (var row = 0; row < reference.Symbols.Keys.Count; row++)
            text.Append(reference.Symbols.Keys[row]).Append("\t1\t").Append(reference.Symbols.Labels[row]).Append('\n');
        text.Append("unknown ").Append(reference.Symbols.UnknownToken).Append("\t0\t-\nend\n");
        return Encoding.ASCII.GetBytes(text.ToString());
    }
    internal static byte[] Reply(string? label) => JsonSerializer.SerializeToUtf8Bytes(new
    {
        ok = true, teacher = false, verified = label is not null, miss = label is null,
        source = label is not null ? "LOCAL" : "CNET", skill = label is not null ? "capsule_core" : "capsule_refusal",
        answer = label ?? "ABSTAIN: uncovered"
    }).Concat(new[] { (byte)'\n' }).ToArray();
}

public sealed class LearningSymbolTests
{
    [Fact]
    public void ExplicitVocabularyPolicyAndSourceRetainIndependentEncodedCoverage()
    {
        LocalTableReference? reference = null;
        var failure = Record.Exception(() => reference = LearningSymbolFixture.Reference());
        Assert.True(failure is null, "LEARNING_SYMBOL_RED explicit symbolic source not supported: " + failure?.Message);
        Assert.NotNull(reference);
        Assert.Equal(2, reference.Values.Count);
        Assert.Equal((ushort)0, reference.ExpectedFor(0));
        Assert.Equal((ushort)1, reference.ExpectedFor(1));
        Assert.Null(reference.ExpectedFor(2));
        Assert.Equal(LearningSymbolFixture.Hash(LearningSymbolFixture.Source), reference.SourceSha256);
    }

    [Fact]
    public void StagedEvaluationRequiresActualSymbolObservations()
    {
        var reference = LearningSymbolFixture.Reference(); var snapshot = new string('a', 64);
        var failure = Record.Exception(() => LearningTableEvaluation.Parse(LearningSymbolFixture.Evaluation(reference, snapshot), reference, snapshot));
        Assert.True(failure is null, "LEARNING_SYMBOL_EVALUATION_RED actual decoder observations unavailable: " + failure?.Message);
    }

    private sealed class Clock : ILearningClock { public LearningInstant Now => new("boot", 100); }

    [Fact]
    public async Task LiveSymbolVerificationMustNotCertifyNumericOnlyObservations()
    {
        var reference = LearningSymbolFixture.Reference();
        var failure = await Record.ExceptionAsync(() => LearningLiveVerification.ObserveAsync(LearningSymbolFixture.Dataset,
            () => reference, _ => Task.FromResult(new ControlStatus(true, 2, new string('a', 64), null, null, true, ControlReason.Ok)),
            (key, _) => Task.FromResult(LearningAskResult.Parse(LearningSymbolFixture.Reply(reference.ExpectedFor(key)?.ToString()))), 2, new Clock()));
        Assert.True(failure is InvalidOperationException, "LEARNING_SYMBOL_OBSERVATION_RED numeric identity alone certified symbolic decoder");
    }

    [Theory]
    [InlineData("ALPHA\tfirst label\nBETA\tsecond label\n", "BETA\tsecond label\nALPHA\tfirst label\n")]
    [InlineData("BETA\t", "ALPHA\t")]
    [InlineData("ALPHA\t", "ALPHA X\t")]
    [InlineData("ALPHA\t", "ALPH@\t")]
    [InlineData("first label", "")]
    [InlineData("first label", "first\tlabel")]
    [InlineData("first label", "first\rlabel")]
    [InlineData("ALPHA", "AALPHA")]
    [InlineData("rows 2", "rows 02")]
    [InlineData("CNET_LOCAL_SYMBOLS_V1", "CNET_LOCAL_TABLE_V1")]
    public void MalformedAndVocabularyChangingSourcesRefuse(string before, string after)
    {
        Assert.Throws<ArgumentException>(() => LearningSymbolFixture.Reference(LearningSymbolFixture.Source.Replace(before, after)));
    }

    [Fact]
    public void LabelsCanChangeButVocabularyAndSnapshotCannotDrift()
    {
        var bytes = Encoding.ASCII.GetBytes(LearningSymbolFixture.Source);
        var reference = LocalTableReference.Parse(bytes, LearningSymbolFixture.Dataset);
        Array.Fill(bytes, (byte)'x');
        var updated = LearningSymbolFixture.Reference(LearningSymbolFixture.Source.Replace("first label", "  say \"hello\" \\ literal  "));
        Assert.Equal("first label", reference.Symbols!.LabelFor(0));
        Assert.Equal("  say \"hello\" \\ literal  ", updated.Symbols!.LabelFor(0));
        Assert.Equal(reference.Symbols.VocabularySha256, updated.Symbols.VocabularySha256);
        Assert.NotEqual(reference.SourceSha256, updated.SourceSha256);
        Assert.True(reference.Symbols.TryEncode("ALPHA", out var ordinal)); Assert.Equal(0, ordinal);
        Assert.False(reference.Symbols.TryEncode("alpha", out _)); Assert.Null(reference.Symbols.LabelFor(2));
        Assert.Equal("cnet_unknown_0", reference.Symbols.UnknownToken);
        Assert.Throws<NotSupportedException>(() => ((IList<string>)reference.Symbols.Keys)[0] = "other");
    }

    [Theory]
    [InlineData("null")]
    [InlineData("\"\"")]
    [InlineData("\"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\"")]
    [InlineData("42")]
    public void InvalidVocabularyPolicyCannotAuthorizeSymbolicInput(string hash)
    {
        var policy = LearningPolicyTests.Valid.Replace("\"authority\":\"verified_tool\"", "\"authority\":\"verified_tool\",\"symbol_vocabulary_sha256\":" + hash);
        Assert.Throws<ArgumentException>(() => LearningPolicy.Parse(Encoding.UTF8.GetBytes(policy)));
    }

    [Fact]
    public void NumericPolicyDoesNotImplicitlyAuthorizeSymbolicSources()
    {
        Assert.Throws<ArgumentException>(() => LocalTableReference.Parse(Encoding.ASCII.GetBytes(LearningSymbolFixture.Source),
            new("calibration", "verified_tool")));
        Assert.Throws<ArgumentException>(() => LocalTableReference.Parse(Encoding.ASCII.GetBytes(LearningSymbolFixture.Source),
            new("calibration", "verified_tool", new string('a', 64))));
    }

    [Theory]
    [InlineData("first label", "second label")]
    [InlineData("ALPHA\t1", "ALPHA\t0")]
    [InlineData("BETA\t1\tsecond label", "BETA\t1\tfirst label")]
    [InlineData("unknown cnet_unknown_0\t0\t-", "unknown cnet_unknown_0\t1\tfirst label")]
    [InlineData("unknown cnet_unknown_0", "unknown cnet_unknown_1")]
    [InlineData("symbols 2", "symbols 02")]
    [InlineData("1\t1\t1\n", "1\t1\t0\n")]
    [InlineData("CNET_SYMBOL_SNAPSHOT_EVAL_V1", "CNET_TABLE_SNAPSHOT_EVAL_V1")]
    public void SymbolicReceiptCannotHideWrongLabelsTokenSelectionOrExtraCoverage(string before, string after)
    {
        var reference = LearningSymbolFixture.Reference(); var snapshot = new string('a', 64);
        var raw = Encoding.ASCII.GetString(LearningSymbolFixture.Evaluation(reference, snapshot));
        Assert.Throws<ArgumentException>(() => LearningTableEvaluation.Parse(Encoding.ASCII.GetBytes(raw.Replace(before, after)), reference, snapshot));
    }

    [Fact]
    public void NumericOnlyReceiptCannotPassAndSourceAndCandidatePinsRemainMandatory()
    {
        var reference = LearningSymbolFixture.Reference(); var snapshot = new string('a', 64);
        var raw = LearningSymbolFixture.Evaluation(reference, snapshot);
        var text = Encoding.ASCII.GetString(raw);
        Assert.Throws<ArgumentException>(() => LearningTableEvaluation.Parse(Encoding.ASCII.GetBytes(text[..text.IndexOf("symbols 2", StringComparison.Ordinal)] + "end\n"), reference, snapshot));
        Assert.Throws<ArgumentException>(() => LearningTableEvaluation.Parse(raw, reference, new string('b', 64)));
        Assert.Throws<ArgumentException>(() => LearningTableEvaluation.Parse(raw, LearningSymbolFixture.Reference(LearningSymbolFixture.Source.Replace("first label", "changed")), snapshot));
        Assert.Throws<ArgumentException>(() => LearningTableEvaluation.Parse(raw[..^1], reference, snapshot));
        Assert.Throws<ArgumentException>(() => LearningTableEvaluation.Parse(new byte[16385], reference, snapshot));
    }

    [Fact]
    public void All256SymbolsMaximumTokenAndLabelSizesAndUnknownCollisionAreBounded()
    {
        var keys = Enumerable.Range(0, 256).Select(i => "cnet_unknown_" + i).Order(StringComparer.Ordinal).ToArray();
        var source = LearningSymbolFixture.Source[..LearningSymbolFixture.Source.IndexOf("rows 2", StringComparison.Ordinal)]
            + "rows 256\n" + string.Join('\n', keys.Select(key => key + "\tx")) + "\n";
        // 256 long keys exceed the unchanged byte cap, even though row count fits.
        Assert.Throws<ArgumentException>(() => LocalTableReference.Parse(Encoding.ASCII.GetBytes(source),
            new("calibration", "verified_tool", LearningSymbolFixture.Hash(string.Join('\n', keys) + "\n"))));
        keys = Enumerable.Range(0, 256).Select(i => "K" + i.ToString("D3")).ToArray();
        source = source[..source.IndexOf("rows 256", StringComparison.Ordinal)] + "rows 256\n" + string.Join('\n', keys.Select(key => key + "\tx")) + "\n";
        var reference = LocalTableReference.Parse(Encoding.ASCII.GetBytes(source),
            new("calibration", "verified_tool", LearningSymbolFixture.Hash(string.Join('\n', keys) + "\n")));
        Assert.Equal((ushort)255, reference.ExpectedFor(255));
        Assert.Equal(256, reference.Symbols!.Keys.Count);
        var largeKey = new string('A', 48); var largeLabel = new string('z', 128);
        source = LearningSymbolFixture.Source.Replace("ALPHA", largeKey).Replace("first label", largeLabel);
        reference = LocalTableReference.Parse(Encoding.ASCII.GetBytes(source), new("calibration", "verified_tool", LearningSymbolFixture.Hash(largeKey + "\nBETA\n")));
        Assert.Equal(largeLabel, reference.Symbols!.LabelFor(0));
        Assert.Throws<ArgumentException>(() => LocalTableReference.Parse(Encoding.ASCII.GetBytes(source.Replace(largeLabel, largeLabel + "z")),
            new("calibration", "verified_tool", reference.Symbols.VocabularySha256)));
        source = LearningSymbolFixture.Source.Replace("ALPHA", "cnet_unknown_0").Replace("BETA", "cnet_unknown_1");
        reference = LocalTableReference.Parse(Encoding.ASCII.GetBytes(source), new("calibration", "verified_tool", LearningSymbolFixture.Hash("cnet_unknown_0\ncnet_unknown_1\n")));
        Assert.Equal("cnet_unknown_2", reference.Symbols!.UnknownToken);
    }

    [Theory]
    [InlineData("correct", 0, 0)]
    [InlineData("constant", 1, 0)]
    [InlineData("wrong", 2, 0)]
    [InlineData("missing", 0, 2)]
    [InlineData("unknown_answered", 1, 0)]
    public async Task LiveSymbolBoundaryRejectsWrongOrConstantDecoderDespiteCorrectNumericIdentity(string fault, int wrong, int missing)
    {
        var reference = LearningSymbolFixture.Reference(); var observed = new List<string>();
        var result = await LearningLiveVerification.ObserveAsync(LearningSymbolFixture.Dataset,
            () => reference, _ => Task.FromResult(new ControlStatus(true, 2, new string('a', 64), null, null, true, ControlReason.Ok)),
            (key, _) => Task.FromResult(LearningAskResult.Parse(LearningSymbolFixture.Reply(reference.ExpectedFor(key)?.ToString()))),
            2, new Clock(), symbolAsk: (token, _) =>
            {
                observed.Add(token);
                var known = reference.Symbols!.TryEncode(token, out var key);
                string? text = known ? reference.Symbols.LabelFor(key) : null;
                if (known && fault == "constant") text = "first label";
                if (known && fault == "wrong") text = "wrong";
                if (known && fault == "missing") text = null;
                if (!known && fault == "unknown_answered") text = "extra coverage";
                return Task.FromResult(LearningAskResult.ParseSymbol(LearningSymbolFixture.Reply(text)));
            });
        Assert.Equal(new[] { "ALPHA", "BETA", "cnet_unknown_0" }, observed);
        Assert.Equal(2, result.CorrectAnswers); Assert.Equal(254, result.CorrectAbstentions);
        Assert.Equal(2, result.SymbolKeys);
        Assert.Equal(wrong, result.WrongSymbolAnswers); Assert.Equal(missing, result.MissingSymbolAnswers);
        Assert.Equal(fault == "correct", result.Passed);
    }

    [Fact]
    public async Task SymbolicProbeSharesTheOverallDeadlineAndSourceFence()
    {
        var original = LearningSymbolFixture.Reference(); var current = original;
        await Assert.ThrowsAsync<InvalidOperationException>(() => LearningLiveVerification.ObserveAsync(LearningSymbolFixture.Dataset,
            () => current, _ => Task.FromResult(new ControlStatus(true, 2, new string('a', 64), null, null, true, ControlReason.Ok)),
            (key, _) => Task.FromResult(LearningAskResult.Parse(LearningSymbolFixture.Reply(original.ExpectedFor(key)?.ToString()))),
            2, new Clock(), symbolAsk: (token, _) =>
            {
                current = LearningSymbolFixture.Reference(LearningSymbolFixture.Source.Replace("first label", "changed"));
                var label = original.Symbols!.TryEncode(token, out var key) ? original.Symbols.LabelFor(key) : null;
                return Task.FromResult(LearningAskResult.ParseSymbol(LearningSymbolFixture.Reply(label)));
            }));
        await Assert.ThrowsAnyAsync<OperationCanceledException>(() => LearningLiveVerification.ObserveAsync(LearningSymbolFixture.Dataset,
            () => original, _ => Task.FromResult(new ControlStatus(true, 2, new string('a', 64), null, null, true, ControlReason.Ok)),
            (key, _) => Task.FromResult(LearningAskResult.Parse(LearningSymbolFixture.Reply(original.ExpectedFor(key)?.ToString()))),
            1, new Clock(), symbolAsk: async (_, stop) =>
            {
                await Task.Delay(Timeout.Infinite, stop); throw new Exception("LEARNING_SYMBOL_DEADLINE_RED text exchange survived budget");
            }).WaitAsync(TimeSpan.FromSeconds(4)));
    }
}
