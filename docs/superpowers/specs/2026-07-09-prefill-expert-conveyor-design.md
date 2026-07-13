# MoE Prefill Expert Conveyor: Dual-R9700 Streaming — Design & Findings

**Date:** 2026-07-09
**Status:** Hardware measured, pattern spiked, engine patches landed & compiling
(user directive: "lets solve prefill … the villain isn't the GPU, it's the bus.
it's PCIe. in our case we have 2 GPUs. Can you solve it").
**Host engine:** DwarfStar (`~/AI/ds4`, ROCm backend) — the MoE teacher engine
this box runs; CNET's own oracle forward is dense batch-1 and has no prefill
wall. End-to-end model runs pend the 81 GB q2 GGUF download (in progress).

## The problem, stated precisely

MoE offload prefill: attention + router run on the GPU, then the selected
experts stream RAM→VRAM over PCIe while the GPU idles, then the experts
compute. Per layer: `t = t_attn+router + t_copy + t_ffn`, serialized. Profiled
reality: the GPU spends more wall time receiving weights than computing them.

## Grounded findings (this box, measured 2026-07-09)

1. **Links:** 2× R9700 (gfx1201, 32 GB) on separate CPU root ports, both
   electrically PCIe 5.0 **x8** (sysfs says x16; measurement says otherwise):
   **28.5 GB/s H2D solo each**. Simultaneously: **46 GB/s aggregate** (1.6×,
   not 2× — an upstream fabric/DRAM limit). P2P GPU0↔GPU1 works, 28 GB/s.
2. **Copy/compute independence:** H2D DMA sustains 100 % of link rate while
   FMA kernels run at 100 % throughput on the same GPU (SDMA engines are real).
   Overlap is free; serialization is purely a software artifact.
3. **Pinning:** `hipHostRegister` of a whole-file 7.4 GB `MAP_SHARED` mmap
   succeeds in 0.15 s on ROCm 7.2.3 and DMAs at full rate (28.6 GB/s).
   Pageable H2D also reaches ~28.4 GB/s on this stack (probe
   `scratchpad/pcie_probe.cpp`, `overlap_probe.cpp`).
4. **Conveyor spike** (`spike/expert_conveyor_spike.cpp`, real GGUF bytes,
   439×16 MB "experts", slot ring + per-expert events, compute chasing):
   serialized 1 GPU 25.7 GB/s → conveyor 2 GPUs **41.8 GB/s** effective
   (**1.63×**); whole-file registration at production scale confirmed.
5. **Model math (DeepSeek V4 Flash q2):** 43 layers × 256 experts × top-6;
   ~6.7 MiB/expert (IQ2_XXS gate/up 2.06 + Q2_K down 2.62) → **~1.72 GB per
   layer**, ~74 GB routed experts total. At a 2048-token chunk every expert of
   every layer is hit with p ≈ 1−e⁻⁴⁸ ≈ 1 → **router-blind streaming is
   mathematically correct for prefill**; the router readback exists only to
   pick which experts to copy, never to gate compute inputs.

## Diagnosis: where ds4 actually serializes (verified in source)

CUDA backend (`ds4_cuda.cu`) is strictly copy-all-then-compute: per layer
`cudaDeviceSynchronize` → blocking D2H readback of selected ids
(`ds4.c:14369-14394`) → per-weight `cudaStreamSynchronize` uploads
(`ds4_cuda.cu:2237`) → FFN. Zero `cudaStreamWaitEvent` in the file. Single
device hardcoded.

ROCm backend (this box) is far better — router-blind **full-layer prefetch
already exists** (`ds4.c:11681-11916`: background pthread loads all 256
experts of layer il+1 into a double-buffered VRAM arena while il computes,
for chunks ≥1024 tokens on pure-q2) — but four leaks keep the GPU idle:

- **L1 — bus deficit:** one link delivers 1.72 GB in ~60 ms; a layer computes
  in ~25-35 ms. The `ready(il)` join (`ds4.c:20565`) therefore blocks every
  layer. No software fix on one GPU can close this; it is bus arithmetic.
- **L2 — SSD relapse:** every upload drops its source page cache
  (`posix_fadvise(DONTNEED)`, `rocm/ds4_rocm_runtime.cuh` read-worker) — right
  for RAM-tight unified-memory boxes, backwards here (91 GB RAM vs 81 GB
  model): every chunk re-reads experts from NVMe, inflating 60 ms loads to
  150-300 ms. Dominant villain for long prompts (>1 chunk).
- **L3 — per-layer device drain:** `ds4_gpu_end_commands` =
  `hipDeviceSynchronize` (`runtime.cuh:4599`), a Metal commit-and-wait port,
  runs at every layer end — it also currently guarantees arena-reuse safety.
- **L4 — selected path (<1024 tokens):** missing-expert kernel launch gated by
  a `pthread_cond_wait` CPU join (`moe_launch.cuh:588-594`), not an event.

## Decision: the conveyor stack

Throughput model per chunk ≈ Σ_layers max(bytes_missing/B_link, t_compute),
pipelined across GPUs. Levers, multiplicative:

| lever | mechanism | expected effect (this box) |
|---|---|---|
| **P0 built** | gfx12 WMMA port (RDNA4 renamed the builtins; kernel used raw gfx11 forms) | ROCm build exists at all on R9700; verified vs scalar ref, 0/18432 bad |
| **P1 built** | RAM-aware page-cache policy: keep pages on discrete-GPU boxes when file ≤85 % MemAvailable; env `DS4_ROCM_STREAM_DROP_FILE_PAGES=0/1` overrides; unified-memory APUs keep legacy drop | steady-state chunks go SSD-bound → PCIe-bound (~350 → ~790 t/s ceiling, 1 GPU) |
| **P3 built** | layer-ahead depth ring: `DS4_ROCM_STREAM_PREFILL_LAYER_AHEAD` (default 1 = historical; max 3), arenas grown to depth+1 (`ds4_gpu_stream_layer_cache_slots_set`, clamp [2,4]) | absorbs jitter; required to hold both links busy at the dual-GPU balance point |
| **P4 ready** | dual-GPU layer pipeline: existing `--role coordinator/worker --layers A:B` over loopback, one process per card via `HIP_VISIBLE_DEVICES` (`run_dual_r9700.sh`); each link carries only its layers' bytes | ~2× chunk throughput: 43×60 ms → ~21×60 ms per chunk per GPU, both links live (46 GB/s aggregate) |
| **P5 next** | resident-aware full-layer loads: arena → per-slot pointer tables so resident LRU experts alias in and only misses stream; VRAM then absorbs ~10-12 layers per GPU permanently | up to further ~1.8× (skip resident bytes); turns prefill compute-bound |
| **P2 next** | event-chased uploads: `hipEventRecord` per expert-group on worker streams + `hipStreamWaitEvent` on stream 0 before the missing/next-layer kernels (pattern already in-tree at `shared_expert.cuh:205-214`); then relax L3's per-layer drain inside the prefill loop only | removes the last few ms/layer of join+drain latency once P4/P5 make copy ≈ compute |

Predicted ceilings (4096-token chunks, q2 Flash): today single-GPU SSD-bound
~350 t/s → P1 ~790 → P1+P4 ~1400-1600 → +P5 ~2500+, at which point the GPUs,
not the bus, are the limit — on a box whose two links were the villain.

## What landed today (all compiling, `make strix-halo ROCM_ARCH=gfx1201`)

ds4 working tree (base 80ebbc3, +214/−40 across 4 files):
- `rocm/ds4_rocm_q8.cuh` — gfx12 WMMA port (`DS4_Q8_WMMA`, `DS4_Q8_WMMA_ROW`:
  RDNA4 8-half K-split operands + packed accumulator rows) behind
  `__gfx1200__/__gfx1201__`; gfx11 path byte-identical.
- `rocm/ds4_rocm_runtime.cuh` — `cuda_stream_drop_file_pages()` policy wrapper
  at both streaming call sites, decided once and logged; layer-arena slots
  2→configurable ≤4.
- `ds4.c` — full-layer prefetch ring (`..._load_ring`), depth env, initial
  kick starts `depth` layers, per-layer `ready(il)`/`start(il+depth)`,
  join-all on error paths. Depth 1 = historical behavior exactly.
- `ds4_gpu.h` — `ds4_gpu_stream_layer_cache_slots_set` prototype.
- `run_dual_r9700.sh` — loopback pipeline launcher (GPU pinning, layer split,
  iGPU excluded by construction).

CNET side: `spike/expert_conveyor_spike.cpp` (the pattern, measured), probes
in scratchpad, this doc.

## Measured (ladder, same day, real V4 Flash q2 86.7 GB, 9,474-token prompt)

| rung | config | prefill | identity vs L0 |
|---|---|---|---|
| L0 | legacy: drop-pages, ahead=1, 1 GPU | 210.6-213.1 t/s | (baseline) |
| L1 | keep page cache | 195.3-200.0 t/s | IDENTICAL |
| L2 | + layer-ahead 2 | 208.3 t/s | IDENTICAL |
| L3 | dual-GPU loopback pipeline | — | blocked (below) |

Findings, honestly:
- The conveyor works at scale: 129 full-layer loads (3 chunks x 43 layers),
  217.69 GiB streamed per pass, GPU execute 16-28 ms/layer while each
  1.7 GB layer arrives — the bus deficit measured, exactly as modeled.
- Single-GPU rungs FLATLINE (~210 t/s): one saturated 28.5 GB/s link is
  the wall; prefetch depth cannot create bandwidth. The levers that move
  this box are the second link and residency (P5).
- P1 (keep pages) is NEGATIVE here: the file (86.7 GB) exceeds what 91 GB
  RAM can hold, so forced retention just adds reclaim pressure. The auto
  policy correctly chooses dropping on this box (its log line confirms);
  P1 pays only with real RAM headroom — as designed, now measured.
- P3 required a REWRITE mid-ladder: the streaming read pool is
  single-tenant, so depth>=2's concurrent whole-layer loads refused
  ("read pool already has active work"). Replaced one-thread-per-layer
  with a single persistent prefetch WORKER draining a monotonic layer
  queue (ensure/want API) — serializes pool access, runs ahead of
  compute, and removes the per-layer thread-relaunch gap. Identity-clean.
- **L3 (dual GPU) WORKS end-to-end** after SIX seams between the
  streaming and distributed subsystems — which had never run together —
  were fixed in sequence (each one unreachable until the previous fell):
  (1) the single-instance lock refuses loopback dual-process runs →
  per-role DS4_LOCK_FILE; (2) the distributed layer-slice evaluator is a
  THIRD prefill driver with no conveyor — encode_layer_batch skips its
  selected load when full-layer mode is on and trusts an arena nobody
  loaded ("full expert table is not mapped") → prefetch worker +
  decode-map wired into ds4_session_eval_layer_slice; (3) 60 s default
  send timeout vs cold arena loads → DS4_DIST_SOCKET_TIMEOUT_SEC=600;
  (4) prompt-scoped arenas must be released at the PREFILL→DECODE
  transition, not per chunk (per-chunk frees get stolen by resident
  seeding; keeping them through decode starves its selected cache) →
  release at the n==1 slice-decode entry; (5) the ROCm resident-cache
  VRAM reserve was HARDCODED 16 GiB (a unified-memory default — half a
  discrete card) → honors DS4_CUDA_STREAMING_EXPERT_CACHE_RESERVE_GB
  now, 4 on this box; (6) operational: GPU1 must actually be free (a
  resident llama-server cost several iterations). An earlier "keepalive
  starvation" hypothesis was WRONG — the route loss was always
  error-triggered; error visibility was the real gap, and worker
  work-errors + recovery causes are now logged.
  Result: pipelined dual-GPU prefill 9,474 tok in 39.9 s = 237.4 t/s
  (send 0.66 s @ 900 MiB/s), decode 5.2 t/s, coherent 32-token output.
  vs single-GPU ~210 t/s: +13 % only, because BOTH processes re-read
  their layer halves from one shared SSD per chunk (auto page-drop; the
  file exceeds RAM) and the flow window limits chunk overlap — the 2×
  bus math needs residency (P5) or RAM headroom to express. Decode-chain
  identity vs the single-process driver diverges at token 2 (different
  decode implementations — same class as the llama.cpp GPU/CPU
  divergences); prefill logits agree (token 1 identical). Also: ds4's
  one-shot CLI exits 1 on long prompts after a complete run (ladder
  tolerates with stats-evidence checks).

## Verification plan (gates before any campaign use)

Blocked on the q2 GGUF download (~81 GB, `ds4/gguf/*.part`, resumable via
`./download_model.sh q2-imatrix`). Then, in order:

1. **Numerics unchanged:** same prompt, `--ssd-streaming`, argmax/top-k
   token-identical across: unpatched vs patched single-GPU, ahead=1 vs 2,
   pages-drop vs keep, single vs dual-GPU pipeline. (Weights are immutable
   bytes; every patch moves copies, not math — the gate should be exact
   token identity.)
2. **Baseline profile:** `DS4_METAL_GRAPH_PREFILL_SPLIT_PROFILE=1` +
   `DS4_ROCM_STREAM_CACHE_STATS=1` on a ≥8k-token prompt: per-layer
   encode/execute vs load, layer_load_bytes; confirm L1/L2 numbers.
3. **A/B ladder,** one lever at a time (their culture: never conflate):
   drop-pages 1→0; ahead 1→2; single→dual. Record t/s prefill + GPU busy %
   (rocm-smi) + link MB/s.
4. **Decode regression:** tokens/s and expert-cache hit rate unchanged
   (patches touch prefill paths and page-cache policy only; P1 also helps
   decode misses by serving them from page cache).

## Non-goals / risks

- No in-process multi-device backend yet (the runtime is a device-0 singleton
  end to end); the process-per-GPU pipeline sidesteps it. De-globalizing is
  the eventual in-process path (inventory of globals mapped, ~40 symbols).
- P1 auto-policy is decided once per process; a box whose MemAvailable
  shrinks drastically mid-run keeps its decision (env override exists).
- Depth >1 raises VRAM by 1.72 GB/step; the 16 GB default cache reserve plus
  Q8→F16 attention residency must still fit — watch the arena-alloc failure
  log line on 32 GB cards, it falls back by refusing (loudly), not corrupting.
- The dual-GPU launcher is unvalidated until the model lands (distributed
  SSD-streaming slices were fixed upstream in f2d701a; loopback is its
  easiest case).
