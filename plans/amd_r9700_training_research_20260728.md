# Dual-R9700 training-rate research and decision

**Date:** 2026-07-28  
**Host:** Ryzen 9 9950X, two discrete Radeon AI PRO R9700 (`gfx1201`, 32 GiB each), one integrated `gfx1036` device that must be excluded  
**Purpose:** accelerate the bounded Alive Valley/Tamagotchi specialist and CNET acquisition without weakening determinism, certification, or service safety.

## Executive decision

Use the two R9700s, but divide work by model scale:

1. **Authoritative simulation and the tiny action policy stay CPU-first.** Alive Valley runs at 0.0098 ms/tick, and CNET's native learner measured about 953k steps/s. A per-decision GPU launch would be slower than the arithmetic unless examples are batched into large matrices.
2. **Use both GPUs as independent workers by default.** Run one candidate/seed/data shard per R9700. This avoids synchronization, gives nearly 2x experiment throughput, and isolates failures.
3. **Use the existing CNET dual-GPU oracle pool for teacher/example generation.** `make unified_gpu` already measured 1.984x over one serial R9700 lane with exact CPU parity and iGPU exclusion. The missing piece is safe campaign integration.
4. **Use a hermetic PyTorch ROCm lane for the larger mood/dialogue/response adapter.** Do not install into `~/ai-env`; do not replace the serving ROCm stack in place.
5. **Treat DDP/RCCL as an opt-in benchmark, not the default.** The cards have peer access but are connected by two-hop PCIe without xGMI, and an open upstream report documents RCCL deadlock on dual R9700 with ROCm 7.2. DDP is retained only if a timeout-bounded all-reduce and training benchmark is stable and materially faster.

This is not a proposal to let a model mutate game state. Unity/Alive Valley remains authoritative. A learned specialist may only emit bounded IDs from a versioned input contract; Unity validates and commits the action.

## Live machine audit

### Hardware and topology

- Two discrete `AMD Radeon AI PRO R9700`, target `gfx1201`, approximately 32 GiB VRAM each.
- PyTorch reports peer access in both directions.
- `rocm-smi --showtopo` reports a two-hop PCIe path between the cards, not xGMI/Infinity Fabric.
- Each card is on a PCIe 5.0 x8 link, currently negotiated at PCIe 5.0 x8.
- ROCm reports non-coherent host access for the discrete devices.
- PyTorch also enumerates the integrated `gfx1036` GPU as device 2; robust launchers must positively allow-list the two discrete `gfx1201` devices rather than relying on ordinals or a deny-list.
- Both R9700s currently have sleeping `llama-server` processes. Low utilization is not permission to evict them; real training requires an explicit maintenance window.

### Installed stack

- System ROCm: 7.2.3.
- `~/ai-env`: custom PyTorch 2.7.1 + ROCm 7.2; BF16 is exposed on both R9700s.
- Present Python packages include `transformers 5.0.0rc2`; the current environment does not provide a complete, pinned PEFT/TRL/bitsandbytes training stack.
- `librccl.so.1` is installed, but `rccl-tests`/`all_reduce_perf` are not installed.
- Profiling tools available: `rocprofv3` and `rocprof-compute`.
- Host kernel is 6.17.0-20. AMD's ROCm 7.14 Radeon matrix lists Ubuntu 24.04.4 with GA kernel 6.8; this host is therefore outside that exact validated kernel row.

### Existing CNET performance

- `make cce_train_bench`: about 0.001049 ms/step, approximately 953k native learner steps/s.
- `make unified_gpu`: existing two-physical-R9700 async oracle substrate; repository evidence records 1.984x versus the serial lane fixture, exact CPU output parity, and iGPU exclusion.
- `CNET_TRAIN_FAST=1`: existing byte-identical CPU fast path, measured 1.61x serial and 4.3x at four OpenMP threads for the deployed H=512 shape.
- Campaign v2-fast: approximately 4-6 hours for one worker and approximately 2 hours for two independent workers over the current 255-unit allowlist. The campaign runner currently forces CPU because the GPUs were reserved.
- Existing OpenCL/hipBLAS GPU inference work should be reused, not reimplemented: raw large GEMM gains from one/two R9700s and a size floor that rejects PCIe-bound micro-GEMMs are already measured.

