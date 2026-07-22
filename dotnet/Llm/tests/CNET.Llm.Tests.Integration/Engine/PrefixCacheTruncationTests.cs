using CNET.Llm.Core.Configuration;
using CNET.Llm.Engine;
using CNET.Llm.Engine.PromptCache;
using CNET.Llm.Engine.Samplers.StopConditions;
using CNET.Llm.Models.Architectures;
using CNET.Llm.Models.Gguf;
using CNET.Llm.Tests.Integration.Fixtures;
using CNET.Llm.Tokenizers.Bpe;
using Xunit;

namespace CNET.Llm.Tests.Integration.Engine;

/// <summary>
/// Regression: with a PrefixCache, a follow-up prompt that grew slightly used to
/// reuse the previous turn's smaller KV cache whenever it could hold the PROMPT
/// alone (ResolveKvCache's old <c>|| MaxLength >= promptLen</c> clause). The
/// decode loop then hit the cache wall early and silently truncated the answer.
/// </summary>
[Collection("SmallModel")]
public class PrefixCacheTruncationTests
{
    private readonly SmallModelFixture _fixture;

    public PrefixCacheTruncationTests(SmallModelFixture fixture) => _fixture = fixture;

    [Fact]
    public void GrownPrompt_GetsItsFullAnswer_NotATruncatedOne()
    {
        using var gguf = GgufFile.Open(_fixture.FilePath);
        var config = GgufModelConfigExtractor.Extract(gguf.Metadata);
        using var model = TransformerModel.LoadFromGguf(gguf, config, ThreadingConfig.SingleThreaded);
        var tokenizer = GgufBpeTokenizerFactory.Load(gguf.Metadata);

        using var prefixCache = new PrefixCache(maxEntries: 1);
        var generator = new TextGenerator(model, tokenizer, prefixCache: prefixCache);

        InferenceOptions Opts(int maxTokens) => new()
        {
            Temperature = 0f,
            StopConditions = [new MaxTokensStopCondition(maxTokens)],
            MaxTokens = maxTokens,
            Threading = ThreadingConfig.SingleThreaded,
        };

        // Turn 1: small prompt, small answer — retained cache is small.
        string header = "The quick brown fox jumps over the lazy dog again and again. ";
        generator.Generate(header + "Continue:", Opts(24));

        // Turn 2: same prefix, slightly longer prompt. Under the old reuse
        // condition the turn-1 cache (promptLen1 + 24 slots) held this prompt
        // but not prompt + 24 new tokens -> silent truncation.
        var r2 = generator.Generate(header + "Continue: The quick brown fox", Opts(24));

        Assert.Equal(24, r2.GeneratedTokenCount);
        Assert.Equal(FinishReason.Length, r2.FinishReason);
    }
}
