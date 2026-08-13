# Opus 5 partition — dual-R9700 training-rate program

**Agent:** Claude Opus 5, xHigh effort, through Herdr only  
**Research authority:** `/home/marble/AI/CNET/plans/amd_r9700_training_research_20260728.md`  
**Execution mode:** sequential tasks in a dedicated worktree/branch; do not edit the operator's active worktree directly.  
**Goal:** materially improve time-to-certified Tamagotchi/CNET specialist by using both R9700s safely, while retaining deterministic simulation authority and exact certification bars.

## Global rules

1. Read the research authority, `docs/RELEASE_POLICY.md`, `plans/amd_gpu_backend.md`, `tools/campaign_v2_fast.sh`, `tools/mining_campaign.sh`, `include/async_runtime.h`, and the Alive Valley README before code changes.
2. Create a dedicated branch/worktree from current CNET HEAD, e.g. `feature/amd-r9700-training-rate`; do not mix work into the active `feature/vision-portable-specialist` worktree.
3. Never `pip install` into `/home/marble/ai-env`. Use a hermetic venv/container/cache path and pin every dependency.
4. Do not stop, restart, or evict the current GPU serving processes without explicit operator authorization. GPU tests must refuse with a clear busy verdict unless an explicit maintenance-window token is supplied.
5. Allow-list discrete `gfx1201` in every GPU path. The current integrated device is `gfx1036`, but discovery must not rely on that deny-list or trust ordinal 0/1 blindly.
6. Every long or potentially hanging operation uses `timeout`; sweep child processes and verify VRAM/process cleanup.
7. No CUDA-only assumptions. PyTorch's `torch.cuda` namespace on ROCm is allowed; CUDA packages/toolchains are not.
8. Unity/Alive Valley remains authoritative. The learner emits bounded IDs only and cannot mutate state or saves.
9. Keep certification/quality thresholds unchanged. Performance wins that degrade held-out/counterfactual quality are rejected.
10. No push or external publication. Local commits are allowed after gates.

## Task 1 — Build the deterministic training corpus and benchmark contract

**Purpose:** create the missing reproducible input to any training-rate claim.

**Exclusive file scope:**

- `/home/marble/AI/AliveValleyDemo/Assets/Scripts/Core/TrainingExport.cs` (new)
- `/home/marble/AI/AliveValleyDemo/Assets/Tests/EditMode/TrainingExportTests.cs` (new)
- `/home/marble/AI/AliveValleyDemo/Tools/HeadlessTestRunner/*` only if required to include the new test file
- `/home/marble/AI/AliveValleyDemo/docs/TRAINING_CONTRACT.md` (new)
- generated artifacts only under `/home/marble/AI/AliveValleyDemo/Artifacts/training/` and never committed

**Deliverables:**

- Versioned binary or JSONL schema for `CreatureState x bounded RecentEvents x Personality -> ActionID + MoodID + ResponseID`.
- Export from deterministic seeded trajectories, including world/seed/tick, causal event IDs, and replay hash.
- Fixed closed vocabularies for all output IDs. No free-form action or direct mutation.
- Seed-sharded generation so CPU cores can feed GPU workers without changing per-seed results.
- Manifest with dataset digest, schema version, seeds, record count, class distribution, and generation wall time.

**Gates:**

- Same seeds produce byte-identical dataset and manifest digest on rerun.
- Single-worker and multi-worker generation produce identical sorted records/digest.
- Existing 30 tests remain green plus new export tests.
- Simulation hash and snapshot round-trip remain unchanged.
- Dataset generation benchmark reports records/s and does not add allocations to steady simulation ticks.

**Stop/rollback:** if export requires Unity state mutation, float logic in Core, or unbounded event history, redesign before proceeding.

## Task 2 — Add a measured, size-aware native CNET training backend

**Purpose:** accelerate genuinely large batched action-head/native LoRA work without slowing the tiny policy.