### Alive Valley state

- The current game contains deterministic utility selection, not a learned policy.
- Simulation is fixed-point, allocation-free after warm-up, and authoritative.
- There is not yet a versioned `CreatureState x RecentEvents x Personality -> ActionID + MoodID + ResponseID` training artifact. Dataset generation and a replayable baseline must precede honest GPU speed claims.

## Primary-source findings

### Current official support

AMD's ROCm 7.14 compatibility matrix explicitly lists:

- Radeon AI PRO R9700 / RDNA 4 / `gfx1201`;
- RCCL 2.30.4;
- PyTorch 2.12.0 and 2.11.0;
- hipBLASLt 1.4.1, rocBLAS 5.5.0, rocWMMA 2.2.1;
- ROCprofiler-SDK 1.3.2.

Source: <https://rocm.docs.amd.com/en/latest/compatibility/compatibility-matrix.html?fam=radeon&w=compute&gpu=ai-r9700&gfx=gfx1201&os=ubuntu>

The ROCm 7.14 release adds PyTorch profiler integration through ROCprofiler-SDK and SDMA activity telemetry useful for identifying multi-GPU data-movement stalls.

Source: <https://github.com/ROCm/ROCm/releases/tag/rocm-7.14.0>

### RCCL and PCIe

AMD's RCCL usage notes say PCIe-connected peer-to-peer access may require `HSA_FORCE_FINE_GRAIN_PCIE=1`. This variable must be tested, not blindly exported: discrete `gfx1201` reports non-coherent host access, and upstream reports include GPU page faults involving host memory and a dual-R9700 RCCL deadlock.

Sources:

- <https://github.com/ROCm/rccl/blob/develop/docs/how-to/rccl-usage-tips.rst>
- <https://github.com/ROCm/rccl/blob/develop/docs/how-to/troubleshooting-rccl.rst>
- <https://github.com/ROCm/rocm-systems/issues/5480>

For DDP, PyTorch overlaps gradient all-reduce through buckets. `bucket_cap_mb`, `gradient_as_bucket_view`, `static_graph`, and compile order can affect performance. For a LoRA where almost all base weights are frozen, synchronized gradient volume is small, but each replica still holds a complete model and each step must rendezvous.

Source: <https://docs.pytorch.org/docs/2.13/notes/ddp.html>

FSDP/ZeRO is appropriate when model/optimizer state does not fit one card. It is not the default for an 8B LoRA that fits comfortably on one R9700 because sharding adds collectives to a problem that independent workers solve without communication.

Source: <https://docs.pytorch.org/docs/2.13/fsdp.html>

### `gfx1201` kernel maturity

The current ecosystem is useful but uneven:

- bitsandbytes has a fused ROCm 4-bit inference GEMM validated on `gfx1201`, but its release notes say ROCm 8-bit optimizer parity is still incomplete. Treat it as a quantized-weight loading option, not a guaranteed optimizer speedup.
- Unsloth now advertises AMD/RDNA4 support and up to 2x training speed, but an open Qwen3.5 SFT report on `gfx1201` fails at step 0 under `fullgraph=True`; partial compile is the reported workaround.
- AITER still has an open issue for general `gfx1201` support.
- upstream FlashAttention and PyTorch CK/AOTriton reports document `gfx1201` compile/dispatch failures. Stable SDPA/manual attention is the baseline; fused attention must pass its own probe.
- field reports on ROCm 7.2 show BF16 QLoRA can work on R9700, while hipBLASLt, Triton stage count, expandable segments, and host-memory staging can each trigger hangs, page faults, or extreme load time depending on model architecture.

Sources:

- <https://github.com/bitsandbytes-foundation/bitsandbytes/releases/latest>
- <https://github.com/unslothai/unsloth/releases/latest>
- <https://github.com/unslothai/unsloth/issues/6825>
- <https://github.com/ROCm/aiter/issues/3294>
- <https://github.com/Dao-AILab/flash-attention/issues/2588>
- <https://github.com/pytorch/pytorch/issues/188113>
- <https://github.com/ulises-c/csen-346/issues/109>
- <https://github.com/ulises-c/csen-346/issues/120>

