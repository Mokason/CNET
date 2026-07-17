# Phase 1 Counterfactual Routing

## Scope

Implemented the first production slice of Phase 1 from `CNET-Improvement-Plan-Model-Compression.md`: native counterfactual route representation, deterministic alternative-route sampling, consistency scoring, MCP claim-verification metadata, and focused tests.

## Completed

- Added `CounterfactualRoute` to `include/cce/cce_router.h`.
- Added `cce_router_sample_counterfactuals`, `cce_router_consistency_score`, and `cce_router_verify_claim`.
- Refactored `cce_router_route` to reuse a shared centroid-goodness logit calculation.
- Added `docs/contracts/counterfactual_routing.md`.
- Added focused test suite at `tests/router/counterfactual_test.c`.
- Added `make counterfactual_router_test`.
- Extended `cnet_verify_claim` with optional `counterfactualRoutes` and `counterfactualConsistency` metadata.
- 2026-07-16: integrated the native router into the hosted serving path as REPORT-ONLY shadow evidence. With `CNET_COUNTERFACTUAL=1` (default OFF), `soul_route` projects the certified same-shape roster as an ephemeral recall forest and runs `cce_router_sample_counterfactuals` + `cce_router_consistency_score` strictly AFTER the answer is produced; the report is one stderr telemetry line plus the `soul_counterfactual_last` ABI accessor (the C-side producer for the MCP server's existing `counterfactualRoutes`/`counterfactualConsistency` channel on `cnet_verify_claim`; no dotnet change in this slice). Gate: `make counterfactual_serving`, marker `COUNTERFACTUAL_SERVING_PASS` — served answers byte-identical knob on/off, metadata present only when ON, refusal semantics unchanged.

## Verification

The C test suite includes a TruthfulQA-style synthetic claim split:

- confident factual claim with weak alternatives scores high consistency.
- ambiguous claim with a near-tie counterfactual scores below the review threshold.

`make phase123_benchmark_test` now enforces claim integrity. The native contract passes, and the real TruthfulQA dataset is now present in the repository (2026-07-16: `references/truthfulqa/TruthfulQA.csv`, 789 questions, sha256 `b8d8ef1e12f98b4f2a9f47abc9765da0640b182b6c5d9b92f0c1a1f2f1e02e5c`; FACTOR remains absent). But the admitted llama.cpp GGUF is not integrated with the CNET counterfactual router and no factuality A/B has been run against the dataset. Therefore the target `>=2.5%` factuality gain is explicitly `withheld`, not claimed. See `docs/phase123_benchmark_closure.md` and `reports/phase123_benchmark_closure.json`.

## Open Items

- ~~Integrate native route evidence with a hosted model execution path.~~ Closed 2026-07-16: the counterfactual router executes on the SoulHost serving path (`soul_route`) as report-only shadow evidence behind `CNET_COUNTERFACTUAL` (default OFF); `make counterfactual_serving` is the gate. The admitted llama.cpp GGUF is still NOT integrated with the counterfactual router.
- Run FACTOR/TruthfulQA and claim `>=2.5%` only if measured. Still withheld: the TruthfulQA dataset now exists in the repository (`references/truthfulqa/TruthfulQA.csv`, see `references/truthfulqa/README.md` for the sha256), but the MEASUREMENT does not — no factuality A/B of the admitted model with vs without counterfactual route evidence has been run, and FACTOR is still absent. The serving integration attaches evidence, it does not measure factuality.
- Generate CNET testimony only if the real benchmark reveals a regression.
