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
- [x] Variable-size structured selection with relabel, family, long-chain and
      independent-label/on-policy ablations; deterministic control retained.
- [x] Versioned core candidates, corruption/incompatibility refusal, shadow
      evaluation, request-pinned activation and rollback.
- [x] One genuinely GPU-fit finite task through existing BTN/capsule certification,
      export/import, OOD refusal and growth replay; no finite compiler substitution.
- [x] Bounded two-device worker lifecycle, cancellation/failure/restart tests,
      explicit guarded promotion in an isolated registry.
- [x] Final source regressions, runtime benchmarks, sanitizers and adversarial
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

## Slices 3–6 — bounded local pipeline

Shared-cell protocol/results are recorded in `cnet_shared_selector_20260906.md`.
The cell is41 FP32 scalars (164bytes); its direct BTN conversion has eight sigmoid
hidden units. Six fitted models pass exhaustive eight-row Boolean OR certification
with the unchanged .05 margin (observed minimum around .492), existing capsule
export/import and growth replay. Payload is861bytes, excluding the manifest and
runtime allocation. This finite certification domain is not held-out accuracy.
An independently worker-trained zero-initialized cell also passes with .489 margin.
There is no finite-domain compiler fallback in this path.

The404-byte canonical core checkpoint binds schema/model/feature/dtype/shape,
unapproved status,41 LE binary32 weights and two evidence SHA256 identifiers.
Content SHA256 uses the existing CCE implementation; no crypto dependency or
second capsule format. Owner-private dirFD/file checks, single-component names,
no-follow/exclusive writes, exact reads, nonfinite/incompatible refusal and
every-byte corruption tests pass. Recomputed hashes do not bypass version/shape/
status/nonfinite checks. Integrity is not authenticity.

The opt-in host owns at most four immutable model+registry generations and64
leases, with one staged candidate. Explicit activation follows independently
computed graph task/calibration gates, all shadow labels and old/new capsule
history replay. Rollback invalidates pending approval; old requests retain their
old model and capsule inventory. A global wrapper mutex serializes the legacy
global certificate cache and BTN scratch; raw legacy APIs must not be used
concurrently outside it. The owner stops new operations before destruction.

Review caught a real promotion bypass: deterministic replay can cover two
same-port capsules with disjoint input coverage, while a structural neural path
chooses only one. The retained RED case proves the old admission passed despite
loss of covered inputs. Promotion now replays every sealed-label join through
the actual staged neural execution path as well as deterministic consistency.
This case refuses even when a small supplied shadow set omits the lost rows.

Worker pool: two jobs, one per discrete ordinal0/1, fixed tiny task, max2,000
training epochs per job, 30CPU-seconds, owner timeout<=60seconds,1024FD limit and
16MiB per-file size. Requested cell device state is385,192bytes; HIP/runtime/code
allocations are additional. The measured child maximum RSS is~215MiB; neither
driver RSS, total scratch quota nor hostile driver isolation is hard-guaranteed.
Snapshots cross a sealed memfd and return in an exact192-byte private IPC record;
EOF, clean child exit, finite fields and CPU/GPU prediction parity are mandatory.
Neither worker has the parent activation interface or serving registry paths.

Landlock ABI>=3 applies before HIP creates threads. Only GPU devices, `/dev/null`
and a job-owned0700 scratch directory accept filesystem writes. Tracing isolated
HIP startup found COMGR needs a temporary compilation directory; denying its mkdir
caused a HIP null-refcount SIGSEGV. The fix supplies a private `TMPDIR`, not write
permission to `/tmp` or serving storage. Job scratch is reclaimed only after
reaping, via bounded descriptor-relative traversal without following symlinks;
unexpected contents are retained loudly. Network and process/metadata restrictions
use TSYNC seccomp after warm-up. Review reproduced then closed read-only-FD
filesystem ioctl and queued-signal bypasses; only DRM/KFD ioctl namespaces remain.
Trusted compiled HIP startup is part of the trust base; this is not a general
hostile-native-code execution service. No shared GPU job is reserved or stopped.

Lifecycle tests cover capacity, immutable transfer, explicit cancellation, child
exit, truncated output, timeout and restart. End-to-end `make -C
experiments/offline_controller product-test` additionally converts the worker
snapshot into a capsule, imports it, checks guarded activation/pinning/rollback,
and benchmarks actual verified requests. It leaves only private test artifacts
under `/tmp`; no live service or registry is activated.

Final security closure distinguishes new-code tests from the existing managed
control-plane dependency advisory; see the final result report. An audit finding
is not converted into a clean security claim. No external Claude/Grok CLI was
invoked: exact-command offers remain unanswered; bounded independent reviewers
and negative tests supplied the recorded findings.

Final closure: fresh 28-suite `make verify`, 94-check capsule ASan/UBSan, new
candidate/selector/host sanitizers, both-device experimental regressions,
64-to-66 capsule capacity and an empty-output-directory product build all pass.
The initial capability gate correctly refused concurrent worktree changes; its
frozen rerun certifies all six existing capabilities at checkpoint449352a, with
unchanged floors. Fresh rocBLAS median training is21.742ms GPU0 and33.982ms GPU1,
versus57.693/57.424ms four-thread OpenBLAS; shared-GPU variability is retained.
The requested audit is complete, but security release clearance remains WITHHELD
for the existing control-plane SQLite High advisory. Details, concrete separate
remediation and deployment limits: `result/cnet_gpu_product_sequence_20260906.md`.