## Training architecture

```text
Alive Valley deterministic simulator (CPU, parallel seeds)
        |
        +--> versioned transition/event dataset + replay hashes
                      |
                      +--> tiny bounded action head
                      |      CPU default; batched native CNET GPU candidate only above measured size floor
                      |
                      +--> response/dialogue LoRA candidates
                             GPU0: candidate/seed A
                             GPU1: candidate/seed B
                             no collective communication

CNET teacher/oracle campaign
        |
        +--> existing async lane pool
                GPU0 oracle lane + GPU1 oracle lane
                independent jobs; exact parity and provenance retained

Optional final-fit experiment
        |
        +--> two-rank DDP/RCCL only after all-reduce soak passes
```

## Optimization priority

1. **Measure phase breakdown:** simulation, serialization, tokenization, H2D, forward, backward, optimizer, checkpoint, evaluation, certification.
2. **Feed both GPUs independently:** sharded candidates and oracle calls give the most robust near-2x throughput opportunity.
3. **Batch before offload:** batch trajectory records and native CNET LoRA matrix operations; retain CPU path below an empirically chosen floor.
4. **BF16 first:** compare BF16 AdamW and the proven Adafactor recipe. Do not assume historic FP16 NaN behavior applies to BF16.
5. **Stable attention first:** PyTorch SDPA/manual baseline; test fused paths behind feature flags and retain only measured winners.
6. **Remove input stalls:** pretokenize/cache immutable examples, pinned-memory DataLoader only if it measures faster on this non-coherent discrete topology, persistent workers, bounded prefetch.
7. **Checkpoint sanely:** atomic checkpoints at useful intervals; do not erase speed gains with per-step disk I/O.
8. **Profile one layer at a time:** `torch.profiler`/ROCprofiler traces plus step-time and utilization metrics. No claims from GPU utilization alone.
9. **Evaluate newer ROCm hermetically:** benchmark the official ROCm 7.14/PyTorch 2.12 combination in an isolated environment or container; never mutate `~/ai-env` or serving services in place.
10. **Keep DDP only if earned:** timeout-bounded RCCL test, then short training soak, then same-quality throughput comparison.

## Required benchmark matrix

Every report must record exact versions, git SHA, dataset digest, model digest, device mask, seed, batch/accumulation, precision, attention backend, optimizer, samples/s or tokens/s, step p50/p95, max VRAM, final loss/quality, and exit status.

Minimum comparisons:

1. CPU action-head baseline.
2. Single R9700, BF16, stable SDPA.
3. Two independent R9700 jobs, same candidate count as serial.
4. Native CNET batched CPU versus batched GPU at several shapes to determine the offload floor.
5. Optional DDP two-R9700 run after RCCL probe.
6. Local ROCm 7.2 stack versus hermetic ROCm 7.14/PyTorch 2.12 stack.

## Keep/revert gates

- No silent iGPU selection.
- No change to authoritative simulation/replay hashes.
- No NaN/Inf in loss, gradients, or weights.
- No GPU reset, page fault, hang, or dirty KFD continuation.
- No degradation beyond preregistered quality tolerance.
- Independent two-GPU throughput target: at least 1.70x over serial one-GPU candidate throughput on the same candidate set.
- DDP target: at least 1.25x samples/s over one GPU at equal effective batch and matching quality; otherwise retain independent workers only.
- Native GPU path must beat the optimized CPU path at the measured shape; otherwise the size floor must select CPU.
- All experiments have a timeout, atomic logs, checkpoint/resume where appropriate, and post-run process/VRAM cleanup.

## Expected outcome

The tiny action specialist itself is unlikely to need both GPUs. The practical gain comes from generating/labeling examples through two GPU oracle lanes and training two dialogue/response candidates concurrently. Based on existing CNET dual-lane evidence, approximately 2x experiment throughput is realistic without RCCL. Larger gains are hypotheses until the new benchmark harness records them.
