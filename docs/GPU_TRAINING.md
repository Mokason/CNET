# AMD training and guarded core candidates

This is an opt-in, bounded experiment in
[experiments/offline_controller](../experiments/offline_controller/).
It does not install a live controller or replace default deterministic serving.

## Run the working sequence

```sh
make -C experiments/offline_controller product-test
```

Requirements: HIP/ROCm for the two discrete `gfx1201` AMD devices, a C compiler,
and Linux Landlock ABI >=3. The target builds the native capsule library and
experimental binaries, then creates a private temporary registry. Its sequence
is: GPU fit, immutable transfer, CPU/GPU parity, direct BTN conversion, existing
capsule certification/import, shadow/history checks, activation, pinning and
rollback. It tests normal workers separately from fault-injection workers.

A queued or completed training job is not an approved candidate. The owner must
evaluate and explicitly activate it. No worker receives the host activation API.

## Three different workloads

| Workload | What it establishes |
| --- | --- |
| Larger fixed recurrent network | Resident forward/update parity and CPU/OpenBLAS/ROCm timing |
| Six shared-cell graph models | Frozen bounded graph-selection tests and iteration/on-policy controls |
| Worker demonstration cell | Eight-row Boolean OR fit through existing BTN/capsule and host lifecycle |

Do not transfer a speedup or accuracy result from one row to another.
The shared cell has three inputs, eight sigmoid hidden units, one output and
41 FP32 weights. The six graph models use Boolean warm-up plus independent BFS
correction labels. The worker demonstration uses only the eight Boolean rows
for 2000 epochs. Neither trains on CNET Tier-A answers.

The graph template applies that supplied local rule repeatedly. It is not
language understanding, an independently discovered algorithm or evidence of
broad task acquisition. Exact BFS remains a much cheaper control on these small
graphs, and both ideal/on-policy variants tied in the recorded experiment.

## Measured September 6 checkpoint

Fresh larger-network medians, 128 updates at batch 128 and three rotated-order
repetitions: GPU0 21.742 ms versus four-thread OpenBLAS 57.693 ms (2.65×);
GPU1 33.982 ms versus 57.424 ms (1.69×). GPU1 included a 70.433 ms run.
These are shared-machine measurements, not reserved capacity.

Maximum sustained rocBLAS weight error was 6.85453415e-7 at the unchanged 1e-4
floor. Sampled FP16/BF16 forward results failed that floor and remain unadmitted.
An unstable repeated-single-batch diagnostic is retained rather than hidden.

On the one-capsule serving test, median-of-p50 latency was 2.624 us deterministic
versus 9.949 us neural. Deterministic serving therefore stays the default.
The [full report](../result/cnet_gpu_product_sequence_20260906.md) contains raw
runs, graph-suite limitations, negative controls and source identities.

## Candidate and activation contracts

- [Core checkpoint](../include/cnet_core_candidate.h): canonical 404-byte record,
  fixed versions/shape/FP32 representation, two evidence hashes and content hash.
  Loads refuse corruption, incompatible metadata, nonfinite weights, unsafe
  file types/permissions and wrong length. A hash is not authentication.
- [BTN conversion](../include/cnet_cell_capsule.h): direct FP32-to-double weight
  copy for the supported 3-bit-to-1-bit layout. Conversion does not certify.
- [Host](../include/cnet_core_host.h): one staged candidate, four loaded
  generations, 64 leases, explicit check/activate/rollback/discard.
- [Neural adapter](../include/cnet_core_selector.h): at most 64 graph nodes,
  corresponding to 62 capsules and two endpoints in the serving adapter.
  Deterministic inventory capacity remains 4096.

Promotion binds the evidence to the staged candidate and current active
generation, checks graph completion/abstention and calibration, validates shadow
labels and replays every relevant sealed-label join through actual proposed
neural execution. A small shadow set cannot excuse lost historical coverage.

Pinned requests retain both their model and capsule inventory. The wrapper
serializes legacy mutable execution state; raw legacy calls must not race it.
Rollback invalidates staged approval. This is in-memory lifecycle support, not
durable daemon rollout or operator-named persistent capsule sets.

## Resources and failure behavior

One worker per discrete device, at most two jobs. Snapshot input uses a sealed
memfd; exact output size, EOF, clean exit and finite/parity checks are required.
Cancellation, timeout, truncated output, child death and restart have tests.

Cell tensor state requests 385,192 device bytes. Measured child maximum RSS was
about 215 MiB, not the total of simultaneous workers. HIP/code/runtime costs are
additional to the 164 model bytes. The larger rocBLAS owner has different tensor
and workspace costs; consult its report instead of applying the tiny-cell size.

Landlock protects writes before HIP creates threads; private compilation scratch
is allowed. Network/process restrictions apply after trusted warm-up. The driver
remains trusted, filesystem reads are allowed, and total RSS/disk are not hard
quotas. See [security](SECURITY.md).

## Further tests and build targets

```sh
make -C experiments/offline_controller cell-test selector-test candidate-test
make -C experiments/offline_controller resident-test rocblas-test stream-test
make amdmath_test
```

`selector-experiment`, `rocblas-benchmark`, `matrix-benchmark` and
`rocblas-train` build bounded tools; inspect their argument checks before runs.
OpenBLAS comparison builds accept `OPENBLAS_CFLAGS` and `OPENBLAS_LIBS`.
Do not reset shared GPUs, change clocks or interrupt other jobs to obtain a
cleaner timing result.
