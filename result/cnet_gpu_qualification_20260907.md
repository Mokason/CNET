# Bounded dual-AMD worker qualification — 2026-09-07

Source checkpoint: `58e0ed7`, branch `feature/autonomous-learning-20260906`.
The owner explicitly approved running the two prepared gates concurrently with
existing services. Gates were run sequentially, with at most two children at once,
one per discrete AMD device. Device 0 is PCI `0000:03:00.0`; device 1 is
`0000:07:00.0`. No service was stopped/reconfigured and no GPU was reset/reserved.
No production policy, ledger or allocator activation setting was changed.

## Observed results

| Gate | Result | Wall seconds | Numerical evidence |
| --- | --- | ---: | --- |
| Production `test_workers … production` | PASS, exit 0 | 0.67 | Both devices trained; GPU0 snapshot evaluated on GPU1; final GPU0 evaluation max absolute error 0 |
| `allocator_worker_test` | PASS, exit 0 | 0.43 | Both devices trained on 16 fixture rows, 2,000 epochs; CPU/GPU max absolute error 2.98023224e-08 each; GPU0 snapshot evaluated on GPU1 |

Production test uses the 8-row Boolean fixture and checks the two-child capacity
limit, immutable snapshot transfer, immediate cancellation and subsequent restart.
This was the production binary, with `fault_injections=0`. Cancellation occurred
at logged elapsed time 0 ms; it does **not** prove cancellation during a live
kernel. The batch fixture also refuses invalid values/bounds and device 2 before
launch. GPU parity requires maximum absolute error strictly below 1e-6; no floor
was changed. All seven completed children produced valid 192-byte receipts with
exit 0; the eighth child was explicitly cancelled. Successful workers completed
in logged 214–234 ms, within their unchanged 30-second BOOTTIME deadline.

`/usr/bin/time` reported maximum RSS 220,556 KiB and 220,320 KiB respectively.
These are tool-reported process/child high-water values, not summed pool RSS,
hard quotas, sustained throughput or representative large-training measurements.
The fixture exports 164 bytes of native floating-point weights, SHA256
`ce5fc520a1d200b8df03244fc45e45da7d81ed2105d9de9ae8fa87aa83c55668`.
That raw test artifact is **not** a certified capsule or deployment candidate.

## Continuity observations and limits

Before: GPU0 100%, GPU1 3%; VRAM 3,947,483,136 and 8,089,628,672 bytes.
After: GPU0 100%, GPU1 3%; both VRAM values unchanged. Six named user services
(`cnet-mouth-8083`, `bonsai-server`, `bonsai-compete-rocm`,
`hermes-offline-mistral`, `cnetd`, `cnet-learning-private-eTgwwG-daemon`)
retained identical PIDs, start timestamps, active/running states and zero restart
counts. No worker remained in the post-test process snapshot. HTTP `/health`
on ports 8080, 8081, 8083 and 8092 returned 200. These are post-run liveness checks,
not proof of unchanged inference latency or correctness during concurrent load.

A kernel-journal scan since `2026-09-07T18:57:08Z` returned `-- No entries --`
(exit 1) for the pattern
`amdgpu.*(fault|reset|timeout|error)|ring.*timeout|GPU.*(fault|reset)`.
The first diagnostic used unsupported `--kernel`; its failure is retained and
the corrected `journalctl -k` scan returned the result above. A narrow log scan
is not proof of absence of every driver fault. Physical suspend/resume, forced in-flight GPU
faults, hard-real-time driver teardown and long-running reliability remain
WITHHELD. The prior 22 CPU boundary cases are separate evidence.

The useful learned-allocator gate is still **failed**, not rerun or retuned:
frozen gain −0.001953125 vs required 0.05; paired-95% lower bound −0.0178745509.
Allocator activation remains withheld. Next useful-controller work requires
eligible independent chronological evidence, development headroom over the
strongest fair controls and fresh whole-episode confirmation. These numerical
fixtures do not supply that evidence.

## Reproduction and receipts

Prepared binaries in `/tmp/cnet-gpu-qualification-20260907-nSt4GL/build` match
the prior recorded hashes. Only documentation changed under
`experiments/offline_controller` between build checkpoint `2eab134` and this run.
Current source/test/binary pins were captured before and checked after execution.
Assertions were enabled in the recorded build; no test-only fault worker was used.

Exact executed gates:

```sh
timeout --kill-after=5s 100s \
  /tmp/cnet-gpu-qualification-20260907-nSt4GL/build/test_workers \
  /tmp/cnet-gpu-qualification-20260907-nSt4GL/build/gpu_worker \
  /tmp/cnet-gpu-actual-20260907-MNJxgT/production-worker.native-weights production
timeout --kill-after=5s 75s \
  /tmp/cnet-gpu-qualification-20260907-nSt4GL/build/allocator_worker_test \
  /tmp/cnet-gpu-qualification-20260907-nSt4GL/build/allocator_worker
```

Use a fresh private output path for a repeat: weight export intentionally refuses
to overwrite an existing file. Each command was wrapped with `/usr/bin/time`;
stdout/stderr and exit codes were retained independently. Local raw receipts are
in `/tmp/cnet-gpu-actual-20260907-MNJxgT`: `inputs.sha256`, `pins-after.txt`,
`production-worker.log/.time/.exit`, `allocator-worker.log/.time/.exit`,
`gpu-before.csv`, `gpu-after.csv`, `services-before.txt`, `services-after.txt`,
the health responses and kernel scan. Temporary receipts are not durable runtime
artifacts. The [durable gate-log extract](cnet_gpu_qualification_20260907.log)
retains both complete test outputs and timing records.

Raw production log SHA256:
`8af8fc66f23d40f3fb0966e247f2bf23fdd737c7b2ce365ad6b869556b9e8f03`.
Raw allocator log SHA256:
`9107245e96a7d7bb612006b653c13445c014d5aedfb98932f7592799b04063df`.
Input manifest SHA256:
`a7db196cfe820563558d841c1dd351eee0bd6daa988efa0661df9c7dc6d60909`.

Astra-only adversarial evidence review corrected the pre-test utilization
snapshot and made the kernel scan's exit status explicit. No required findings
remained in this bounded report/receipt review; it was not a full security audit.
