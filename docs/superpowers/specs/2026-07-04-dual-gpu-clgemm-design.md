# Dual-GPU Campaigns: Oracle Pool + Multi-Device GEMM — Design

**Date:** 2026-07-04
**Status:** Built and measured same session (user directive: "use both AMD AI
R9700 GPUs, do not touch the 3rd integrated GPU, make it even faster").
**Topic:** Use both discrete R9700s for extraction campaigns without moving a
single bit of any mined digest — and never touch the integrated GPU.

## Grounded findings (why this design)

1. **The box:** one OpenCL platform (ROCm 3581.0), three GPU devices: 2×
   `AMD Radeon AI PRO R9700` (gfx1201, 32 CU, 32 GB, host-unified = 0) and
   the gfx1036 iGPU (1 CU, host-unified = 1). `CL_DEVICE_HOST_UNIFIED_MEMORY`
   is a perfect discrete/integrated discriminator — no name matching.
2. **This was the first Linux build ever** (all bin/ artifacts were PE32+;
   the campaigns to date ran on the Windows/4070 Ti Super box). Ported:
   `-D_DEFAULT_SOURCE` (glibc hides popen/fseeko/usleep under strict
   `-std=c11`), an `_fseeki64 -> fseeko` shim in cce_gguf.c, and WALL-clock
   timing in gpu_equiv (`clock()` is wall on Windows but process-CPU-time on
   Linux — with N driver threads spinning it overcounts N-fold).
   NOTE: `acquire` and `base` suites fail two certify steps on Linux
   PRE-CHANGE (verified against the untouched tree) — a pre-existing
   Windows/Linux port issue (suspect libc numerics, e.g. RAND_MAX 32767 vs
   2^31-1), tracked separately; `flagship` (72 checks) and `mutate` pass.
3. **The head was paid 2-3x:** `cce_gguf_qwen2_forward` computed the LM head
   ([1024 x 262144] ≈ 1 GB, the dominant GEMM) for EVERY sequence row, then
   copied out only the last. Restricting to the last row is bit-identical
   per element and also fixed a latent fallback bug (manual head wrote row 0
   while the copy-out read row n-1). CPU forward: 4.7 → 7.1 fwd/s.
4. **Intra-GEMM split across 2 GPUs is a WASH here** (measured, wall-clock):
   the forward is 29 skinny (T ≤ 3) GEMM calls, each a blocking queue
   round-trip; per-call latency, not bandwidth, dominates. Column-splitting
   the big matrices halves their read time but adds a second round-trip:
   single R9700 53.0 fwd/s vs split-across-2 46.5 fwd/s. The split stays in
   (correct, bit-identical, and the right shape for future fatter models)
   but it is NOT where the second GPU earns its keep.
5. **Mining is embarrassingly parallel:** a unit is |V| INDEPENDENT oracle
   calls (each a fresh context, KV reset). That is where 2 GPUs ≈ 2x.

## Decision

Two cooperating mechanisms, both defaulting to "all discrete GPUs, never
the iGPU":

**A. Multi-device `cce_clgemm` (public API unchanged).** open() enumerates
every platform's GPU devices, keeps the DISCRETE ones of the first platform
that has any (host-unified property, not names; an iGPU-only box keeps its
single device). Weights ≥ threshold are column-split across devices (packed
slices, per-device queues, non-blocking enqueues, finish-all, scatter);
smaller weights go whole to one device round-robin. Every output element
keeps the same k-ascending accumulation → split results are BIT-IDENTICAL
to single-device. Env: `CNET_GPU_DEVICES=i,j`, `CNET_GPU_COUNT=n`,
`CNET_GPU_SPLIT_MB=m` (default 32). New: `cce_clgemm_open_device(i)` opens
the i-th device of that selected set alone; `cce_clgemm_device_count()`.

**B. Oracle POOL for campaigns (the real speedup).** flagship_run with
CNET_GPU=1 and >1 discrete GPU builds one model instance per GPU (lane 0
reuses the loaded model), each with its OWN clgemm handle — via the new
per-instance `cce_gguf_qwen2_set_clgemm(m, h)` (the process-global setter
stays as the one-model default; a shared handle across threads would race
on queues). The oracle fn dispatches lanes by OMP thread id; the maker
reports `FlagshipOracle.width = nlanes`; flagship passes it to the new
`acquire_oracle_set_parallel()`; `mine_from_oracle` then mines enumeration
points into PER-INDEX SLOTS `width`-wide and compacts SERIALLY in index
order — exemplar tables, counters, and digests are identical to serial by
construction. Nested OMP: `lanes` outer threads × `inner = ncores/lanes`
CPU-tile threads per forward. `CNET_ORACLE_LANES=1` restores the single
path exactly.

Gates, extended not weakened: the startup equivalence sweep runs PER LANE
against a CPU reference computed before any handle is attached; the
determinism spot check runs per lane AND cross-lane (lane i's answers must
equal lane 0's — mined knowledge must never depend on scheduling).

**C. Thermal governor sees AMD now.** `fs_gpu_temp()` reads amdgpu sysfs
first — max `temp1_input` across cards with ≥ 4 GiB `mem_info_vram_total`
(the iGPU carve-out stays out of the governor) — then falls back to
nvidia-smi (now max across GPUs, stderr silenced). Campaigns on this box
were previously UNGOVERNED (temp always -1); now: `max GPU temp 61 C`.

## Measured (4-unit TOPK smoke, V=256, duty 1.0, this box)

| config                              | wall  | s/unit | digests |
|-------------------------------------|-------|--------|---------|
| 1× R9700 (CNET_GPU_COUNT=1)         | 26.0s | 6.5    | ref     |
| 2× R9700 column-split, 1 lane       | 34.0s | 8.5    | 4/4 IDENTICAL |
| 2× R9700 oracle pool (default)      | 10.0s | 2.5    | 4/4 IDENTICAL |

Pool = **2.6x** over one GPU end-to-end (cnb_audit behavior-digest fidelity
4/4 IDENTICAL across all three bases). gpu_equiv: max|Δlogit| = 0.0,
argmax + top-3 64/64 on both 1- and 2-device configs.

## Non-goals

- No GPU for BTN training (unchanged from 2026-07-03).
- No heterogeneous load balancing (equal split; `CNET_GPU_DEVICES` is the
  escape hatch). No P2P — split slices are disjoint by construction.
- No parallel PILOT phase (sampled mode only, ≤ 64 calls) and no parallel
  conformal probe — serial, exactly as before.
- The iGPU is never selected unless explicitly forced via
  `CNET_GPU_DEVICES`.

## Adversarial review (42-agent, 4 lenses + refuters) — fixes folded in

- `cce_clgemm_open_device(i)` now indexes the devices that actually OPEN
  (the `cce_clgemm_device_count` index space), so a lane never pins a device
  the probe already proved dead.
- An explicit `CNET_GPU_DEVICES` spec that matches nothing REFUSES (NULL)
  instead of silently expanding to all discrete devices.
- The forest scratch archive is unique per model instance
  (`gguf_qwen2_forest.<seq>.cce`, removed at free) — pool lanes no longer
  remove()/rewrite each other's live backing file.
- gpu_equiv wall timer: `clock_gettime(CLOCK_MONOTONIC)` on POSIX, `clock()`
  kept on Windows (timespec_get is absent from MSVCRT MinGW).
- Known + accepted: CNET_ORACLE_INT8=1 oracles skip the GPU seam entirely
  (int8 blocks fail the plain-float gate — pre-existing, applies to the
  real-12B oracle); governor iGPU exclusion is the 4 GiB VRAM-carve-out
  heuristic, correct on this box.

## Risks, stated

- Two model instances double host RAM for the oracle (~2 GB fragment: fine;
  a future 12B float oracle would need the int8-on-load path, which skips
  the GPU seam anyway).
- Lane divergence on non-identical GPUs would be caught by the per-lane
  sweep + cross-lane spot check (decision level — the oracle emits one-hot
  argmax decisions, so decision-identity is exactly the property digests
  need).
- OMP oversubscription: outer×inner is sized to the machine
  (`omp_set_max_active_levels(2)`, inner = ncores/lanes), not stacked.
