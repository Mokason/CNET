# GPU candidate product sequence

Status: proposed next product work; investigation evidence is in
`result/cnet_controller_investigation_20260906.md`. Decision records stay here,
not in a second ADR/task tree. No certification floors are changed.

## 1. Device-resident training of the existing bounded candidate

Reuse `src/cce/amdmath/cce_amdmath.cpp` device-pointer APIs and memory pool.
Keep weights, activations, derivatives and optimizer state on the selected AMD
device across steps. Upload batches and download scalar metrics or explicit
checkpoint snapshots only. Preserve the existing CPU reference. First implement
one forward/update path, then add batched asynchronous execution; do not combine
precision changes with residency changes.

- Acceptance: RED sustained-update parity test first; FP32 CPU/current-GPU/resident
  parity at the existing 1e-4 diagnostic tolerance, unchanged task floors.
- Acceptance: profiler demonstrates removal of per-layer host round trips;
  publish end-to-end speed and VRAM at identical batch/data/update budgets.
- Verification: original experiment suites, sustained parity on both AMD devices,
  immutable-snapshot test and error-path refusal. Estimated scope: medium slices
  of 3–5 files, split forward residency from optimizer residency.

## 2. Matrix throughput and precision, after residency

Benchmark a batch/shape sweep against the scalar reference **and an optimized CPU
baseline**. Reuse compatible ROCm GEMM facilities, checking gfx1201 support locally.
Only then test BF16/FP16 multiplication with FP32 master weights/accumulation.
Do not pad or quantize stored public capsule representations to match a kernel.

- Acceptance: publish wall time, copy/kernel breakdown, CPU baseline and memory;
  record shared versus exclusive hardware conditions. No automatic floor override.
- Acceptance: separately frozen accuracy and abstention retain their floors;
  nonfinite gradients, unsupported dimensions or GPU failure fail loud.
- Verification: numerical parity before performance claims. Depends on 1;
  one isolated backend intervention per comparison, small/medium slices.

## 3. Variable-size capsule selection experiment

Replace fixed absolute action slots with a shared structured node/edge encoder and
bounded iterative processor. Input contains available capsule identity/version,
typed ports, compatibility and verified execution state, never oracle answers.
Output is a ranked proposal plus calibrated abstention—not a certificate. The
existing typed planner/verifier remains authoritative and deterministic control
remains in every benchmark.

- Acceptance: node-relabel tests and unseen graph/capsule sizes; compare recurrence
  versus nonrecurrent controls under the improved, matched data regime.
- Acceptance: independently labelled intermediate/tool-result data, with separate
  on-policy correction ablation; never train on CNET Tier-A answers.
- Verification: frozen family/size/long-chain/abstention suites, not aggregate
  coverage alone. Depends on 1 for efficient experiments, but no prerequisite
  that mixed precision win. Split encoder, processor and datasets into medium tasks.

## 4. Typed, versioned core candidate activation

Define one additive core checkpoint contract: schema/model/feature versions,
shape/dtype, content hash, independent training-evidence digest, evaluation manifest
and candidate status. This is a core-model artifact, not another capsule package.
Stage a read-only candidate; pin one version for an entire request, shadow-evaluate
it, then make activation an explicit guarded operation with rollback. A capsule
registry generation change must not change an in-flight request's interpretation.

- Acceptance: RED malformed/incompatible/corrupt-checkpoint refusal and old/new
  request-pinning tests before a serving hook; GPU failure cannot weaken guards.
- Acceptance: offline regression/calibration and shadow gates before approval;
  no trainer may directly mutate active core weights or certified registry state.
- Verification: source `make verify`, covered task gates, crash/rollback tests and
  resource budgets. Depends on 3; split schema, shadow path and activation into
  separate medium tasks. Live service changes require explicit deployment scope.

## 5. One genuine GPU-fit-to-capsule vertical slice

Choose one finite, typed task with an independent exact verifier. Train a GPU
candidate or distil verified proposals into the existing BTN representation.
Measure the GPU-trained/converted BTN **before any finite compiler fallback**.
Continue through the existing contract, coverage and capsule export/import path.

- Acceptance: RED bad conversion/margin/OOD refusal tests first; all certified
  rows retain existing robust margin .05, and out-of-coverage inputs abstain.
- Acceptance: export/import round trip and growth replay pass without modifying
  other units; label provenance stays independent. Report `finite_domain_compile`
  explicitly if used; it cannot satisfy a claimed learned-compression result.
- Verification: `make knowledge_capsule`, `make knowledge_accumulation_bench`,
  `make coverage_abstain`, `make knowledge_composition_bench`, `make capability_cert`.
  Depends on 1 and existing capsule interfaces, not on 4. Medium task slices.

## 6. Two-device worker lifecycle and promotion

After approved scheduling scope: reserve one GPU for training and one for frozen
candidate evaluation/shadow workloads, with bounded RAM/VRAM, cancellation and
snapshot transfer. Existing jobs already occupy both cards; this document does
not authorize pausing them. For this tiny model, independent seed/evaluation jobs
are the first useful two-GPU division; synchronized multi-GPU training must earn
its communication cost in a later benchmark.

- Acceptance: no partial checkpoint activation, no direct trainer writes to serving
  weights, no automatic admission after a training-loss improvement.
- Acceptance: failure/restart/saturation tests preserve service behavior; promotion
  requires all unchanged task, abstention, interference and history gates.
- Verification: `make own_learning_health`, source regression and pinned-version
  capsule-swap tests, then explicitly approved deployment. Depends on 4 and/or 5.

## Checkpoints and boundaries

After 1–2, review measured speed/parity before enlarging the network. After 3,
review transfer evidence before adding a serving hook. After 4–5, review artifact
and regression evidence before enabling worker promotion. There is no claim that
floating-point computation itself requires GPUs; the measured workload benefit,
training scale and service budget determine placement.
