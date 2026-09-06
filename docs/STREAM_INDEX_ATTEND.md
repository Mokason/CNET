# Stream-index attention integration

The optional stream index constrains the dense Qwen2 attention support in
[cce_gguf.c](../src/cce/cce_gguf.c). Bind it through
`cce_gguf_qwen2_bind_stream_index` and update it as tokens are appended.

The path writes K/V, scores the available positions, masks inactive positions
to negative infinity while retaining the current position, then applies
softmax/value gathering. Masking is not proof that all discarded dot products
or allocations were avoided.

This binding differs from the separate opt-in `CNET_SPARSE_KV` selector.
Do not infer a long-context accuracy gain from a smaller active-set counter.
Run the relevant native attention/selector gates and an explicitly identified
model-quality benchmark before making that claim.

The MTK KV-flush path clears the stream index with the host's neural state.
See [index API](KV_STREAM_INDEX.md), [epoch law](WEIGHT_EPOCH.md) and
[benchmark taxonomy](phase123_benchmark_closure.md).
