# Exact v3 routing optimization — 2026-09-11

The user requires an improvement without sacrificing speed, routing quality, or resource use. Keep the current 2048-dimensional encoding, quantization, radii, ambiguity thresholds, and capsule format. Accept changes only with exact baseline parity and measured resource or latency gains.

Implemented:

1. Reduce int8 squared norms with int32 arithmetic. The maximum at width 2048, including -128, is 33,554,432; a compile-time assertion guards the bound. Conversion to double and square root are unchanged.
2. Compute the wide query once per distinct representation: BAG/HD/CGRAM_C share plain unigrams, STEM/STEM_BI share stemmed unigrams. Preserve the float path's separate encoder handling.
3. Replace the shared wide query array with per-call scratch. Separate wide and float helpers so each path has its own stack footprint.

The deterministic overlapping-query test produced `CNET_VSA_Q8_BENCH_RED` before implementation and passes afterward. It pauses a route after its first norm calculation while another route encodes a different query; it checks all route-result fields against serial results. Encoder tables are warmed before this test. This establishes query-buffer isolation, not blanket thread safety for lazy encoder initialization or mutable registries.

The norm change preserves every result for every int8 vector at the fixed width by construction. Corpus parity additionally compared 316,240 vectors and norms across 63,248 sentences from 1,068 corpora. No semantic-quality improvement is claimed; the measured sweep floors remain unchanged. The prior integrity/calibration review findings are outside this optimization and remain open.

Reproduction gate: `make cnet_vsa_q8_bench`; timing-only mode: `bin/test_cnet_vsa_q8_bench --timing-only`. The test uses GNU linker `--wrap` only to schedule deterministic query overlap, with no production synchronization overhead. Results and raw paired timings: `result/cnet_vsa_q8_optimization_20260911.md` and the adjacent JSON.
