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

LongBench / InfiniteBench and full-vs-sparse quality measurements are still pending.

## Open Items

- Implement true multi-context query personalization and selective recomputation.
- Add benchmark harness for LongBench / InfiniteBench or SCBench-style shared context tests.
- Measure quality at 15-25% KV budget and only then claim the Phase 2 success metric.
- Generate CNET testimony if lost-in-the-middle regressions appear in benchmark runs.
