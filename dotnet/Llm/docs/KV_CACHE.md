# KV cache ownership and reuse

The engine has [simple, paged and quantized cache implementations](../src/CNET.Llm.Engine/KvCache/).
KV stores intermediate neural state for a specific model, token sequence,
position convention and precision. It is not certified knowledge.

[TextGenerator](../src/CNET.Llm.Engine/TextGenerator.cs) supports prefix
reuse for both simple and paged caches, not only SimpleKvCache.
Quantized/device caches have separate restrictions; do not assume an
unimplemented prefix-clone or compatible staging buffer.

Cache capacity, physical allocation and prompt-prefix reuse are different
metrics. Paged staging may gather data for a kernel; a logical block count
does not prove all contiguous memory traffic was eliminated.

Reset/truncate/copy semantics must preserve the exact retained prefix,
and request owners must serialize mutable cache use. Changing model weights
or tokenization invalidates reuse. Compare fresh versus reused logits/tokens,
including the next generated token after rollback.

Historical illustrative memory savings are preserved in the
[archive](../../../docs/MAINTENANCE.md), not measured production RSS.
See [SPECULATIVE.md](SPECULATIVE.md) for rollback/eligibility constraints.
