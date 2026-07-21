# CNET Six-Priority Improvement Chunk (2026-07-21)

**Safety envelope (autonomous):**
- No public push
- No reserved GPU wake (GPU_0/GPU_1 stay policy-bound)
- No starting Gemma/Bonsai/Qwen teachers; hermetic or already-running offline paths only
- No enable of `cnet-gap-lane` service
- Dirty-tree WIP preserved; Makefile link-dupe fix included

## Priorities → gates

| # | Priority | Gate | Marker |
|---|---|---|---|
| P1 | Serve feedback loop | `make serve_feedback` | `SERVE_FEEDBACK_PASS` |
| P2 | Composition distill | `make self_improve` + `make post_seal_serve` | existing markers |
| P3 | Miner efficiency A/B | `make miner_efficiency_bench` | `MINER_EFFICIENCY_BENCH_PASS` |
| P4 | Oracle park-wake + unattested honesty | `make acquire` + `make oracle_unattested` | existing + `ORACLE_UNATTESTED_PASS` |
| P5 | Product surface | `make json_toolcall` + `make soul_residual_serve` + `make residual_gguf` | existing |
| P6 | Benchmark taxonomy | `make phase123_benchmark_test` + `make benchmark_taxonomy` | `PASS_WITH_EXTERNAL_CLAIMS_WITHHELD` / `BENCHMARK_TAXONOMY_PASS` |

## Umbrella

```
make cnet_use_loop_acceptance
→ CNET_USE_LOOP_ACCEPTANCE_PASS
```

Single final check for this chunk. Focused RED→GREEN inside each priority; umbrella is the release authority for the ordered set.

## Hypotheses

- H0: serve path does not move reliability/stats; miner has no comparable efficiency signal; unattested oracles look like attested; external benchmarks can be relabeled measured.
- H1: hermetic serve feedback moves Laplace reliability and certified_serves; miner A/B reports teacher efficiency; unattested digests are explicit labels; taxonomy refuses measured_* without a real path.
