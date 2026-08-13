# Opus 5 partition — gfx1201 kernel repair

**Date:** 2026-07-28  
**Agent:** Claude Opus 5, xHigh effort, through Herdr pane `w3:p1`  
**Execution branch/worktree:** `feature/amd-r9700-training-rate` at `/home/marble/AI/CNET-worktrees/amd-r9700-training`  
**Insertion point:** finish existing Task 2, then execute this repair before the original PyTorch LoRA Task 3. Continue original Tasks 3–5 afterward.

## Goal

Turn the five known `gfx1201` failure families into reproducible RED cases and working GREEN paths for the exact Qwen3.5/LoRA shapes CNET needs. A local compatibility patch or correct accelerated fallback is acceptable; exception suppression, an untested architecture allow-list, and claims of general RDNA4 support are not.

The five tracks are:

1. AITER architecture/kernel support;
2. FlashAttention CK compilation;
3. PyTorch CK/AOTriton SDPA dispatch;
4. Unsloth Qwen3.5 graph compilation;
5. KFD host-memory/page-fault and RCCL dual-R9700 transport.

bitsandbytes is a verification/integration gate, not a sixth repair: release 0.50.0 already validates its fused ROCm 4-bit GEMM on `gfx1201`, while explicitly withholding 8-bit optimizer parity.

## Stop-the-line preflight — remove EOL contamination without losing Task 2

The newly-created CNET worktree currently shows about 94 tracked files and roughly 31,693 insertions/deletions caused only by CRLF→LF normalization. Legitimate Task 2 semantic work also exists in `src/cce/cce_lora.c` plus new batch files.

Before resuming code:

1. Record `git status --short`, `git diff --stat`, and `git diff --ignore-space-at-eol --stat`.
2. Preserve every file whose EOL-insensitive diff is non-empty and every untracked Task 2/plan file.
3. For each tracked file whose EOL-insensitive diff is empty, restore the exact `HEAD:<path>` bytes with a binary-safe script. Do not use a blanket restore.
4. Re-run the three commands. The 94-file/31k-line noise must disappear; intended Task 2 files must remain.
5. Add a local worktree note or setup script so subsequent tools do not renormalize untouched legacy CRLF files.
6. Do not commit line-ending-only changes.

Expected preflight marker: `GFX1201_EOL_PREFLIGHT_PASS` with semantic tracked files and untracked files listed.

## Global constraints

- Strict RED→GREEN TDD: every repaired behavior first needs a minimal reproducer that fails for the expected reason.
- Use only isolated environments/caches. Never install into `/home/marble/ai-env`.
- Pin exact upstream repository URL, commit SHA, ROCm/PyTorch version, compiler flags, and applied patch digest in `training/rocm/upstream.lock.json`.
- Keep upstream clones/build trees outside the repo, under a bounded cache such as `/home/marble/.cache/cnet-gfx1201-repair/`.
- Store only minimal patch files, probes, locks, and reports in CNET. Do not vendor full third-party repositories.
- AMD/ROCm only. No CUDA wheels, headers, libraries, paths, or assumptions.
- Positively allow-list exactly two discrete `gfx1201` R9700s; reject the integrated `gfx1036` and do not trust ordinals.
- Never stop/restart/evict serving processes. Real GPU execution remains blocked until an explicit maintenance-window authorization is present.
- Every compile, probe, or distributed operation has a timeout, a child-process sweep, and a VRAM/KFD cleanup check.
- Do not set `HSA_FORCE_FINE_GRAIN_PCIE=1`, pinned memory, nonblocking host transfers, expandable segments, fullgraph compile, fused attention, or hipBLASLt globally. Each is an isolated experiment.
- Fix only the actual model shapes used by the selected Qwen3.5/LoRA lane plus a small boundary matrix. Broader claims remain withheld.
- No external push or upstream PR. Produce locally reviewable patches with source provenance.

## File scope

New files should remain under:

- `training/rocm/upstream.lock.json`
- `training/rocm/compat/`
- `training/rocm/patches/aiter/`
- `training/rocm/patches/flash-attention/`
- `training/rocm/patches/pytorch-sdpa/`
- `training/rocm/patches/unsloth/`
- `training/rocm/probes/`
- `training/rocm/reports/` for small machine-readable summaries only
- `scripts/run_gfx1201_kernel_gate.sh`
- `tests/test_gfx1201_kernel_gate.sh`
- this plan's results appendix

The existing original Task 3 files may consume the compatibility router after this gate. Do not widen Task 6 into unrelated CNET C core or Unity files.

## Track K1 — AITER `gfx1201`

**Upstream RED authority:** `ROCm/aiter#3294`, “lacking general support for gfx1201.”

