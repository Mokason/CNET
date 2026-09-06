# Streaming KV selection

The streaming index lives in [cce_sparse_kv.h](../include/cce/cce_sparse_kv.h)
and [cce_sparse_kv.c](../src/cce/cce_sparse_kv.c). It chooses attention support
under a token budget using prefix sinks, recent tokens, stride landmarks and
optional attention scores.

Initialize a budget and index, set K/V slot sizes, and call
`cce_kv_stream_index_on_append` after appending each token.
`active[0..active_n)` is the selected set. Check API return values and
capacity before consuming it; changing a read budget is not freeing all
underlying KV storage.

```sh
make sparse_kv_test kv_stream_index
```

Index counters estimate selected support. They do not alone demonstrate
end-to-end decode acceleration, retained answer quality or reduced resident
memory. For the actual dense attention mask, see
[stream-index integration](STREAM_INDEX_ATTEND.md). Pager residency and
[weight epochs](WEIGHT_EPOCH.md) are separate correctness boundaries.
