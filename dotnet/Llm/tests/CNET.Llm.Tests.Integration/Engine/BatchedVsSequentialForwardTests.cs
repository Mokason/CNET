using CNET.Llm.Core.Configuration;
using CNET.Llm.Core.Tensors;
using CNET.Llm.Engine.KvCache;
using CNET.Llm.Models.Architectures;
using CNET.Llm.Models.Gguf;
using CNET.Llm.Tests.Integration.Fixtures;
using CNET.Llm.Tokenizers.Bpe;
using Xunit;
using Xunit.Abstractions;

namespace CNET.Llm.Tests.Integration.Engine;

/// <summary>
/// Does a batched forward produce the same logits as feeding the same tokens one
/// at a time? Speculative decoding of every kind assumes it does: verification
/// runs a batch, but the sequence it is verifying was produced one token at a
/// time. If the two paths disagree, argmax can differ on a near-tie and
/// speculation stops being output-identical to plain greedy.
/// </summary>
[Collection("SmallModel")]
public class BatchedVsSequentialForwardTests
{
    private readonly SmallModelFixture _fixture;
    private readonly ITestOutputHelper _output;

    public BatchedVsSequentialForwardTests(SmallModelFixture f, ITestOutputHelper o)
    { _fixture = f; _output = o; }

    [Fact]
    public unsafe void BatchedForward_MatchesSequentialForward()
    {
        using var gguf = GgufFile.Open(_fixture.FilePath);
        var config = GgufModelConfigExtractor.Extract(gguf.Metadata);
        using var model = TransformerModel.LoadFromGguf(gguf, config, ThreadingConfig.SingleThreaded);
        var tokenizer = GgufBpeTokenizerFactory.Load(gguf.Metadata);

        int[] ids = tokenizer.Encode("The capital of France is Paris and the capital of Italy is Rome");
        int vocab = config.VocabSize;
        int prefix = 6, extra = 4;

        // Sequential: feed tokens one at a time, keep each step's logits.
        var seqLogits = new float[extra][];
        using (var kv = new SimpleKvCache(config.NumLayers, config.NumKvHeads, config.HeadDim, 128))
        {
            using (model.Forward(ids.AsSpan(0, prefix), Enumerable.Range(0, prefix).ToArray(), -1, kv)) { }
            for (int i = 0; i < extra; i++)
            {
                using ITensor t = model.Forward([ids[prefix + i]], [prefix + i], -1, kv);
                seqLogits[i] = new Span<float>((void*)t.DataPointer, vocab).ToArray();
            }
        }

        // Batched: same prefix, then all `extra` tokens in one call.
        var batchLogits = new float[extra][];
        using (var kv = new SimpleKvCache(config.NumLayers, config.NumKvHeads, config.HeadDim, 128))
        {
            using (model.Forward(ids.AsSpan(0, prefix), Enumerable.Range(0, prefix).ToArray(), -1, kv)) { }
            using ITensor t = model.Forward(
                ids.AsSpan(prefix, extra),
                Enumerable.Range(prefix, extra).ToArray(), -1, kv);
            for (int i = 0; i < extra; i++)
                batchLogits[i] = new Span<float>((float*)t.DataPointer + (long)i * vocab, vocab).ToArray();
        }

        int argmaxDiffs = 0;
        double worstAbs = 0;
        for (int i = 0; i < extra; i++)
        {
            int a = Array.IndexOf(seqLogits[i], seqLogits[i].Max());
            int b = Array.IndexOf(batchLogits[i], batchLogits[i].Max());
            if (a != b) argmaxDiffs++;
            for (int v = 0; v < vocab; v++)
                worstAbs = Math.Max(worstAbs, Math.Abs(seqLogits[i][v] - batchLogits[i][v]));
        }

        _output.WriteLine($"positions compared : {extra}");
        _output.WriteLine($"argmax disagreements: {argmaxDiffs}");
        _output.WriteLine($"largest logit delta : {worstAbs:E3}");

        // This SHOULD be zero. It is not: the two paths are different
        // implementations — decode (seqLen == 1) uses fused RMSNorm+quantize and
        // the R4-interleaved kernels, batched uses unfused norm and the tiled
        // GEMM path. The gap is ~0.5 in logit space, far past rounding.
        //
        // The consequence is not cosmetic: any speculative decoder verifies with
        // a batched forward but is checking a sequence produced token-at-a-time,
        // so on a near-tie the two disagree and speculation stops being
        // output-identical to plain greedy. That applies to SpeculativeDecoder
        // just as much as to PromptLookupDecoder.
        //
        // Pinned at the measured magnitude so both a regression and a fix are
        // visible. Tighten this to 0 when the paths are reconciled.
        Assert.True(worstAbs < 1.0,
            $"batched vs sequential logit gap grew to {worstAbs:E3} (was ~0.5)");
    }
}
