# CNET-ASI-5 v5 semantic-admission recovery plan

Status: **DEVELOPMENT ONLY — NO V5 CANDIDATE OR FIXTURE FREEZE**

## Objective and evidence boundary

Produce a new native-C candidate that preserves v4's perfect measured
selective correctness and OOD abstention while raising independently held-out
covered-answer coverage from 0.40 to at least the unchanged 0.95 floor. The
six certified capsules, their typed domains, portability format, corruption
and compatibility refusal, scorer floors, and residual-call prohibition do
not change merely to improve coverage.

V5 development may use only v4 aggregate metrics recorded in
`plans/cnet_7b_competition_v4_results_20260814.md`. Individual v4 responses,
prompt/output pairs, per-row diagnostics, and fixture wording are forbidden.
V5 development prompts contain intent labels and externally verified contract
metadata only; CNET Tier-A answers remain excluded.

Protocol deviation recorded 2026-08-14: while locating composition symbols, a
read-only source search was accidentally scoped across the v4 benchmark
directory and printed several v4 fixture prompt lines. No row outputs or
per-row scores were read. Those lines are prohibited as v5 development input;
the v5 candidate corpus is independently generated from the public contracts,
and the actual v5 fixture still starts only after S5. This means the stronger
claim that no v4 wording was ever displayed is withdrawn rather than hidden.

## Development checkpoint

- V5.1 diagnostic gate: PASS, with one serving path and no answer exposure.
- V5.2 native semantic matrix: 160 covered forms (32 per intent), 160 paired
  OOD mutations, all 16 policy states, and no stored answer values.
- Prior-source audit: 320 candidates against 2,807 exclusions, with zero exact,
  canonical, or thresholded near overlaps.
- RED at commit `a5b9d32`: 61/180 combined covered development cases answered,
  zero unsafe answers across 181 combined OOD cases, zero structural
  duplicates. This is diagnostic evidence only, not a benchmark result.

## Causal diagnosis

V4 answered 128 covered rows and all 128 were correct. It safely abstained on
all 128 OOD rows and had zero contract violations, invocation failures, or
residual calls. The lane counts were 32/64, 16/64, 16/64, 16/64, and 48/64.
Therefore the smallest causally implicated subsystem is semantic admission:
the learned intent proposal, typed semantic parser, or argument extraction.
Capsule execution and answer computation are not implicated by this evidence.

## Architecture decision

Keep the learned base and independent typed proof as two mandatory gates, but
make their boundary observable in development. Replace template-shaped
acceptance with an explicit typed semantic frame:

```text
ASCII lexemes
  -> learned intent proposal + calibrated confidence
  -> closed token-role classification
  -> one unambiguous typed contract frame
  -> exact argument/configuration consumption
  -> capsule coverage check(s)
  -> certified execution or abstention
```

Every semantically meaningful token must be consumed by the selected frame.
Unknown words, unsupported operators, unconsumed values, extra capabilities,
side effects, contract overrides, and ambiguous frames abstain. Neutral request
scaffolding is a finite, frozen vocabulary shared across intents. This keeps
the parser closed while allowing safe word-order and scaffolding variation.

## Ordered increments

### V5.1 — authenticated v4 result and development-only diagnostics

Add a refusal-stage diagnostic API that reports only which admission gate
stopped a developer-authored prompt: intent proposal, intent disagreement,
typed frame, argument parsing, coverage, or execution. It must not alter answer
JSON, scorer behavior, or release journals.

Acceptance criteria:

- a focused test first emits `CNET_7B_V5_DIAGNOSTIC_RED` against the absent API;
- the GREEN test distinguishes each refusal stage without exposing answers;
- existing v4 runtime, sanitizer, capsule, and score tests remain unchanged and
  green.

### V5.2 — independent semantic stress corpus

Create a deterministic native-C generator for development-only paraphrase
families derived from the five public contract definitions. Use token-role and
clause-order cross-products, not fixture templates. Add paired counterfactuals
for range/type, extra values, parameter variants, hop mutations, multi-intent,
unrelated tasks, side effects, and overrides.

Acceptance criteria:

- at least 32 structurally distinct covered forms per intent across boundary
  values and all 16 policy states;
- at least one paired OOD mutation for every covered form, with no answer
  values in the exported training corpus;
