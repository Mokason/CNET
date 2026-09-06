# Historical engine review

The original findings and proposed patches are preserved exactly in the
[archive](../../../docs/MAINTENANCE.md). They are not all current defects.

Source now includes regex pretokenization in
[Gpt2TiktokenEncoding](../src/CNET.Llm.Tokenizers/Bpe/Gpt2TiktokenEncoding.cs),
exact-K tie handling in
[TopKSampler](../src/CNET.Llm.Engine/Samplers/TopKSampler.cs),
a maximum-probability fallback in
[CategoricalSampler](../src/CNET.Llm.Engine/Samplers/CategoricalSampler.cs)
and [SpeculativeDecoder](../src/CNET.Llm.Engine/SpeculativeDecoder.cs).
The decoder still rejects nongreedy acceptance.

Use [ROADMAP.md](ROADMAP.md) for open implementation scope and
[SERVER.md](SERVER.md) for actual security. Preserve rejected optimization
and parity evidence. Before applying an old suggested patch, reproduce it
against current source with a focused failing test.
