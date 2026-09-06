# Attention implementation boundary

[TransformerModel](../src/CNET.Llm.Models/Architectures/TransformerModel.cs)
owns the implemented model forward; the CPU
[attention kernels](../src/CNET.Llm.Cpu/Kernels/Attention.cs) and
[strategy](../src/CNET.Llm.Cpu/Attention/NaiveAttentionStrategy.cs)
provide the corresponding computation.

Head dimensions, query/KV-head counts, causal position and cached length
must match the loaded configuration. Metadata naming an attention family
does not create a compatible implementation.

[TransformerArchitecture](../src/CNET.Llm.Models/Architectures/TransformerArchitecture.cs)
explicitly rejects DeepSeek because MLA is not implemented on this path.
Do not infer support from an enum or copy native DS-runtime results here.

Check prefill and decode separately, including cache reuse and batch-vs-step
logit parity. Attention support or context configuration alone does not prove
long-context task quality. See [KV_CACHE.md](KV_CACHE.md) and
[POSITION_ENCODING.md](POSITION_ENCODING.md).
