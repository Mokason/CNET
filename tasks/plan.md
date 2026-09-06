# Cognitive Runtime Integration Plan

Source of truth: `plans/cognitive_runtime_integration_20260726.md`.

## Delivery slices

1. Shared workspace
   - Define a fixed-capacity, deterministic workspace contract.
   - Support push, recent, query, clear, and digest operations.
   - Prove ring eviction, trust preservation, and digest behavior hermetically.
2. Semantic cortex
   - Add hermetic and optional residual-HTTP backends.
   - Publish only uncertified proposals into the shared workspace.
   - Keep final authority outside the semantic cortex.
3. Sleep consolidation
   - Ingest episodic records into tile memory.
   - Consolidate duplicate/redundant records into semantic/procedural memory.
   - Emit a provenance-bound report with pruning and merge counts.
4. Calibrated governance
   - Route answers through per-route margin, reliability, and sample floors.
   - Default to abstention for invalid or insufficient evidence.
   - Reject claim publication without bound evidence references.
5. Capability certification
   - Validate held-out manifests and evidence markers without a shell.
   - Emit machine-readable results with fixture and evidence digests.
   - Attach the classification lane only after its healthy marker is committed.
6. Integration and release evidence
   - Add focused Make targets and a `cognitive_runtime` umbrella.
   - Update README, changelog, and integration-plan checkboxes.
   - Run complete gates, inspect the diff, and commit only coherent green slices.

## Coordination boundary

Claude owns `tests/cce_train_bench.c` until a commit containing
`CLASSIFICATION_LANE_HEALTHY` lands. This work must not modify or stage that file.

## September 6 capsule lifecycle follow-up

The completed integration plan above is preserved. The new operator-requested
scope is recorded in `plans/cnet_capsule_hot_swap_20260906.md`: explicit live
capsule working-set swaps, guarded upgrades, restart/rollback, and local-source
codebase evidence. The operator requested increasing the small inventory cap
given 96 GB RAM. Capacity verification is recorded separately in
`plans/cnet_capsule_capacity_4096_20260906.md`; resident lifecycle remains pending.