**Exclusive file scope:**

- `src/cce/cce_lora.c`
- `include/cce/cce_lora.h`
- new native backend files under `src/cce/` and `include/cce/` named `cce_lora_batch*`
- `tests/test_cce_lora.c`
- new `tests/cce_lora_train_bench.c`
- Makefile entries limited to the new gate

**Deliverables:**

- Batched formulation of forward and `dA`/`dB` updates; remove the current per-sample scalar hot loop where mathematically safe.
- Optimized CPU baseline using existing deterministic/OpenMP conventions.
- Optional GPU implementation using the existing OpenCL/hipBLAS seams with model/optimizer buffers resident across steps.
- Runtime size floor selecting GPU only when measured faster; CPU remains the default for Tamagotchi-sized shapes.
- Backend telemetry: selected backend, shape, batch, transfer time, compute time, steps/s, and fallback reason.

**Gates:**

- Default legacy API behavior unchanged.
- Deterministic repeatability within each backend.
- Finite loss/weights and matching held-out accuracy versus CPU reference within a preregistered tolerance.
- ASan/UBSan and allocation-failure paths.
- Benchmark includes tiny, medium, and large shapes; GPU is never recommended for a shape where it loses.
- Retain GPU implementation only if at least one realistic batched shape is at least 1.50x faster than optimized CPU. A clean negative result with a correct CPU size floor is acceptable.

**Stop/rollback:** do not claim GPU acceleration from synthetic large GEMM if end-to-end training including transfers is slower.

## Task 3 — Create the hermetic R9700 PyTorch LoRA lane

**Purpose:** train the larger mood/dialogue/response specialist on one R9700 reproducibly.

**Exclusive file scope:**

- new `training/rocm/` directory in CNET
- new `scripts/run_rocm_lora_bench.sh`
- new `tests/test_rocm_training_contract.sh`
- no modification of `/home/marble/ai-env`

**Deliverables:**

- Locked environment recipe for the current stable baseline and a separate ROCm 7.14/PyTorch 2.12 candidate. Prefer a container or isolated venv with explicit ROCm wheel index and hashes.
- Trainer consumes Task 1 manifest, masks one discrete `gfx1201`, and records complete provenance.
- BF16 stable-SDPA baseline; compare Adafactor with BF16 AdamW. FP16 is not the default.
- Feature-gated probes for hipBLASLt, `torch.compile`, Unsloth, bitsandbytes/prequantized loading, and fused attention. Known-risk paths are OFF until their probe passes.
- Cache tokenized immutable examples; tune DataLoader workers/prefetch/pinned memory by measurement.
- Atomic checkpoint/resume and NaN/page-fault detection.

**Benchmark matrix:**

- eager BF16 + stable SDPA;
- compile OFF versus safe partial compile;
- `TORCH_BLAS_PREFER_HIPBLASLT` OFF/ON where the model is eligible;
- Adafactor versus AdamW BF16;
- local ROCm 7.2 environment versus hermetic ROCm 7.14/PyTorch 2.12;
- 20-step smoke followed by a bounded 200-step stability run for winners.

**Gates:**

- Finite loss and gradients, deterministic data order, successful resume, no iGPU use.
- Exact environment lock and JSON benchmark report.
- Quality parity on held-out/counterfactual set before a speed winner is accepted.
- No changes to serving Python environments or services.

**Stop/rollback:** on GPU page fault/reset, stop the run, collect logs, verify KFD/process cleanup, and do not auto-resume on a dirty device.

## Task 4 — Integrate two independent R9700 training/oracle workers

**Purpose:** make both GPUs useful without RCCL by default.

**Exclusive file scope:**

- new `tools/dual_r9700_training.sh`
- `tools/campaign_v2_fast.sh`
- `tools/mining_campaign.sh` only through an additive explicit GPU mode
- new `config/cnet-training-window.env.example`
- new tests under `tests/test_dual_r9700_training*`
- no service unit changes outside new opt-in training units

