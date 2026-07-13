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

- confident factual claim with weak alternatives should score high consistency.
- ambiguous claim with a near-tie counterfactual should score below review threshold.

Full FACTOR/TruthfulQA benchmarking is still pending; this pass only establishes the native contract, integration surface, and regression harness.

## Open Items

- Run the larger internal factuality benchmark and measure the target `>=2.5%` gain.
- Connect native route evidence into the MCP server from an actual hosted forest instead of passing route metadata as tool arguments.
- Generate CNET testimony only if the benchmark or integration test reveals a regression.
