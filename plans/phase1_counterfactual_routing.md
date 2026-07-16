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

## Verification

The C test suite includes a TruthfulQA-style synthetic claim split:

- confident factual claim with weak alternatives scores high consistency.
- ambiguous claim with a near-tie counterfactual scores below the review threshold.

`make phase123_benchmark_test` now enforces claim integrity. The native contract passes, but the admitted llama.cpp GGUF is not integrated with the CNET counterfactual router and no real FACTOR/TruthfulQA dataset is present. Therefore the target `>=2.5%` factuality gain is explicitly `withheld`, not claimed. See `docs/phase123_benchmark_closure.md` and `reports/phase123_benchmark_closure.json`.

## Open Items

- Integrate native route evidence with a hosted model execution path.
- Run FACTOR/TruthfulQA after that integration and claim `>=2.5%` only if measured.
- Generate CNET testimony only if the real benchmark reveals a regression.
