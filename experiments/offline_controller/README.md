# Offline controller and GPU product experiments

This directory contains separate, bounded experiments, not a live service.
Start with the [GPU guide](../../docs/GPU_TRAINING.md) and
[current measured report](../../result/cnet_gpu_product_sequence_20260906.md).

## End-to-end product test

From the repository root:

```sh
make -C experiments/offline_controller product-test
```

The target builds the capsule library and test binaries, runs both discrete AMD
devices, converts a genuinely fitted cell to an existing certified capsule, and
tests independent admission, pinned activation and rollback in a private registry.
Artifacts remain under a new `/tmp/cnet-gpu-product-*` directory. No live registry,
GPU reservation, service restart or remote publication is performed.

Requirements are HIP/ROCm for `gfx1201`, Linux Landlock ABI >=3 and a C compiler.
Native worker jobs additionally require Linux >=6.9 thread pidfds, accessible
`/proc/self/fd` and `CLOCK_BOOTTIME` POSIX timers. Unsupported guards refuse;
there is no weaker fallback. Rebuild pool callers and native workers together:
the private entry contract now passes an absolute deadline and FD5 thread pidfd.
Override `OUT` for a separate build directory. Assertions must remain enabled.
Check device occupancy before running GPU targets; do not displace live jobs.

## Focused checks

Run these from this directory:

```sh
make test gpu-test investigate-test
make resident-test rocblas-test stream-test
make cell-test selector-test candidate-test worker-sandbox-test
make worker-boundary-test
```

The fixed recurrent network, six graph-suite cells and eight-row worker model
are different workloads. Shared parameters and filenames do not transfer one
workload's evidence to another. Native fixture/weight files are experiment data;
portable core checkpoints and certified capsules use their own existing APIs.
`make test` includes the CPU-only worker boundary regression and does not require
HIP or GPU access. It tests lifetime, deadlines and descriptor refusal, not GPU
math or physical machine suspension.

## Earlier experiment and investigation

`bash run.sh` runs the original frozen three-seed comparison, which failed its
initial generalization gate. `--local-baseline` additionally contacts the local
model at `127.0.0.1:8092`; use that only when intentionally comparing against the
already-running endpoint. It writes a new `/tmp/cnet-controller-run-*` directory.

`node audit.mjs ARTIFACT_DIRECTORY` checks recorded paths independently;
`bash test_audit.sh ARTIFACT_DIRECTORY` tests forged-acceptance refusal.
[INVESTIGATION.md](INVESTIGATION.md) and
[the original result](../../result/cnet_offline_controller_20260906.md) preserve
failed controls and the cause of the earlier hurdle. They are historical
evidence, not a current promotion instruction.

## Safety

Training uses independent labels, never CNET Tier-A answers. GPU errors poison
private candidates; no silent backend fallback or lowered floor can turn them
into accepted serving state. Worker completion is not approval. The default
deterministic core remains unchanged; [security limits](../../docs/SECURITY.md)
include the trusted HIP/driver boundary and the open control-plane advisory.

At native entry, before input or GPU initialization, workers bind their lifetime
to the exact spawning thread and arm a suspend-inclusive deadline. The thread
pidfd covers death before the parent-death signal was armed. Child stdio is
`/dev/null`; only the explicit result pipe is writable output. Parent lifecycle,
exit and signal records remain available, but worker runtime stderr is discarded.
The trusted dynamic loader precedes entry. SIGKILL and kernel/driver teardown
are not hard real-time GPU cleanup guarantees or an arbitrary-executable sandbox.
See the [September 7 expansion report](../../result/cnet_operational_expansion_20260907.md)
for current verification and the outstanding actual-device run.
