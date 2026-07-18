# AMD GPU backend for CNET (pure C) — research, decision, measured results

**Host:** dual **AMD Radeon AI PRO R9700** (`gfx1201`) + iGPU excluded, ROCm 7.2.3, OpenCL 2.0, hipBLAS.

**Constraints:** pure **C**, CNET-native path (no llama.cpp for these gates), dual discrete must not be slower than single without reason.

## Decision (binding)

| Role | Backend | Notes |
|------|---------|--------|
| **Primary full-forward** | **OpenCL `cce_clgemm`** | Dual discrete, int8 + FP, size floor, iGPU out |
| **Secondary / large FP GEMM** | **hipBLAS `cce_hipgemm`** | Pure C dlopen; peak single-matrix; `CNET_GPU_BACKEND=hip` |
| **Vulkan** | Deferred for CCE | Whole-engine (llama) tool, not residual linear seam |
| **DS GPU** | Only if CNET DS host later | Not external DeepSeek |

Env: `CNET_GPU_BACKEND=auto|opencl|hip`, `CNET_GPU_COUNT`, `CNET_GPU_DEVICES`, `CNET_GPU_SPLIT_MB` (default 8), `CNET_GPU_MIN_FLOPS` (default 1M — skip PCIe-bound micro GEMMs).

## Why dual was slower before

1. Tiny models (d=8): PCIe + kernel launch ≫ compute → GPU loses.
2. Dual without size floor: every micro-matvec paid GPU tax.
3. hip multi-device serialized H2D/D2H per device (fixed: launch all sgemms then download).
4. hip-only full forward left **int8** specialists on CPU (hip has no q8 path yet).

## Measured results (this host, 2026-07-18)

### Raw GEMM (T=1, no model)

After local-A tile kernel (still k-ascending / bit-identical):

| Path | K=N=4096 | Notes |
|------|----------|--------|
| CPU (naive) | ~0.5 GFLOP/s | |
| OpenCL ×1/×2 | **~84 GFLOP/s** | was ~34 before tile |
| hipBLAS ×1 | ~130 GFLOP/s | |
| hipBLAS ×2 | **~186 GFLOP/s** | dual wins on large GEMM |

Gate: `make gpu_matmul_bench` → `GPU_MATMUL_BENCH_PASS`.

### Full CNET GGUF forward — **Qwythos-9B** (no llama.cpp)

`bin/cnet_gguf_gpu_bench …/Qwythos-9B-…-Q8_0.gguf 4 opencl`  
Load ~3 min; n=4; `CNET_FOREST_NO_PERSIST=1`.

| Mode | CPU tok/s | GPU tok/s | Speedup | Notes |
|------|-----------|-----------|---------|--------|
| **OpenCL ×2 + int8** (`CNET_ORACLE_INT8=1`) | ~1.29 | **~10.0** | **~7.8×** | tiled kernel + A-cache; **production path** |
| OpenCL ×2 + FP (`CNET_INFER_FP=1`, earlier) | ~1.16 | ~3.4 | ~3.0× | before tile; FP more PCIe |
| hip ×1 / ×2 | ~1.1–1.2 | ~1.4 | ~1.2× | no q8 path yet |

**Deploy tip:** prefer **int8 specialists + OpenCL dual**; avoid `CNET_INFER_FP=1` when chasing tok/s.

Hermetic tiny identity: `make gguf_gpu` → `GGUF_GPU_PASS`, max|Δlogit|=0, dual gfx1201.

**Gemma** `Models/gemma-4-12B-it-MTP-Q8_0.gguf` is **not** a full CNET-loadable trunk here (49 tensors, no attn_k/v — refuse). Use Qwythos or a complete gemma GGUF.

## Gates / tools

| Target | Role |
|--------|------|
| `make gpu_matmul_bench` | CPU vs OpenCL×1/×2 vs hip×1/×2 |
| `make gguf_gpu` | Tiny fixture CPU↔OpenCL decision identity |
| `make dual_gpu_bench` | Dual scaffold |
| `make gguf_gpu_real MODEL=… N=… BACKEND=…` | Real CNET GGUF GPU bench |