1. Pin current AITER upstream.
2. Discover the exact AITER operators reachable from the chosen Qwen3.5 LoRA trainer; do not port unused operators.
3. Write a minimal import/build/forward/backward probe for each required operator and capture the unpatched failure as `RED_AITER_GFX1201`.
4. Patch architecture detection, target flags, kernel registry, and only the required operator implementation/configuration.
5. Compare outputs and gradients with a stable PyTorch BF16/FP32 reference.
6. Run repeated bounded launches to catch lazy compilation and cache failures.

GREEN requires all required operators to compile and execute on real `gfx1201`, finite output/gradients, FP32 `rtol/atol <= 1e-4` where applicable and BF16 `rtol/atol <= 2e-2`, plus `GREEN_AITER_GFX1201`. An allow-list edit without execution is not GREEN.

If an operator cannot be correctly ported without unsupported ISA/compiler work, hard-disable that operator on `gfx1201`, route to the measured stable backend, and report `AITER_GFX1201_PARTIAL` with the exact unsupported operator. Do not claim general support.

## Track K2 — FlashAttention CK compile failure

**Upstream RED authority:** `Dao-AILab/flash-attention#2588`, `v_cmp_u_f32 invalid` on `gfx1201`.

1. Pin current ROCm FlashAttention/CK source and compiler version.
2. Reproduce the failing instruction on the smallest causal BF16 forward/backward shape, then on representative Qwen3.5 head dimensions and sequence lengths derived from the actual model config. Emit `RED_FLASH_ATTN_GFX1201`.
3. Localize whether the defect is architecture macro selection, compare intrinsic lowering, wave size, CK tile choice, or generated assembly.
4. Produce the smallest source patch that compiles valid `gfx1201` code. Do not edit generated output when the generator is the root cause.
5. Verify causal masking, non-multiple sequence tails, forward output, backward gradients, deterministic repeat, and no OOB under available sanitizer/device diagnostics.
6. Benchmark against stable SDPA under identical shapes and warmup.

GREEN requires the real patched kernel to execute forward and backward with tolerance parity and no compile error/page fault: `GREEN_FLASH_ATTN_GFX1201`. Route production to it only when median end-to-end speed is at least 1.10x stable SDPA and exceeds run-to-run noise; otherwise keep the correct patch/probe but retain SDPA routing.

## Track K3 — PyTorch CK/AOTriton SDPA dispatch

**Upstream RED authority:** `pytorch/pytorch#188113`, CK SDPA flash-attention assertion on `gfx1200/gfx1201`.

1. Reproduce the assertion with a minimal `scaled_dot_product_attention` forward/backward fixture and emit `RED_SDPA_GFX1201`.
2. Record which backend PyTorch selected and why for every test shape.
3. Implement a shape/dtype/architecture capability router that never selects a broken CK/AOTriton kernel. If a small upstream dispatch patch is required, save it under `patches/pytorch-sdpa/` with the pinned SHA.
4. Test stable math/memory-efficient SDPA and any repaired fused backend separately; a fallback must be visible in telemetry, not silent.
5. Cover causal/noncausal, dropout 0, ragged/tail dimensions used by the trainer, BF16 and FP32 reference, forward and backward.

GREEN is `GREEN_SDPA_GFX1201`: no assertion or recompile loop, finite output/gradients, correct backend telemetry, tolerance parity, and 200 repeated calls without failure. Production chooses the fastest correct backend per measured shape; unsupported fused shapes fail over explicitly.

## Track K4 — Unsloth Qwen3.5 fullgraph failure

**Upstream RED authority:** `unslothai/unsloth#6825`, Qwen3.5 SFT on `gfx1201` fails at step 0 with `FailOnRecompileLimitHit` and `fullgraph=True`.

1. Pin current Unsloth and its transitive Triton/PyTorch versions.
2. Reproduce the step-0 failure with the smallest Qwen3.5 LoRA training fixture allowed by the hermetic lane. Emit `RED_UNSLOTH_QWEN35_GFX1201`.
3. Capture graph-break/recompile reasons. Fix the dynamic boundary or partition compilation around it; do not merely raise the recompile limit.
4. Make partial compilation the explicit `gfx1201` default if fullgraph cannot be made stable without semantic changes. Eager fallback remains available.
5. Verify data order, labels, loss, gradients, checkpoint/resume, and compile-cache reuse.
6. Run a 20-step smoke and 200-step bounded stability run on the winner.

GREEN is `GREEN_UNSLOTH_QWEN35_GFX1201`: finite loss and gradients through 200 steps, no step-0 recompile failure, successful resume, matching held-out quality, and measured speed versus eager BF16 stable-SDPA. A partial-compile fix is valid; hiding the failure by disabling all compilation without reporting the performance result is not.

## Track K5 — KFD page faults and dual-R9700 RCCL deadlock

**Upstream RED authorities:** `ROCm/rocm-systems#5480` for dual-R9700 RCCL deadlock, plus the R9700 host-memory/page-fault reports cited by `amd_r9700_training_research_20260728.md`.

