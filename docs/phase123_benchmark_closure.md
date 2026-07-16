# Phase 1–3 Benchmark Closure

## Authority

`make phase123_benchmark_test` builds and runs the three native contracts plus the claim-integrity unit tests. `tools/run_phase123_benchmarks.py` then runs bounded CPU-only measurements and writes `reports/phase123_benchmark_closure.json`.

The authority distinguishes three evidence levels:

- `measured_pass` / `measured_fail`: the declared metric was executed on the required runtime path.
- `contract_pass`: the native API and focused regression contract passed.
- `withheld`: prerequisites for an external benchmark claim are absent, so no score is claimed.

Synthetic fixtures are never relabeled as FACTOR, TruthfulQA, LongBench, or InfiniteBench results.

## Phase 1 — Counterfactual Factuality

- Native counterfactual route and claim-verification contract: pass.
- Real FACTOR/TruthfulQA dataset present in the repository: no.
- Admitted llama.cpp GGUF integrated with the CNET counterfactual router: no.
- Claimed `>=2.5%` gain: withheld.

The open work is runtime integration followed by a real external benchmark; the repository now fails closed rather than repeating the target as if it were measured.

## Phase 2 — Sparse KV Routing

The real native selector was called through `bin/libphase123_benchmark.so` at:

- contexts: 512, 2,048, and 8,192 tokens;
- budgets: 15%, 20%, and 25%;
- nine total measurements.

All nine runs selected the exact budget and retained all three injected heavy-hitter needles. This is selector evidence only. The admitted llama.cpp GGUF does not execute this CNET sparse-KV path, so LongBench/InfiniteBench quality remains withheld.

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