## Speed work landed

1. Local-memory **A-tile** OpenCL kernels (FP + q8), k-ascending bit-identity kept.
2. **Activation fingerprint cache** — skip H2D when q/k/v or gate/up reuse the same rows.
3. hipBLAS **A cache** + multi-device launch/download split.
4. Size floor + dual column-split + iGPU exclude (earlier).
5. **GPU attention** (`cce_clgemm_attn_decode`) when prefix ≥ 64 tokens (classic + qwen35 full-attn).
6. **GPU silu×up** for single-token FFN mid.
7. **hip int8**: expand-to-float LRU (`CNET_HIP_Q8_MB`, default 8 GiB) + sgemm.
8. RMS/add/silu OpenCL helpers for residency chain.

## Residual stream + device KV (landed)

`cce_cl_stream.h` on primary OpenCL device (same CCE residual, not a second model):

| API | Role |
|-----|------|
| `stream_bind` / `set_x` / `get_x` | residual x[D] on device |
| `stream_rms_x` | ln = rms(x)*w on device → matmul A |
| `stream_linear_{fp,q8}` | linear from device ln → host C |
| `stream_linear_{fp,q8}_slot` | linear from device ln → Q/K/V/AO/TMP slot |
| `stream_rope_slot` | NEOX RoPE on Q or K slot (optional freq factors) |
| `stream_kv_write` / `kv_write_slots` | host or device-slot K/V → device cache |
| `stream_attn` | host Q + device KV → host out |
| `stream_attn_dev` | device Q slot + device KV → AO slot |
| `stream_use_slot_as_a` | AO (or other slot) as next matmul A |
| `stream_add_x_host` | x += delta |

Wired:
- **Classic qwen2 decode**: full stream layer (rms → slot QKV → device RoPE → kv_write_slots → attn_dev → o_proj → FFN). Soft-fail restores pre-layer residual checkpoint then host path.
- **qwen35 full-attn**: residual stream + stream Q/K/V linears; per-head QK-norm + YaRN still host (device NEOX ≠ YaRN); device KV + stream_attn. GDN layers host residual.

`CNET_GPU_STREAM=0` disables. Measured (dual gfx1201, Qwythos Q8): N=4 ~8.4 tok/s (~7.4×), N=64 ~7.6 tok/s (~6.8×) vs ~1.1 CPU.

## Next levers (landed 2026-07-18)

1. **Device YaRN + per-head QK-norm** (`cnet_rope_yarn`, `cnet_head_rms`, pack_q, gate_ao) on qwen35 full-attn decode; host fallback if any step soft-fails.
2. **Device FFN**: gate/up → slots → `silu_mul_slots` → down → `add_x_slot` (no host mid). Residual host download skipped between consecutive full-attn layers.
3. **Dual-GPU**: stream matmul handles already-split residents; concurrent gate@d0+up@d1 via `stream_linear_pair_slots` (**opt-in** `CNET_GPU_PAIR=1` — T=1 decode often PCIe-slower). Non-stream column-split GEMM still dual by default.

Measured (Qwythos Q8, dual gfx1201): N=4 ~7.4 / N=16 ~7.3 tok/s (~6.5× CPU). Hermetic `GGUF_GPU_PASS`. Synthetic DS microbench (~719 tok/s) remains a different scale (tiny residual stack).

## Further headroom

1. Device Gated-DeltaNet (hybrid layers still host — biggest Qwythos gap).
2. Fused kernels (rms+linear, silu+down) to cut launch overhead.
3. True dual residual pipeline (layer i on d0, i+1 on d1) without per-step A bcast.

## Isolation

No llama.cpp / Python in CCE GPU path. Runtime blobs not committed. iGPU never selected (OpenCL host-unified filter; hip `hipDeviceAttributeIntegrated`).