- exact duplicate, canonical overlap, and template-near-overlap audit against
  all v1-v4 fixtures and all prior development sources;
- a pre-implementation run emits `CNET_7B_V5_SEMANTIC_COVERAGE_RED` with stage
  counts, not prompt text.

### V5.3 — learned intent generalization

Use V5.1 stage counts to change the smallest necessary learned component. Grow
the answer-free external-spec corpus and, only if required by measured
tokenization refusal, the fixed context or model capacity. Calibration still
requires zero wrong covered answers, full calibration OOD abstention, packed
parity, and a stored threshold. The complete base remains below 1% of Bonsai
8B parameters and bytes.

Acceptance criteria:

- learned intent proposal covers every positive stress row at the calibrated
  threshold and rejects every negative calibration row;
- zero Tier-A answer bytes enter training or calibration;
- deterministic retraining reproduces artifact and metadata hashes.

### V5.4 — typed-frame generalization, one contract at a time

Implement and commit five separately testable typed-frame slices: increment,
minutes, CRC-8/ATM, access policy, then three-hop composition. Each slice
normalizes semantic roles and accepts harmless order/scaffolding changes while
requiring exact type, configuration, cardinality, and consumed-token proofs.

Acceptance criteria for every slice:

- its positive stress matrix reaches 100% answered and exact;
- every paired counterfactual abstains;
- no other lane's behavior regresses;
- composition executes exactly three independently imported capsules and
  records exactly three coverage checks for every answered row.

### V5.5 — freeze candidate before fixture authorship

Run deterministic rebuilds, ASan/UBSan, exhaustive capsule replay, independent
portable import, cumulative retention, corruption/incompatibility refusal,
semantic stress, OOD, and marker-cardinality gates. Freeze the full transitive
behavior/build/evaluation closure and artifact members in candidate commit
`S5`.

Acceptance criteria:

- development covered coverage and selective accuracy are both 1.0;
- development unsafe OOD, contract violations, residual calls, non-finite
  outputs, and invocation failures are zero;
- all immutable benchmark floors remain byte-identical to v4;
- the candidate/build closure is sorted, unique, complete, and reproducible
  from a scrubbed Git archive without Python.

### V5.6 — independent fixture and one-shot benchmark

Only after `S5`, derive an exact seed from `S5`, the artifact manifest, and
`CNET-ASI-5-v5`. Author a new 448-row fixture and independent oracle without
executing either backend. Audit it against all prior fixtures/development
corpora, freeze it in `F5`, perform read-only adversarial review, then issue the
one-shot/resumable benchmark.

Acceptance criteria:

- 320 covered and 128 OOD rows with unchanged lane/class balance;
- zero duplicate, canonical, or thresholded near overlaps;
- exactly one authenticated terminal verdict;
- the project goal completes only with `CNET_7B_COMPETE_PASS` and preserved
  raw aggregate evidence. A FAIL starts another independent candidate cycle;
  no floor is lowered and no v5 row-level result becomes training data.

## Checkpoints

1. After V5.1-V5.2: review stage-count evidence and approve the smallest model
   or parser changes; do not touch capsule behavior.
2. After V5.3-V5.4: run all existing and new development gates plus sanitizers;
   review false-accept boundaries before freezing.
3. After V5.5: no candidate, scorer, floor, or release-identity change is
   permitted. Fixture work starts only from the committed `S5` archive.
4. After V5.6: report the exact terminal result and keep broader claims
   **WITHHELD** beyond the measured suite.

## Risks and mitigations

| Risk | Impact | Mitigation |
|---|---|---|
| Coverage is increased by a fail-open parser | Unsafe OOD answers | Closed token roles, exact consumption, paired counterfactuals, zero-unsafe gate |
| The learned base merely memorizes templates | Another held-out coverage collapse | Cross-product role/order stress, source-family holdouts, post-freeze independent fixture |
| V4 held-out leakage influences v5 | Invalid comparison | Aggregate-only development; exclude v4 fixture wording and row outputs; contamination audit |
| A larger model becomes a vanity metric | Misleading size claim | Increase only from measured classifier-stage misses and retain both 1% ceilings |
| Repeated full rebuilds dominate iteration time | Slow feedback | Focused RED/GREEN targets per admission stage; full archive gate only at checkpoints |
