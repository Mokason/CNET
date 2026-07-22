using CNET.Llm.Engine;
using CNET.Llm.Models.Architectures;
using CNET.Llm.Models.Gguf;
using CNET.Llm.Tests.Integration.Fixtures;
using CNET.Llm.Tokenizers;
using Xunit;

namespace CNET.Llm.Tests.Integration.Engine;

/// <summary>
/// Integration tests for <see cref="WarmupRunner"/> against SmolLM-135M Q8_0.
/// </summary>
[Collection("SmallModel")]
public sealed class WarmupRunnerTests
{
    private readonly SmallModelFixture _fixture;

    public WarmupRunnerTests(SmallModelFixture fixture) => _fixture = fixture;

    private (TransformerModel model, GgufFile gguf, CNET.Llm.Tokenizers.Bpe.BpeTokenizer tokenizer) LoadModel()
    {
        var gguf = GgufFile.Open(_fixture.FilePath);
        var config = GgufModelConfigExtractor.Extract(gguf.Metadata);
        var model = TransformerModel.LoadFromGguf(gguf, config);
        var tokenizer = GgufBpeTokenizerFactory.Load(gguf.Metadata);
        return (model, gguf, tokenizer);
    }

    [Fact]
    public void Run_CompletesSuccessfully()
    {
        var (model, gguf, tokenizer) = LoadModel();
        using var _ = gguf;
        using var __ = model;

        var generator = new TextGenerator(model, tokenizer);
        var options = new WarmupOptions { Iterations = 1, MaxTokens = 4 };

        WarmupRunner.Run(generator, tokenizer, options);
    }

    [Fact]
    public void Run_WithDisabledOptions_SkipsWarmup()
    {
        var (model, gguf, tokenizer) = LoadModel();
        using var _ = gguf;
        using var __ = model;

        var generator = new TextGenerator(model, tokenizer);

        // Should return immediately without running any inference
        WarmupRunner.Run(generator, tokenizer, WarmupOptions.Disabled);
    }
}
