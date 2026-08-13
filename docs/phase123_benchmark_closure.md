# Phase 1–3 Benchmark Closure

## Authority

`make phase123_benchmark_test` builds and runs the three native contracts plus the claim-integrity unit tests. `tools/run_phase123_benchmarks.c` then runs bounded CPU-only measurements and writes `reports/phase123_benchmark_closure.json`.

The authority distinguishes three evidence levels:

- `measured_pass` / `measured_fail`: the declared metric was executed on the required runtime path.
- `contract_pass`: the native API and focused regression contract passed.
- `withheld`: prerequisites for an external benchmark claim are absent, so no score is claimed.

Synthetic fixtures are never relabeled as FACTOR, TruthfulQA, LongBench, or InfiniteBench results.

## Phase 1 — Counterfactual Factuality

- Native counterfactual route and claim-verification contract: pass.
- Native router on the hosted serving path: pass (2026-07-16). `soul_route` executes `cce_router_sample_counterfactuals` + `cce_router_consistency_score` as REPORT-ONLY shadow evidence behind `CNET_COUNTERFACTUAL` (default OFF). The hermetic gate is `make counterfactual_serving` (marker `COUNTERFACTUAL_SERVING_PASS`): served answers are byte-identical with the knob on or off, report metadata is present only when ON, and refusal semantics are unchanged.
- Real TruthfulQA dataset present in the repository: yes (2026-07-16) — `references/truthfulqa/TruthfulQA.csv`, 789 questions, sha256 `b8d8ef1e12f98b4f2a9f47abc9765da0640b182b6c5d9b92f0c1a1f2f1e02e5c` (see `references/truthfulqa/README.md`). FACTOR: still absent.
- Admitted llama.cpp GGUF integrated with the CNET counterfactual router: no.
- Claimed `>=2.5%` gain: withheld — the dataset alone is not a measurement. No factuality A/B (admitted model with vs without counterfactual route evidence) has been run against it, and the serving integration attaches route evidence, it does not measure factuality.

The remaining open work is GGUF-runtime integration followed by a real measured A/B on the in-repo TruthfulQA split (and a FACTOR dataset, still absent); the repository still fails closed rather than repeating the target as if it were measured.

## Phase 2 — Sparse KV Routing

The real native selector was called through `bin/libphase123_benchmark.so` at:

- contexts: 512, 2,048, and 8,192 tokens;
- budgets: 15%, 20%, and 25%;
- nine total measurements.

All nine runs selected the exact budget and retained all three injected heavy-hitter needles.

Since 2026-07-16 the selector also EXECUTES on a real KV cache attention path: CNET's own `cce_gguf_qwen2` forward (the mining-oracle runner), opt-in via `cce_gguf_qwen2_set_sparse_kv()` / `CNET_SPARSE_KV=<fraction>`, default OFF with a byte-identical forward. The hermetic gate `make sparse_kv_exec` (marker `SPARSE_KV_EXEC_PASS`, in the `phase123_benchmark_build` tier beside `sparse_kv_test`) pins on a synthetic 288-token context with three planted heavy-hitter needles: OFF == ON@1.0 bit-identity; at budget 0.25 the per-step budget ceiling, needle retention at every decode step (selection-tap evidence), in-situ top-scorer retention, and decode argmax agreement vs full KV of 32/32 = 1.00 against a stated 0.90 threshold; malformed budgets refused.

This is selector plus hermetic synthetic-fixture execution evidence on CNET's OWN runner — `contract_pass`, not a long-context benchmark score. The admitted llama.cpp GGUF runtime does not execute this CNET sparse-KV path, and no real LongBench/InfiniteBench dataset is present in the repository, so LongBench/InfiniteBench quality remains withheld.

## Phase 3 — Creative Preservation

Two deterministic creative prompts were generated on CPU by both the admitted reference and recovered Q4_K_S candidate, then scored through the committed native C rubric:

- reference mean: `0.8005000055`;
- candidate mean: `0.7645000219`;
- delta: `-0.0359999835`;
- declared preservation floor: `-0.05`;
- verdict: measured pass.

This is a bounded lexical-rubric preservation measurement, not a human-preference claim. Human and independent LLM judging were not run and remain explicitly labeled `not_run`.

During closure, `make narrative_coherence_test` was found to be dead: the test expected an API not exposed by the committed header/source, and the Makefile omitted `-Iinclude`. The test now exercises the actual public API (`cnet_narrative_evaluate` and `cnet_narrative_passes`) and passes.

## Verdict

`PASS_WITH_EXTERNAL_CLAIMS_WITHHELD`

This means local native contracts and the real Phase 3 preservation measurement pass; it does not mean Phase 1 FACTOR/TruthfulQA or Phase 2 LongBench/InfiniteBench success metrics were achieved.
