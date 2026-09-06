# GPU product execution ledger

User requested the complete next sequence, benchmarks and vulnerability checks.
Work stays on `experiment/offline-controller-20260906`; no live GPU reservation,
service interruption, master push or deployment is implied. Existing certification
floors, independent labels and final per-hop verification remain mandatory.

## Ordered completion gates

- [x] Resident FP32 forward/update path, immutable snapshots, sustained parity.
- [ ] Batched/private-stream execution; measured CPU/host-GPU/resident comparison.
- [ ] Optimized CPU and supported ROCm matrix/precision experiments, keep only
      improvements that retain numeric and held-out task gates.
- [ ] Variable-size structured selection with relabel, family, long-chain and
      independent-label/on-policy ablations; deterministic control retained.
- [ ] Versioned core candidates, corruption/incompatibility refusal, shadow
      evaluation, request-pinned activation and rollback.
- [ ] One genuinely GPU-fit finite task through existing BTN/capsule certification,
      export/import, OOD refusal and growth replay; no finite compiler substitution.
- [ ] Bounded two-device worker lifecycle, cancellation/failure/restart tests,
      explicit guarded promotion in an isolated registry.
- [ ] Final source regressions, runtime benchmarks, sanitizers and adversarial
      vulnerability/dependency review; remaining live deployment authority explicit.

## Initial resident contract and threat model

The existing fixed-shape experimental `Net` remains the numerical reference.
An opaque resident owner keeps weights and scratch on its selected discrete AMD
device. It accepts bounded batches of finite features and normalized finite label
distributions, returns status/loss, and only exports weights on explicit snapshot.
Snapshots are copies; later training must not mutate them. One caller owns each
mutable context. Immutable snapshots may be handed to separate evaluator workers.

Assets: active core/capsule integrity, independent evidence, host/device memory,
service availability. Boundaries: caller dimensions/features/labels, checkpoint
files/manifests, GPU errors, candidate-to-active promotion and registry changes.
Abuse tests: zero/oversized batches, nonfinite or malformed labels/weights, invalid
device, null outputs, corrupt/truncated or symlinked files, identity substitution,
stale registry generation, partial updates, cancellation and memory exhaustion.
GPU failure poisons a private training candidate; it cannot become a serving
snapshot. Training workers never write active weights or certify their own output.

No new network service is needed. Cross-model review, if invoked, receives only
the relevant artifact/contract with tools disabled, using existing authentication.
All changes are reviewed before their checkpoint commit. A missing linked skill
Definition-of-Done file is replaced by the explicit gates above, not skipped tests.

## Slice 1 — resident FP32 checkpoint

Accepted in the experimental worktree only. `resident.h/cpp` owns six bounded
device allocations (1,086,028 requested bytes, excluding HIP runtime allocations),
reusing the existing device-pointer matrix kernels. The existing pool does not
expose borrowed device pointers, so this fixed-size owner is not another general
pool or capsule format. The original CPU/reference sources remain unchanged.

RED: missing resident interface, then training stub refusal. Initial constant-batch
stress parity also failed; that diagnostic is retained, not reclassified as a
passing gate. Corrected the sustained test to the **existing** eight-minibatch
RNG471, LR .15, 128-update protocol with final batch-0 prediction comparison.
Both GPUs pass the unchanged 1e-4 threshold, weight maximum error 1.1920929e-7.
Depth 1–4, tied/untied, successive batches 1/17/128, invalid labels, independent
snapshot immutability, finite-input overflow and poisoned-context refusal pass.
Runtime failure injection tests the error-return branch, not an actual GPU reset.

Three alternating-order 128-update runs on shared hardware: median resident time
48.19 ms on GPU0 and 61.52 ms on GPU1, versus host-GPU 182.69/195.82 ms and scalar
CPU 731.85/729.08 ms. This is **not yet an optimized CPU comparison**. Final losses,
weights and batch-0 probabilities pass parity before a timing record is accepted.
Review found and fixed NaN masking in maximum-error checks and missing final
probability comparison. No remaining implementation blocker reported by reviewer.

The isolated 24-update profile records 98 synchronous copies: 4/update plus initial
weights and explicit snapshot. Training transfers are 78,344 bytes/update versus
1,825,472 previously. Requested device state is approximately 1.04 MiB. There are
still 48 device-wide waits (2/update); slice 2 replaces those with private-stream
ordering. API durations include waiting and initialization, not pure copy cost.
Raw timing, diagnostic and profiler evidence lives in
`result/cnet_gpu_product_evidence_20260906/resident_sync/`.

Numerical limitation: repeated **single** batch RNG711 at LR .15 amplifies FP32
differences after ~30 updates; at update 37 even CPU versus the old host-GPU loss
differs by .0188. Resident loss differs by .000249 there. Arbitrary long unstable
trajectories are not bitwise reproducible. Keep the diagnostic and task-level
confirmation gates; never widen a certification threshold to hide it. HIP math
functions and floating-point contraction can differ from host evaluation; see
[AMD HIP math API](https://rocmdocs.amd.com/projects/HIP/en/develop/reference/math_api.html).

Cross-model review was offered with an exact tool-disabled Claude command; no
response has arrived. Internal adversarial review and regression tests completed.
No live services, training jobs, core activation, capsule registry or master were
changed by this slice.
