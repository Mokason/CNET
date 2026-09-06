# GPU product execution ledger

User requested the complete next sequence, benchmarks and vulnerability checks.
Work stays on `experiment/offline-controller-20260906`; no live GPU reservation,
service interruption, master push or deployment is implied. Existing certification
floors, independent labels and final per-hop verification remain mandatory.

## Ordered completion gates

- [x] Resident FP32 forward/update path, immutable snapshots, sustained parity.
- [x] Batched/private-stream execution; measured CPU/host-GPU/resident comparison.
- [x] Optimized CPU and supported ROCm matrix/precision experiments, keep only
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

## Slice 2 — private streams, matrix backend and full-training regression

The additive `cce_amdmath_*_f32_stream` C ABI borrows a HIP stream, enqueues only,
and validates dimensions/grid/byte-count arithmetic. Library version is 0.3.0.
Legacy valid `_dev` semantics remain: synchronous SGD, including finite negative
learning rates. RED compilation tests preceded the stream and batched interfaces.
Review caught a shared-grid-limit bug and the accidental rejection of legacy
negative rates; both are fixed, including a real 1,048,561-row SGD regression.

Resident calls now use a private nonblocking stream and pinned bounded staging
for 1–8 sequential minibatches. All inputs are checked before submission. Error
paths attempt to drain queued work, retain the original error and poison the
owner. A faulting runtime can itself refuse a drain; no successful completion is
claimed in that case. A new injected-after-enqueue test observes stream idle and
snapshot refusal. It does not reset or fault the shared physical GPU.

The original scalar network is also instantiated with OpenBLAS substitutions for
its two matrix calls, avoiding a copied reference algorithm. OpenBLAS 0.3.26
Ubuntu packages were downloaded/extracted under `/tmp`, not globally installed.
`OPENBLAS_CFLAGS` and `OPENBLAS_LIBS` select this benchmark-only dependency;
normal builds need no OpenBLAS. Package SHA256s:

- `libopenblas0-pthread_0.3.26+ds-1ubuntu0.1_amd64.deb`:
  `7dc3b4384c02aecb87eb8b70fa26c5843a08af242f4638aa4b36922bdc4f5b04`
- `libopenblas-pthread-dev_0.3.26+ds-1ubuntu0.1_amd64.deb`:
  `01ae4d0433927e4109ae614c047f9df55dc3c8515e6369423f36f5e18d59101e`

Measured interventions, three rotated-order repetitions, same 128 updates/batch
128: private-stream naive FP32 ~35 ms, versus OpenBLAS 1-thread ~63 ms and
4-thread ~58 ms. Optional rocBLAS FP32 with bounded workspace further reduces
median GPU0/GPU1 time to **21.48/21.40 ms**; the simultaneous-run CPU baseline
is recorded in `resident_rocblas/bench_gpu*.jsonl`. All loss/weight/final-probability
checks retain 1e-4. Maximum sustained weight error is 6.85453415e-7 for rocBLAS.
No timing is an exclusive-hardware capacity claim.

Six matrix shapes per device span N=1..2048, input=65/209/512, output=16/64/512.
Both FP32 backends pass forward and FP32-master SGD checks. FP16 and BF16
multiplication/FP32 accumulation run on both cards, but **all sampled shapes fail
the unchanged 1e-4 forward gate**. Their speed excludes conversion cost; they are
not admitted and have no claimed task-accuracy result. This is evidence of working
GPU matrix acceleration, not grounds to weaken a certification floor. Primary
references: [rocBLAS design notes](https://rocmdocs.amd.com/projects/rocBLAS/en/latest/conceptual/rocblas-design-notes.html),
[OpenBLAS runtime controls](https://www.openmathlib.org/OpenBLAS/docs/runtime_variables/).

The 24-update private-stream profile has 12 training async copies (4 per group of
8), one snapshot copy and one initial synchronous weights upload. No
`hipDeviceSynchronize` occurs. Five private-stream waits are 3 groups + snapshot
+ close. Tensor allocations total 1,634,412 bytes and pinned batch 626,724 bytes.
rocBLAS receives a user-owned 16 MiB workspace, but also allocates a separate
25 MiB internal buffer and transient 32 MiB during handle creation: tracked device
allocation peak ~74.56 MiB, steady ~42.56 MiB. Runtime/code/cache allocations are
not equated with model parameters. Cold process maximum RSS measured 303,580 KiB.

Both resident backends retrained the fixed three seeds for 30,000 updates, with
3,840,000 accepted independently BFS-labelled samples per seed and unchanged
data partition/feature transform/LR. This reruns an **already observed frozen
suite**, not a newly unseen holdout. The retained frozen evaluator reads the
snapshots and identifies their SHA256. Independent Floyd-Warshall/transition
audits check complete traces, counters and unchanged .95/.95 floors.

- Naive resident: completion 1262/1268/1254 of 1284, unreachable abstention
  761/757/761 of 764; no invalid accepted transitions in 19,684 checked actions.
- rocBLAS resident: completion 1261/1268/1254 of 1284, abstention 760/759/763 of
  764; no invalid accepted transitions in 19,624 checked actions.
- Masked/unmasked results agree. Long-chain sparsity remains a limitation of this
  old suite; the next slice must test longer chains explicitly.

Commands: `make stream-test resident-test rocblas-test`, `make amdmath_test`,
`make test gpu-test investigate-test` in their respective root/experiment
directories. `make optimized-benchmark matrix-benchmark rocblas-benchmark`
builds benchmarks; `make resident-train rocblas-train` builds bounded offline
trainers. Both trainer binaries take `SEED DEVICE UPDATES VALIDATION_TSV` in a
fresh output directory. `audit_resident.mjs FROZEN_EVIDENCE OUTPUT_DIRECTORY`
checks the regression trace. Raw evidence is under `resident_async/`, `matrix/`,
`resident_confirmation/` and `resident_rocblas/` in the product evidence tree.
Earlier managed-workspace measurements are retained separately, not overwritten.

Final stream/rocBLAS adversarial review reports no substantive unresolved issue.
The optional exact-command Claude offer remains unanswered; no cross-model CLI
has been invoked. Full-source/security closure remains in the final product gate.
An interim fresh `make verify` passed all 28 logged suites after the stream API
change (`source_regression_after_streams.log`); final regression must run again
after the remaining product slices.
