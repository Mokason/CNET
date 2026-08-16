# CNET 7B-class competition implementation plan

Current execution plan: `plans/cnet_7b_competition_v5_20260814.md`.

V4 completed a frozen 448+448-row run and failed two immutable coverage gates;
its authenticated aggregate result is recorded in
`plans/cnet_7b_competition_v4_results_20260814.md`. The historical P0-P6 plan
below is retained as the original dependency contract. V5 repeats the
candidate-freeze -> independent-fixture -> one-shot-result sequence and may use
only aggregate v4 metrics.

Objective: produce a reusable native C artifact and frozen evidence showing
whether CNET-ASI-5 v1 matches or beats the pinned Bonsai 8B baseline without
weakening any certification or abstention gate.

## Dependency graph

```text
P0 preregistration
  -> P1 frozen fixture + scorer contract (RED, then GREEN)
     -> P2 certified capsule artifact (RED, then GREEN)
     -> P3 compact WordLM intent base (RED, then GREEN)
        -> P4 native end-to-end runtime
           -> P5 baseline run (first exposure to held-out prompts)
              -> P6 comparison, adversarial review, evidence report
```

P2 and P3 may proceed independently after P1. P5 is forbidden until the fixture,
scorer, and baseline prompt are committed with digests.

## P0 — freeze the falsifiable target

Files:

- `plans/cnet_7b_competition_20260813.md`
- `tasks/cnet_7b_competition_plan.md`
- `tasks/cnet_7b_competition_todo.md`

Done when the baseline identity, suite composition, output contract, immutable
PASS floors, claim boundary, and no-eviction/no-Python/no-CUDA rules are recorded
before querying the baseline.

## P1 — fixture and scorer contract

Files, first increment:

- `include/cnet_compete.h`
- `tests/test_cnet_compete_contract.c`
- `tools/cnet_compete_fixture.c`
- `benchmarks/cnet_asi5_v1/baseline_system.txt`
- Makefile target(s)

RED proof must show the absent fixture/scorer contract fails with exactly one
`CNET_7B_COMPETE_RED` marker. GREEN must deterministically generate 448 unique
rows, validate lane cardinalities and types, reject malformed/duplicate rows,
and reproduce byte-identical fixture and digest files on a second run.

Commit the generated fixture before any request reaches port 8080.

## P2 — certified specialist artifact

Implement deterministic finite-domain BTN synthesis and six typed contracts.
For every primitive, independently generate its exhaustive truth table, replay
all rows through certification, export one capsule, import it into a fresh base,
and replay again. Add corruption and manifest-compatibility refusal tests. The
composition test must prove three separately imported units, exact port
continuity, per-hop coverage enforcement, and absence of a direct shortcut.

Expected files per increment are the production source/header, one focused
test, and Makefile wiring. Generated capsules belong under a benchmark artifact
directory and are not hand-edited.

## P3 — compact language base

Train CCE WordLM in native C on specification-generated prompt/intent pairs.
Training, calibration, tokenization, export, packed reload, parameter counting,
and threshold selection are deterministic. Calibration chooses a confidence
threshold without reading held-out labels; the fixed rule is the highest
threshold that preserves at least 98% covered calibration coverage with zero
wrong answers while rejecting every calibration OOD example. If no threshold
meets that rule, P3 fails rather than relaxing it.

The packed reload must reproduce intent decisions on calibration. Record seed,
dimensions, steps, loss, calibration confusion matrix, parameter count, and
artifact bytes. Tier-A outputs are rejected as training provenance.

## P4 — native end-to-end runtime

Build a CLI/library that:

1. loads the packed WordLM and the six existing-format capsules;
2. parses one natural-language prompt into typed fields;
3. predicts typed intent and applies the calibrated confidence gate;
4. checks contract shape and exact coverage before every execution hop;
5. emits only the canonical JSON answer or canonical abstention;
6. never calls a residual teacher in scored mode;
7. reports provenance, unit names, confidence, refusal reason, and latency to a
   separate evidence stream without contaminating answer JSON.

All malformed, ambiguous, out-of-range, incompatible, missing, and corrupted
states fail closed.

## P5 — frozen baseline and CNET evaluation

Run the committed 448 prompts exactly once per system through the native scorer.
For Bonsai use the pinned endpoint, model digest, fixed system message,
temperature 0, and finite timeout. Preserve raw responses and request metadata.
Do not retry only failed content; a whole-run retry receives a new run ID and is
reported.

Then run the identical fixture through the CNET artifact. Record quality,
coverage, abstention, per-lane results, residual count, latency, parameters, and
bytes. Latency labels must disclose that the pinned Bonsai server is CPU-bound.

## P6 — verdict and review

Run the existing capsule, coverage, composition, pipeline-status, and relevant
sanitizer gates. Perform code-quality and fresh-context adversarial reviews.
Verify all preregistered floors mechanically; one missing datum yields WITHHELD,
not PASS. Publish raw result files plus a concise benchmark report naming what
was and was not tested.

The user goal is complete only if there is a runnable artifact and frozen
benchmark evidence with `CNET_7B_COMPETE_PASS`. Otherwise continue improving the
smallest causally implicated component without changing the held-out fixture or
floors.
