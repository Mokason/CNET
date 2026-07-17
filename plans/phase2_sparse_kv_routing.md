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

## Execution on CNET's own KV path (2026-07-16)

The selector now EXECUTES on a real KV cache attention path: the
`cce_gguf_qwen2` forward in `src/cce/cce_gguf.c` — the same runner the
mining oracle binds to. Wiring:

- `cce_gguf_qwen2_set_sparse_kv(model, budget_fraction)` plus the
  `CNET_SPARSE_KV=<fraction>` env knob read at load. Default OFF; when OFF
  the forward is byte-identical (the historical attention code runs
  verbatim behind one branch). Malformed budgets (negative, > 1, NaN,
  garbage env values) are refused; the qwen35 hybrid runner is refused
  (it has no sparse read).
- When ON, each attention step's softmax/V-read restricts to the rows
  chosen by the ONE `cce_specialist_select_kv_tokens()` selector under
  `max_tokens = ceil(fraction * visible rows)`, scored by that step's raw
  pre-softmax q·k values. Budget 1.0 selects every row and is gated
  BIT-IDENTICAL to OFF.
- Hermetic gate: `make sparse_kv_exec` (terminal marker
  `SPARSE_KV_EXEC_PASS`, wired into `phase123_benchmark_build` beside
  `sparse_kv_test`). On a synthetic 320-ctx qwen2 GGUF the test writes
  itself (288-token context: 256-token prefill + 32 teacher-forced decode
  steps, three planted heavy-hitter needle tokens mid-context at positions
  101/149/203) it pins: OFF == ON@1.0 bit-identity; at budget 0.25 the
  budget ceiling holds at every (layer, head, step), all three needles stay
  attended (selection tap evidence, layer 0, both heads, every decode
  step), the in-situ top-scoring row is always kept, and decode argmax
  agreement vs full KV measured 32/32 = 1.00 against a stated 0.90
  threshold; malformed budgets refused; OFF-restore bit-identical.

This is hermetic synthetic-fixture evidence on CNET's OWN runner. It is
NOT the admitted llama.cpp GGUF runtime, and no real long-context dataset
is present in the repository — LongBench / InfiniteBench quality remains
explicitly `withheld`.

## Open Items

- ~~Integrate sparse selection into the admitted model's KV execution path.~~
  DONE for CNET's own runner (`cce_gguf_qwen2`, hermetic gate
  `make sparse_kv_exec`, see above). The admitted llama.cpp runtime still
  does not execute this path; that integration remains open.
- Implement true multi-context query personalization and selective recomputation.
- Run LongBench / InfiniteBench or SCBench after integration and claim quality only from those measurements (still blocked: no real dataset in the repository, and the llama.cpp runtime does not execute this path).
- Generate CNET testimony if the real long-context benchmark reveals lost-in-the-middle regressions.
