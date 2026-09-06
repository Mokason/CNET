using CNET.Llm.Core.Attention;
using CNET.Llm.Core.Configuration;
using CNET.Llm.Core.Models;
using CNET.Llm.Core.Tensors;
using CNET.Llm.Engine;
using CNET.Llm.Server;
using CNET.Llm.Tokenizers;

namespace CNET.Llm.Tests.ServerSecurity;

internal sealed class FixtureEngine : IModel
{
    private int _calls;
    private int _disposeCalls;
    internal int Calls => Volatile.Read(ref _calls);
    internal int DisposeCalls => Volatile.Read(ref _disposeCalls);
    internal Action<int>? OnForward { get; set; }
    internal Action? OnDispose { get; set; }
    public long ComputeMemoryBytes => 0;
    public ModelConfig Config { get; } = new()
    {
        Architecture = Architecture.Llama, VocabSize = 4, HiddenSize = 1, IntermediateSize = 4,
        NumLayers = 1, NumAttentionHeads = 1, NumKvHeads = 1, HeadDim = 1, MaxSequenceLength = 128,
    };

    internal ServerState CreateState()
    {
        var tokenizer = new FixtureTokenizer();
        return new ServerState
        {
            Options = new ServerOptions { Model = "fixture-engine", Host = "127.0.0.1", Port = 0 },
            Config = Config, Model = this, Tokenizer = tokenizer, IsReady = true,
            Generator = new TextGenerator(this, tokenizer), ChatTemplate = new FixtureChatTemplate(),
        };
    }

    public ITensor Forward(ReadOnlySpan<int> tokenIds, ReadOnlySpan<int> positions, int deviceId)
        => Forward(tokenIds, positions, deviceId, null);

    public unsafe ITensor Forward(ReadOnlySpan<int> tokenIds, ReadOnlySpan<int> positions, int deviceId, IKvCache? kvCache)
    {
        ObjectDisposedException.ThrowIf(DisposeCalls > 0, this);
        int call = Interlocked.Increment(ref _calls);
        OnForward?.Invoke(call);
        var tensor = UnmanagedTensor.Allocate(new TensorShape(1, 4), DType.Float32);
        var logits = new Span<float>((void*)tensor.DataPointer, 4);
        logits.Fill(-100);
        logits[1] = 100;
        return tensor;
    }

    public void Dispose()
    {
        Interlocked.Increment(ref _disposeCalls);
        OnDispose?.Invoke();
    }

    private sealed class FixtureTokenizer : ITokenizer
    {
        public int VocabSize => 4;
        public int BosTokenId => 0;
        public int EosTokenId => 3;
        public int[] Encode(string text) => [1];
        public int CountTokens(string text) => 1;
        public string Decode(ReadOnlySpan<int> tokenIds) => new('x', tokenIds.Length);
        public string DecodeToken(int tokenId) => tokenId == 1 ? "x" : "";
    }

    private sealed class FixtureChatTemplate : IChatTemplate
    {
        public string Apply(IReadOnlyList<ChatMessage> messages, ChatTemplateOptions options) => "fixture prompt";
    }
}
