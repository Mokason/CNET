# Reliability Persistence (Stats Sidecar) — Design

Date: 2026-06-11
Status: Implemented 2026-06-11 (TDD; `make test` + all demos green).
Repo: G:\AI\CNET (not a git repo — written, not committed)

## Problem

Reliability counters (`output_successes`/`output_failures`) are zeroed on
init/load, so the evidence the executors gather dies with the process. For
learned scoring to actually learn, evidence must accumulate across runs.

## Decision: per-primitive sidecar, never the weight file

Evidence lives in a separate stats file, NOT in the weight file (no v6):

- The weight file stays a **pure function definition**. Folding execution
  history into it would make saving depend on deployment history, break the
  byte-identical regeneration gate, and conflate what a primitive IS with
  how it has BEHAVED.
- The sidecar is **deployment experience**: written by whoever ran the
  primitive, whenever they choose to checkpoint it.

Format (versioned like everything else):

    CNET_STATS 1
    <successes> <failures>

API (explicit paths; the BTN does not remember where it was loaded from):

    int btn_save_stats(const BinaryTransformNetwork *btn, const char *path);
    int btn_load_stats(BinaryTransformNetwork *btn, const char *path);

Load REPLACES the counters (predictable; the accumulation loop is
load-weights -> load-stats -> execute -> save-stats). A missing or malformed
sidecar returns -1 and leaves the counters untouched — callers treat -1 as
"no recorded history" and proceed fresh. Registry-level stores (one file,
keyed by name) rejected: more machinery, and the per-primitive sidecar
matches the per-primitive weight artifact.

## Staleness rule

Evidence describes the weights it was gathered against. **Retraining a
primitive invalidates its sidecar.** The retrainer owns the invalidation:
nn_demo removes the sidecars of primitives it regenerates (today
hex_value_stats.txt and increment_stats.txt, the ones route_demo writes).
A weights-hash binding inside the sidecar was considered and deferred
(YAGNI): the single-retrainer ecosystem makes the remove-on-retrain rule
sufficient, and dynamic hidden growth makes cheap structural checks
unreliable anyway.

## Demo integration

route_demo closes the loop for real: load stats for hex_value + increment at
start (tolerating absence), execute the 16 routed chains, print the running
evidence totals, save stats at exit. Repeated `./route_demo` runs visibly
accumulate (16/0 -> 32/0 -> ...), and `make route` (which retrains) resets.

## Tests (TDD)

- test_contract.c: save/load round-trip restores counters and the derived
  btn_reliability score; loading a non-stats file (the v1 weights fixture)
  returns -1 and leaves counters untouched; missing file returns -1.
- test_router.c: full cross-run loop with the real executor against temp
  files — run 1: fresh load, missing sidecar, execute (1 success), save;
  run 2: fresh weight load (counters zero), load sidecar (restores 1),
  execute (2), save; run 3: fresh load + sidecar shows the accumulated 2.

## Out of scope (YAGNI)

- Weights-hash binding in the sidecar (revisit if retraining stops being
  centralized in nn_demo).
- Merge-on-load / multi-writer aggregation semantics.
- Persisting stats for the other demos (dag/hetero/split) — same calls,
  wire when wanted.
