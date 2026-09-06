# Shared typed-selection experiment and guarded product boundary

Status: implemented bounded experiment, after measured resident FP32 checkpoint. No
certification floor changes and no claim beyond the specified task gates.

## Model and measured scope

Use a shared local-update cell: three inputs (goal indicator, previous node score,
maximum compatible-successor score), eight sigmoid hidden units, one sigmoid
output, 41 FP32 parameters. Apply the same cell at every node for at most 64
iterations. Record first high-confidence activation to rank compatible next hops.
Identity/version and typed-compatibility metadata are preserved by the graph
adapter, not converted into arbitrary numeric ID magnitudes. Verify all metadata
and bounds before the model sees a graph. Unsupported/uncertain inputs abstain.

This is a **learned local update inside an explicit graph-propagation template**,
not unconstrained problem solving. The deterministic shortest-path control stays
in every comparison. Small deterministic routing may be cheaper on CPU; GPU
training usefulness does not imply GPU placement for each live request.

The cell's sigmoid layout also permits direct FP32-to-double copying into the
existing BTN representation. That is one genuine GPU-fit-to-capsule test, not a
replacement capsule format or finite-domain compiler. This vertical slice can
run before activation work because product-plan stage 5 depends on 1, not 4.

## Ordered slices and gates

1. Cell math and GPU fit (small: header, CPU cell, experimental GPU trainer, tests).
   RED numerical-gradient/CPU-GPU update/invalid-input tests first. Independently
   generated Boolean local-update labels; finite weights, immutable snapshot,
   poisoned-device refusal. Do not substitute hand-written output weights.
2. Structured graph adapter and frozen evaluation (medium). Train on size <=8;
   test disjoint DAG/cyclic/chain/tree/typed-distractor fixtures at sizes 8/16/32/64,
   explicit long chains, node permutations, unreachable and unsupported inputs.
   Three fixed seeds; same model/data for one-step and iterative controls.
   Compare ideal intermediate labels versus additional on-policy inputs labelled
   by an independent exact graph algorithm, never model output targets.
   Freeze threshold .9, max nodes/iterations 64 and .95 completion/.95 unreachable
   abstention floors before final evaluation. Report each family/size, not only
   averages; deterministic control must pass. Report score calibration separately.
3. GPU-fit BTN export (medium). Direct weight conversion, robust margin .05 over
   the explicit finite domain, coverage refusal beyond it, existing capsule
   export/import and growth replay. Bad conversion, weak margin and OOD tests
   must fail first. No fallback compiler may satisfy this slice.
4. Versioned candidate and request pinning (medium slices). Canonical fixed-width
   checkpoint, shape/dtype/feature/model versions, SHA256 and independent-evidence
   identifiers; reject malformed/truncated/nonfinite/incompatible files. Hashes
   detect corruption, not provenance authentication. Read-only immutable candidate
   snapshots pair with pinned capsule registry generation. Explicit shadow and
   regression checks precede owner-approved activation/rollback; trainer cannot
   certify or activate itself. Default certified runtime behavior stays intact.
5. Bounded isolated two-device workers (medium). One owned training worker and
   one immutable-snapshot evaluator, cancellation, partial-output refusal, death,
   restart and capacity refusal. No reservation or interruption of existing GPU
   jobs. Only private test registry activation is authorized by the current task.
6. Full source/capsule/health regressions, timings, sanitizer and adversarial
   file/metadata/promotion checks; record deployment authority separately.

## Explicit limitations to preserve in the report

The shared cell learns a bounded propagation rule, not language semantics or new
specialist knowledge by itself. Its small finite certification domain is not a
held-out accuracy population. Confidence is not a certificate. The neural graph
cap does not reduce the existing 4096-capsule registry limit: unsupported neural
inventories remain outside this experiment's admission claim. Actual capsule
execution enforces its typed contract and coverage at every hop, including after
hot swaps. No training on CNET's own Tier-A answers is allowed.

## Measured outcome and review corrections

All six fixed cells (three seeds, ideal/on-policy inputs) complete 640/640 positive
and abstain on 640/640 negative cases in each of two 1,280-case suites. This is
15,360 evaluated case-model pairs, NOT 15,360 unique graphs. The original suite
has uniformly productive positive graphs and goal-isolated negatives. Review
added a separately frozen stress suite: every graph contains productive/dead-end
regions, every positive offers a legal dead-end branch; cyclic traps, interior
cuts, unavailable nodes and incompatible edges are included. No weights were
retuned for this added suite. Chain cases reuse topology under node permutations
and metadata changes. Both variants tie; no on-policy benefit is demonstrated.
One-iteration controls complete only 25/640 positives; iterative controls use the
same weights and have up to 63-hop chains. Maximum family-size Brier is 7e-8.

Supervision erratum: original training logs said only `independent_BFS`, but the
2,000-epoch warm-up uses an explicit Boolean OR specification. Correction rounds
use independent BFS targets, including on-policy inputs. The complete local
recurrence is supplied by that warm-up; the experiment does not discover it.
Corrected training logs name both sources. Repeating all six fits after the
stable logit-space BCE fix yields byte-identical weights; no accuracy retuning.
Original raw logs/hashes remain historical; final source hashes identify the
corrected implementation. Native fixtures/weights are experimental data, not
portable model or capsule formats.

Independent review found saturated FP32 probabilities made probability-space
BCE incorrect (logit20/1000 reported ~69). RED/ GREEN tests cover both signs at
20 and1000; GPU loss now uses stable logit-space softplus. The independent
finite-difference gradient error is 1.329e-7 on both devices.

Deterministic BFS is ~140–195 times faster than graph-cell propagation on these
small fixtures. Actual one-capsule serving has ~3us deterministic versus ~10us
opt-in cell median latency. Accordingly deterministic serving stays the default.
The neural serving adapter admits at most62 capsules plus two endpoint nodes;
the unchanged deterministic inventory supports4096. Scores never bypass a
per-hop audit, contract or coverage check.