1. Keep independent workers as production baseline; do not involve RCCL in K1–K4.
2. Add a single-GPU host-transfer matrix varying pageable/pinned, blocking/nonblocking, and allocator settings under strict memory ceilings. Record KFD/dmesg-visible faults when permissions allow. Never induce system-wide memory pressure.
3. Add timeout-bounded two-rank all-reduce correctness fixtures at small/medium message sizes under default transport first. Emit `RED_RCCL_GFX1201` only for a reproduced hang/error, not for an upstream report alone.
4. Test `HSA_FORCE_FINE_GRAIN_PCIE=1` as a separate opt-in condition only after the default result is captured; never persist it globally.
5. On timeout or page fault: kill every child rank, quarantine partial outputs, verify processes/VRAM are gone, and mark the device dirty until an operator health check.
6. If a minimal RCCL/env/topology fix exists, retain it only after repeated all-reduce correctness and the original hang no longer reproduces. Otherwise emit `RCCL_GFX1201_DISABLED` and keep dual independent workers.

GREEN for synchronized mode requires `GREEN_RCCL_GFX1201`, repeated FP32/BF16 correctness, no hang/page fault, cleanup proof, and the original Task 5 throughput bar of at least 1.25x one-GPU samples/s. Anything less keeps DDP disabled. Independent dual workers remain a successful repair outcome because they use both GPUs without the broken collective path.

## bitsandbytes 0.50.0 integration gate

Do not patch already-fixed 4-bit code until a RED fixture proves a local defect.

- Pin bitsandbytes 0.50.0 or newer known commit.
- Verify its advertised fused ROCm 4-bit GEMM on `gfx1201` against FP32/BF16 dequantized reference for representative Qwen3.5 shapes.
- Verify the LoRA backward path needed by training, not inference alone.
- Explicitly reject ROCm 8-bit optimizer selection because upstream 0.50.0 still withholds parity. Route to BF16 Adafactor/AdamW based on the original optimizer stability gate.
- Emit `BNB_GFX1201_UPSTREAM_FIXED_VERIFIED` only after real-device correctness; otherwise capture the actual RED and create a minimal local patch under a newly justified scope.

## Unified gate and report

`scripts/run_gfx1201_kernel_gate.sh` must support:

- `--dry-run`: validate locks, patches, architecture allow-list, busy-GPU refusal, and command construction without importing GPU libraries;
- `--probe <track>`: run one bounded track;
- `--all`: run all authorized tracks sequentially with per-track timeout and cleanup;
- JSON output with environment, upstream SHAs, patch digests, selected devices, RED evidence, GREEN evidence, correctness error, median/p95 timing, variance, fallback reason, and cleanup result.

Required markers:

```text
GFX1201_EOL_PREFLIGHT_PASS
RED_AITER_GFX1201 / GREEN_AITER_GFX1201 or AITER_GFX1201_PARTIAL
RED_FLASH_ATTN_GFX1201 / GREEN_FLASH_ATTN_GFX1201
RED_SDPA_GFX1201 / GREEN_SDPA_GFX1201
RED_UNSLOTH_QWEN35_GFX1201 / GREEN_UNSLOTH_QWEN35_GFX1201
RED_RCCL_GFX1201 / GREEN_RCCL_GFX1201 or RCCL_GFX1201_DISABLED
BNB_GFX1201_UPSTREAM_FIXED_VERIFIED
GFX1201_KERNEL_GATE_PASS | GFX1201_KERNEL_GATE_BLOCKED | GFX1201_KERNEL_GATE_FAIL
```

`PASS` requires all trainer-required K1–K4 paths to have a correct GREEN or an explicit measured fallback, bitsandbytes verification or explicit non-use, K5 to select either proven RCCL or independent workers, all tests green, and cleanup proof. `BLOCKED` is required when real GPUs remain occupied and no maintenance authorization exists; code-only/static checks cannot be reported as kernel fixes.

## Required final report

Return:

1. exact worktree/branch and commits;
2. EOL cleanup before/after status proving no line-ending contamination was committed;
3. each upstream URL, pinned SHA, local patch, and patch digest;
4. RED reproducer command and exact failure signature for each reproduced defect;
5. GREEN command and exact correctness/performance result on real `gfx1201`;
6. which routes are patched fused kernels, safe accelerated fallbacks, or disabled;
7. 20-step and 200-step LoRA stability results where applicable;
8. process/VRAM/KFD cleanup evidence;
9. honest blocked rows where serving ownership prevented execution;
10. confirmation that `/home/marble/ai-env`, live services, and external repositories were not modified or pushed.

A patch that compiles but was never executed on `gfx1201` is not a fix. A fallback that prevents a crash and passes correctness is a valid compatibility fix, but its performance and withheld fused path must be named explicitly.