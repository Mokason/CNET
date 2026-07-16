# Phase 2 Sparse Per-Specialist KV Routing

## Scope

Implemented the first Phase 2 slice: a reusable sparse KV selector, a runtime mode flag for full vs sparse context routing, an autoregressive Supra KV hook, focused selector tests, and SamKV/SCOPE adaptation notes.

## Completed

- Added `include/cce/cce_sparse_kv.h` and `src/cce/cce_sparse_kv.c`.
- Defined `cce_specialist_kv_budget` for per-specialist KV budgets.
- Defined `SparseKVSelector` and `cce_context_routing_mode`.
- Implemented `cce_specialist_select_kv_tokens()`.
- Added `cce_supra_set_context_routing()`.
- Wired sparse routing into `cce_supra_generate_text()` / `kv_step()` while keeping full KV as default.
- Added `tests/sparse_kv_test.c` and `make sparse_kv_test`.
- Wrote `references/samkv_cnet_adaptation.md` and `docs/contracts/sparse_kv_routing.md`.

## Verification

The focused test validates:

- full-budget mode returns all tokens in causal order.
- sparse mode keeps initial anchors, recent tokens, long-range anchors, and high-score middle tokens.
- default budget targets roughly 20% of context.

The bounded authority also called the real native selector at 512, 2,048, and 8,192 tokens with 15%, 20%, and 25% budgets. All nine measurements used the exact budget and retained every injected heavy-hitter needle.

This is selector evidence, not LongBench quality evidence. The admitted llama.cpp runtime does not execute this CNET sparse-KV path, so LongBench / InfiniteBench quality is explicitly `withheld`. See `docs/phase123_benchmark_closure.md` and `reports/phase123_benchmark_closure.json`.

## Open Items

- Integrate sparse selection into the admitted model's KV execution path.
- Implement true multi-context query personalization and selective recomputation.
- Run LongBench / InfiniteBench or SCBench after integration and claim quality only from those measurements.
- Generate CNET testimony if the real long-context benchmark reveals lost-in-the-middle regressions.