**Deliverables:**

- Dry-run-first launcher that discovers the two discrete R9700s, rejects the iGPU, checks active GPU processes/VRAM, and requires an explicit maintenance-window authorization value before real work.
- Two independent workers: one candidate or campaign shard per GPU, each with isolated logs, checkpoints, seeds, and output directories.
- Reuse CNET's existing async oracle lane pool and `resource_mask`; do not implement a second queue system.
- Deterministic shard manifest and fail-closed merge. Conflicting artifacts or mismatched provenance refuse merge.
- Bounded CPU producer pool so simulation/tokenization does not starve either GPU.
- Cancellation, timeout, disk/RAM/VRAM ceilings, and cleanup/restore verification.

**Gates:**

- Hermetic fake-GPU tests cover busy refusal, iGPU exclusion, child failure, timeout, partial output quarantine, merge mismatch, and cleanup.
- Real maintenance-window benchmark compares the same two-candidate workload serially on one GPU versus concurrently on both.
- Acceptance target: at least 1.70x candidate throughput and no quality/provenance change.
- Existing `make unified_gpu` remains green and is cited as the oracle-lane substrate.

**Stop/rollback:** never automatically stop or restart serving workloads. If a card is busy, report `GPU_TRAINING_WINDOW_BLOCKED` and exit nonzero.

## Task 5 — Add fail-closed RCCL/DDP evaluation and close the evidence loop

**Purpose:** determine whether a synchronized two-GPU run is worthwhile on this PCIe topology; retain only measured value.

**Exclusive file scope:**

- new `training/rocm/probe_rccl.sh` (torch RCCL probe is WITHHELD / out of tree)
- new `training/rocm/run_ddp_probe.sh`
- new `training/rocm/benchmark_matrix.sh`
- new `tests/test_ddp_probe_contract.sh`
- `plans/amd_r9700_training_research_20260728.md` results appendix
- README/ARCHITECTURE/CHANGELOG only after measured gates

**Deliverables:**

- Timeout-bounded two-rank all-reduce probe with correctness checks over several message sizes and dtypes.
- Record RCCL topology/logs and test default transport first. Evaluate `HSA_FORCE_FINE_GRAIN_PCIE=1` only as a separate experiment.
- Short DDP LoRA benchmark using equal effective batch and the same dataset/seed as one-GPU baseline; include `gradient_as_bucket_view`, static graph eligibility, and bucket-size sweep.
- FSDP probe only if a selected model genuinely cannot fit one R9700; otherwise document `not_applicable`.
- Final machine-readable report comparing CPU, one GPU, dual independent workers, and optional DDP.

**Gates:**

- No collective hang across repeated probes; watchdog kills all ranks and verifies cleanup.
- Bit/within-tolerance all-reduce correctness for FP32 and BF16.
- At least 200 stable training steps before DDP can be recommended.
- DDP retained only if it reaches at least 1.25x one-GPU samples/s at equal effective batch and matching quality. Otherwise emit `DDP_NOT_RECOMMENDED` and keep independent workers as production default.
- `git diff --check`, targeted tests, relevant CNET umbrellas, and one final process/VRAM sweep.

## Final report required from Opus 5

Return:

1. branch/worktree and commit IDs;
2. exact files changed per task;
3. commands actually executed and exact PASS/FAIL markers;
4. measured throughput/quality table with honest skipped/blocked rows;
5. whether production default is CPU, one GPU, independent dual GPU, or DDP for each training layer;
6. all remaining blockers, including the Unity license and any lack of authorized GPU maintenance window;
7. confirmation that no service was stopped and no external push occurred.

A plan, stub, or simulated benchmark is not completion. However, if real GPU execution is blocked by active serving processes and no maintenance authorization, finish all hermetic code/tests, report the blocker, and do not fabricate device results.
