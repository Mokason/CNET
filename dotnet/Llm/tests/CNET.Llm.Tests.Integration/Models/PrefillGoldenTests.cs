using System.Globalization;
using System.Text;
using CNET.Llm.Core.Configuration;
using CNET.Llm.Engine;
using CNET.Llm.Engine.Samplers.StopConditions;
using CNET.Llm.Models.Architectures;
using CNET.Llm.Models.Gguf;
using CNET.Llm.Tests.Integration.Fixtures;
using CNET.Llm.Tokenizers.Bpe;
using Xunit;

namespace CNET.Llm.Tests.Integration.Models;

/// <summary>
/// Golden tests pinning greedy generation against SmolLM-135M Q8_0.
///
/// These exist because <c>MatMul.GemmQ8_0</c> switches strategy at
/// <c>DequantF32TokenThreshold</c> (16) tokens: below it, activations are
/// quantized to Q8_0 and the quantized kernel runs; at or above it, weight tiles
/// are dequantized to f32 once and activations are consumed unquantized. The two
/// produce different results, so the cases below deliberately straddle the
/// boundary — a change that moves either group is a real numeric change and
/// should be understood before the goldens are updated.
///
/// To regenerate after an intentional change:
/// <code>CNET_LLM_UPDATE_GOLDENS=1 dotnet test --filter FullyQualifiedName~PrefillGolden</code>
/// then review the diff of <c>Goldens/prefill_greedy.tsv</c>.
/// </summary>
[Collection("SmallModel")]
public class PrefillGoldenTests
{
    private readonly SmallModelFixture _fixture;

    public PrefillGoldenTests(SmallModelFixture fixture) => _fixture = fixture;

    private const int MaxTokens = 24;

    /// <summary>
    /// Prompts chosen to sit on both sides of the 16-token threshold. Token
    /// counts are recorded in the golden file, so a tokenizer change that moves
    /// a case across the boundary shows up as a diff too.
    /// </summary>
    private static readonly (string Name, string Prompt)[] Cases =
    [
        // Names describe which side of the threshold the case is meant to land
        // on; the actual token count is recorded in the golden file, and
        // GoldenCases_CoverBothSidesOfThreshold verifies the intent still holds.
        ("below_short",    "The capital of France is"),
        ("below_medium",   "The capital of France is Paris, and the"),
        ("below_near",     "One two three four five six seven eight nine ten eleven twelve"),
        ("at_threshold",   "One two three four five six seven eight nine ten eleven twelve " +
                           "thirteen fourteen fifteen sixteen seventeen"),
        ("above_medium",   "Paris is the capital and most populous city of France, situated on the river Seine. " +
                           "It has been a major settlement for more than two millennia. The capital of France is"),
        ("above_long",     "Machine learning is a field of study in artificial intelligence concerned with the " +
                           "development and study of statistical algorithms that can learn from data and generalise " +
                           "to unseen data, and thus perform tasks without explicit instructions. Recently, " +
                           "generative models have been able to"),
    ];

    private static string GoldenPath()
    {
        // Walk up from the test binary to the project directory.
        var dir = new DirectoryInfo(AppContext.BaseDirectory);
        while (dir is not null && !File.Exists(Path.Combine(dir.FullName, "CNET.Llm.Tests.Integration.csproj")))
            dir = dir.Parent;

        Assert.NotNull(dir);
        return Path.Combine(dir!.FullName, "Goldens", "prefill_greedy.tsv");
    }

    private (int promptTokens, int[] generated) Generate(string prompt)
    {
        using var gguf = GgufFile.Open(_fixture.FilePath);
        var config = GgufModelConfigExtractor.Extract(gguf.Metadata);
        // Single-threaded: thread count must not change results, and pinning it
        // keeps the goldens reproducible across machines.
        using var model = TransformerModel.LoadFromGguf(gguf, config, ThreadingConfig.SingleThreaded);
        var tokenizer = GgufBpeTokenizerFactory.Load(gguf.Metadata);

        var generator = new TextGenerator(model, tokenizer);
        var response = generator.Generate(prompt, new InferenceOptions
        {
            // Greedy argmax, so the goldens record the model's arithmetic and no
            // RNG at all. This requires SamplerSteps to stay null: SamplerPipeline
            // only enables greedy on its auto-build path, and an *empty* step
            // array instead means categorical sampling from the raw distribution.
            Temperature = 0f,
            StopConditions = [new MaxTokensStopCondition(MaxTokens)],
            MaxTokens = MaxTokens,
            Threading = ThreadingConfig.SingleThreaded,
        });

        return (response.PromptTokenCount, response.GeneratedTokenIds);
    }

    [Fact]
    public void GreedyGeneration_MatchesGoldens()
    {
        bool update = Environment.GetEnvironmentVariable("CNET_LLM_UPDATE_GOLDENS") == "1";
        string path = GoldenPath();

        var produced = new List<(string Name, int PromptTokens, int[] Generated)>();
        foreach (var (name, prompt) in Cases)
        {
            var (promptTokens, generated) = Generate(prompt);
            produced.Add((name, promptTokens, generated));
        }

        if (update)
        {
            Directory.CreateDirectory(Path.GetDirectoryName(path)!);
            var sb = new StringBuilder();
            sb.AppendLine("# Greedy generation goldens — SmolLM-135M Q8_0, single-threaded, argmax.");
            sb.AppendLine("# Regenerate: CNET_LLM_UPDATE_GOLDENS=1 dotnet test --filter FullyQualifiedName~PrefillGolden");
            sb.AppendLine("# Cases straddle MatMul.DequantF32TokenThreshold (16): prompts below it use the");
            sb.AppendLine("# quantized kernel, prompts at or above it dequantize weight tiles to f32.");
            sb.AppendLine("# name\tpromptTokens\tgeneratedTokenIds");
            foreach (var (name, promptTokens, generated) in produced)
                sb.AppendLine($"{name}\t{promptTokens}\t{string.Join(",", generated)}");
            File.WriteAllText(path, sb.ToString());
            return;
        }

        Assert.True(File.Exists(path),
            $"golden file missing: {path}. Regenerate with CNET_LLM_UPDATE_GOLDENS=1.");

        var expected = new Dictionary<string, (int PromptTokens, string Generated)>(StringComparer.Ordinal);
        foreach (string line in File.ReadAllLines(path))
        {
            if (line.Length == 0 || line[0] == '#') continue;
            string[] parts = line.Split('\t');
            expected[parts[0]] = (int.Parse(parts[1], CultureInfo.InvariantCulture), parts[2]);
        }

        foreach (var (name, promptTokens, generated) in produced)
        {
            Assert.True(expected.TryGetValue(name, out var want), $"no golden for case '{name}'");
            Assert.Equal(want.PromptTokens, promptTokens);
            Assert.Equal(want.Generated, string.Join(",", generated));
        }
    }

    /// <summary>
    /// Guards the premise of the golden set: the cases must actually straddle the
    /// threshold. If a tokenizer change pushed every case to one side, the
    /// goldens would silently stop covering the path they exist to cover.
    /// </summary>
    [Fact]
    public void GoldenCases_CoverBothSidesOfThreshold()
    {
        const int threshold = 16;
        var counts = Cases.Select(c => Generate(c.Prompt).promptTokens).ToArray();

        Assert.Contains(counts, n => n < threshold);
        Assert.Contains(counts, n => n >= threshold);
        // A case sitting just past the boundary is the one most likely to catch
        // an off-by-one in the threshold check, so require one to exist.
        Assert.Contains(counts, n => n >= threshold && n < threshold + 8);
    }
}
