# Dual-R9700 Unified Asynchronous Inference — Design

**Date:** 2026-07-10
**Status:** Implementation in progress
**Hardware:** 2× AMD Radeon AI PRO R9700 (`gfx1201`), iGPU excluded

## Place, dilemma, consequence

**Place:** The boundary between CNET's certified specialist runtime and the
actual inference engines that own GPU queues, model state, and KV timelines.

**Dilemma:** Splitting every skinny decode GEMM across both cards adds queue
round-trips and was measured slower, but assigning only one request to one card
leaves the other card idle. DS4 has the opposite shape for large MoE prefill:
one logical request needs both PCIe links and a chunk conveyor. A single policy
cannot optimize both workloads.

**Consequence:** Make *lane topology* explicit. CNET uses work-conserving
request lanes for independent work and a resource mask for pipeline backends
that consume both GPUs. Backends remain modular; CNET owns admission, status,
evidence, queueing, deadlines, and telemetry.

## Decisions

**Decision: request-level parallelism is the default for skinny decode.**
One backend instance and one queue per R9700 avoids queue races and preserves
the measured 2.6× campaign gain; column-splitting remains available for truly
wide/fat operations but is not the default throughput policy.

**Decision: DS4 is an engine backend, not a second CNET planner.**
Use DS4's public `ds4_engine`/`ds4_session` API or its OpenAI-compatible server.
Its distributed coordinator/worker route is one CNET lane with resource mask
`GPU0|GPU1`; DS4's prefill window supplies the internal asynchronous conveyor.

**Decision: the iGPU is excluded by capability.**
CNET keeps using `CL_DEVICE_HOST_UNIFIED_MEMORY == 0`, not product-name matching.
A forced device list remains an explicit override.

**Decision: no cutover before shadow acceptance.**
The existing llama.cpp service on `127.0.0.1:8081` remains live until a staged
CNET/DS4 endpoint passes model identity, API, correctness, throughput, failure,
and restart gates. AICIMO escalation is not redirected earlier.

## Systematic component

- Bounded FIFO queue and monotonically increasing tickets.
- One worker thread per lane; each lane borrows exactly one `OracleEntry` and
  therefore one backend context/GPU queue.
- Work-conserving shared queue: any idle lane takes the next eligible job.
- Exact port geometry checked before admission.
- Input copied at submit; output owned by the job until collection.
- Explicit queued/running/done state, cancellation, deadline, and backpressure.
- Per-job Oracle v2 result, lane index, queue latency, execution latency.
- Per-lane calls, successes, failures, busy time, and resource mask.
- Shutdown drains running callbacks and refuses new work.

## Irreducible/runtime component

GPU contention, model stochasticity, PCIe/NVMe jitter, thermal throttling, and
backend availability are measured rather than assumed. A deadline cannot
preempt an arbitrary in-flight GPU callback; cancellation is immediate while
queued and cooperative-at-boundary while running. The result records this
instead of pretending the compute vanished.

## Native ABI

New opaque runtime: `include/async_runtime.h`, `src/async_runtime.c`.

```text
cnet_lane_pool_open(specs[], lane_count, queue_capacity)
cnet_lane_pool_submit(input, counts, options) -> ticket
cnet_lane_pool_try_collect(ticket)
cnet_lane_pool_wait(ticket, timeout)
cnet_lane_pool_cancel(ticket)
cnet_lane_pool_lane_stats(index)
cnet_lane_pool_close()
```

Each lane spec carries:

- borrowed `OracleEntry *`;
- resource mask (`1<<0` GPU0, `1<<1` GPU1);
- stable lane name;
- flags reserved for future topology constraints.

Oracle v2 gains append-only `CANCELLED` and `DEADLINE_EXCEEDED` statuses. The
pool never collapses these into generic failure.

## Topologies

### A. Data-parallel lane pool

```text
request queue
   ├── lane 0 → CCE/DS4/CNET backend instance → GPU0
   └── lane 1 → CCE/DS4/CNET backend instance → GPU1
```

Best for independent decode requests, Oracle mining, evaluation, and batch
jobs. Each lane has independent model/session scratch and command queue.

### B. DS4 distributed pipeline lane

```text
CNET request
  → DS4 coordinator/API on GPU0 (layers 0:S)
  → asynchronous activation/chunk window
  → DS4 worker on GPU1 (layers S+1:output)
```

This is one logical CNET lane with resource mask `0b11`. DS4 already exposes
`--dist-prefill-window`, layer slicing, session cancellation, persistent server
APIs, and the mmap/quantized-weight model needed to avoid CCE's current bulk-f32
GGUF expansion.

## DS4 extraction boundary

Adopt behavior through stable boundaries, not wholesale source copying:

1. mmap quantized weights and direct quantized matmul;
2. ROCm gfx1201 WMMA/hipBLASLt kernels and plan cache;
3. distributed layer pipeline and prefill window;
4. session/KV API, cancellation, tokenizer, and sampler;
5. official-vector top-logprob validation.

CNET contributes typed contracts, identity, evidence, independent certification,
lane scheduling, lifecycle, and unified host projection.

## Replacement acceptance matrix

A CNET/DS4 endpoint may replace llama.cpp only when all are green:

1. `/v1/models`, `/v1/chat/completions`, streaming SSE, tool-call continuation,
   cancellation, and malformed-request handling.
2. Same GGUF bytes and prompt corpus: greedy token identity; every reference
   top-20 token represented within the configured logprob tolerance.
3. Single-request prefill/decode throughput recorded separately.
4. 1, 2, 4, and 8 concurrent requests: throughput, p50/p95/p99 first-token and
   inter-token latency, queue wait, both GPU utilization, VRAM, and power.
5. Worker loss, coordinator loss, timeout, queue-full, and restart recovery.
6. No iGPU allocation and no orphan process after failure.
7. CNET backend identity includes model bytes, prompt/template, sampler,
   DS4 build/toolchain, and topology configuration.

## Gates

- `make unified_async` — CPU/model-free 2+ lane semantics.
- `make unified_gpu` — opens exactly the two discrete R9700s, runs independent
  queues concurrently, and proves bit-identical results.
- `make test && make unified && make build` — ordinary regression closure.
- DS4 staging runs on a non-production port and never kills port 8081 until the
  replacement matrix passes.
