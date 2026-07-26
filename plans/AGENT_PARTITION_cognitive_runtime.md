# Agent file partition (2026-07-26 cognitive runtime)

## Claude exclusive until CLASSIFICATION_LANE_HEALTHY is committed
- `tests/cce_train_bench.c`
- (optional local only) scratch diagnostics under /tmp

After Claude commits HEALTHY, Claude **stops** (or only touches that file if a follow-up fix is needed).

## Codex exclusive
- `include/cnet_shared_workspace.h` `src/cnet_shared_workspace.c` (+ tests/make)
- `include/cnet_semantic_cortex.h` `src/cnet_semantic_cortex.c` (+ tests/make)
- `include/cnet_sleep_consolidate.h` `src/cnet_sleep_consolidate.c` (+ tests/make)
- `include/cnet_capability_cert.h` `src/cnet_capability_cert.c` + `config/capability_manifests/*`
- calibrated abstention/provenance modules
- `Makefile` targets for the above (avoid rewriting `cce_train_bench` recipe until Claude done)
- docs: README/CHANGELOG rows for new gates
- `plans/cognitive_runtime_integration_20260726.md` checkboxes

## Shared later (Codex only after Claude commit)
- capability_cert wiring that *reads* classification markers (no edit of bench source required)
- `make cognitive_runtime` umbrella

## Never both
- Same file simultaneous edit
- Force-push / push without user ask
- Commit `cce.dll` / `cnet.so` as feature noise
