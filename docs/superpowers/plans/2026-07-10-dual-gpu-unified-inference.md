# Dual-R9700 Unified Async Runtime Implementation Plan

> **For Hermes:** Execute with vertical RED→GREEN tracer bullets and one final
> H0/H1 gate. Preserve both dirty repositories and do not cut over port 8081
> before staged acceptance.

**Goal:** Add a work-conserving 2+ lane native runtime to CNET, prove both
R9700s execute independent certified backend calls concurrently, and stage DS4
as the dual-GPU universal inference backend without a second planner.

**Architecture:** Oracle v2 remains the per-call semantic authority. An opaque
pthread lane pool owns queue/job memory but borrows one Oracle entry per worker.
GPU tests bind one `cce_clgemm_open_device()` handle per lane. DS4 remains a
modular engine reached through its public server/distributed interface.

**Tech stack:** C11, pthreads, OpenCL/ROCm, DS4 ROCm, .NET only for existing
host projection.

---

### Task 1: CPU asynchronous lane contract

**Files:**
- Create: `include/async_runtime.h`
- Create: `src/async_runtime.c`
- Create: `tests/test_async_runtime.c`
- Modify: `include/acquire.h`
- Modify: `Makefile`

1. Write a test with two 20 ms Oracle v2 callbacks and eight queued jobs.
2. Assert ticket uniqueness, ordered collection, both lanes used, exact output,
   per-job evidence, and elapsed time below the serial lower bound.
3. Run the test before implementation; expect missing-header failure.
4. Implement the smallest opaque pool and rerun to GREEN.
5. Wire `unified_async` into `make unified` immediately.

### Task 2: Backpressure, cancellation, and deadlines

**Files:** same as Task 1.

1. Add failing tests for queue full, queued cancellation, already-expired job,
   wait timeout, invalid ticket, and clean close.
2. Append explicit Oracle statuses for cancelled/deadline exceeded.
3. Implement fixed-capacity job slots, conditions, and monotonic timestamps.
4. Confirm no input/output lifetime dependence on caller memory.

### Task 3: Dual-R9700 real backend lanes

**Files:**
- Create: `tests/test_async_gpu_lanes.c`
- Modify: `Makefile`

1. Write a test that opens `cce_clgemm_open_device(0)` and `(1)` separately.
2. Register one Oracle v2 callback per handle and submit enough independent
   deterministic matmuls through the lane pool.
3. Verify every output against the same k-ascending CPU reference and require
   nonzero work on both lane indices.
4. Report serial GPU0 and two-lane wall throughput; do not make a brittle speed
   assertion while GPU1 has unrelated load.
5. Add `make unified_gpu`; require exactly two discrete devices and reject the
   unified-memory iGPU.

### Task 4: Recoverable DS4 dual-GPU staging

**Files:**
- Create: `scripts/run_cnet_ds4_dual.sh`
- Create: `config/cnet-ds4-dual.env.example`
- Modify: `DS4_EXTRACTION.md`
- Modify: `plans/delegation_master_report.md`

1. Validate binaries/model paths and free VRAM before launch.
2. Start the DS4 distributed worker on GPU1 and coordinator API on GPU0 using
   PID files, separate logs, bounded readiness polling, and cleanup traps.
3. Keep port 8081 untouched; stage on 8082/8100.
4. Emit a machine-readable readiness record containing model path, SHA-256,
   DS4 binary hash, topology, PID, and endpoint.
5. Provide explicit `start`, `status`, and `stop` actions; never use nohup or
   orphan shell-level backgrounding in tests.

### Task 5: Closure and benchmark

**Files:**
- Modify: `README.md`
- Create: `plans/phase8_dual_gpu_async_runtime.md`
- Modify: `plans/delegation_master_report.md`

1. Run focused CPU and GPU gates.
2. Run `make test && make unified && make build` once after all source edits.
3. Benchmark serial versus two-lane throughput and report both GPU occupancy,
   queue latency, and external GPU1 contention.
4. Run DS4 staging only after native closure. If port 8081's llama service must
   be stopped for a real dual-GPU run, first preserve its exact command and do
   not redirect AICIMO until DS4 passes health and output gates.
5. Final H1 requires real source, real two-GPU execution, and all gates green.
