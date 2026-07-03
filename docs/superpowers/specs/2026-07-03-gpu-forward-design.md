# GPU Forward for the CCE Transformer Runner (OpenCL, dynamic) — Design

**Date:** 2026-07-03
**Status:** Draft → building same session (user directive: "next part GPU
integration, since we only used CPU").
**Topic:** GPU-accelerate the oracle forward path (`cce_gguf_qwen2_forward`)
so extraction campaigns stop being bound by single-thread CPU GEMMs — with a
decision-equivalence gate against the CPU path, because a diverging oracle
silently changes the meaning of extracted knowledge.

## Grounded findings (why this design)

1. **The existing CUDA path has never compiled.** The Makefile defines
   `-DC CE_HAVE_CUDA` (typo splitting the macro), `cce_gpu.c` contains CUDA
   `<<<>>>` launch syntax in a `.c` file gcc could never parse, and
   `init_cuda` writes a struct field that doesn't exist. The OpenCL branch
   initializes a context but its matmul is a stub that falls back to CPU.
2. **nvcc on Windows requires MSVC as host compiler** — it cannot feed this
   MinGW toolchain (same class of constraint as the recorded no-ASan and
   -mno-avx findings). Any CUDA-toolkit-based path fights the toolchain.
3. **The forward has ONE seam:** every linear projection (q/k/v/o, gate/up/
   down, lm_head) flows through `apply_linear_rows(cascade, in, out)` in
   cce_gguf.c, which loops rows through `cce_cascade_forward` on CPU. The
   LM head GEMM ([seq×1024]×[1024×262144] ≈ 0.8 GFLOP) dominates.

## Decision

**OpenCL backend with runtime-compiled kernels and a dynamically loaded
`OpenCL.dll`** (LoadLibrary/GetProcAddress; minimal CL types declared in a
private header — no SDK, no import lib, no new toolchain dependency; the
binary runs unchanged on GPU-less machines). NVIDIA's driver ships OpenCL, so
the 4070 Ti Super is served; so is any future AMD/Intel box. The cuBLAS-
dynamic alternative was considered and rejected: vendor lock + version-
coupled DLL names for a workload where even a simple tiled kernel wins
(skinny GEMMs against resident weights are memory-bound).

## Mechanics

- **`src/cce/cce_clgemm.c` + `include/cce/cce_clgemm.h`** (new, self-
  contained): `cce_clgemm_open()` (dynamic-load OpenCL → platform/device/
  context/queue → build the GEMM kernel from embedded source; any failure →
  NULL = CPU-only), `cce_clgemm_close()`,
  `cce_clgemm_matvecs(h, A[T×K] host, W_key, W[K×N] host, C[T×N] host)` —
  T ≤ 8 activation rows against a resident weight matrix. **Weight
  residency:** device buffers cached keyed by host pointer (the model's
  weight tensors are stable for the process lifetime); ~1.5GB device total
  for the gemma fragment (LM head 1.07GB + layers ~0.4GB) — fits 16GB.
- **Seam integration:** `apply_linear_rows` gains a guarded fast path: if a
  global clgemm handle is set AND the cascade is a single plain-float linear
  block (the exact shape `cce_gguf_add_linear_branch` builds), GEMM on GPU
  (+ CPU bias add); otherwise the existing per-row path runs untouched.
  Opt-in via `cce_gguf_qwen2_set_gpu(m, handle)`; default NULL = today's
  behavior byte-for-byte.
- **flagship_run:** `--gpu` (argv or `CNET_GPU=1`) enables; prints backend +
  device name; the thermal governor's GPU reads finally guard real load.
- **Makefile:** fix the `-DCCE_HAVE_CUDA` typo (honesty), add clgemm to
  $(CCE); no new mandatory flags.

## The equivalence gate (load-bearing)

New tool `gpu_equiv` (built like flagship_run, NOT in verify — needs the
model + a GPU): loads the model twice-in-one-process, runs N=64 deterministic
contexts through CPU and GPU paths and reports:
- max |Δlogit| over the full vocab (expect small but nonzero — float
  accumulation order differs; PRINTED, not gated);
- **argmax agreement and window-restricted top-3 agreement: MUST be 100%**
  (decision-identical oracles — the property certification integrity needs);
- forwards/s both paths (the speedup number).
Mining with `--gpu` refuses to start unless the equivalence sweep passes at
startup (same posture as the determinism spot check).

## Non-goals

- No GPU for BTN training (nn.c stays CPU; different milestone if ever).
- No CUDA/cuBLAS; no attention-kernel offload (attention is tiny at seq≤3);
  no quantized (QGKP ternary) GPU path in v1 — plain float cascades only,
  everything else falls back to CPU exactly as now.
- No verify-chain GPU test (the gate machine may be GPU-less): verify covers
  the CPU fallback being byte-identical (existing gates) + the loader's
  clean absent-DLL failure (a unit test that opens with a bogus DLL name).

## Risks, stated

- OpenCL kernel correctness on skinny GEMMs: the equivalence sweep is the
  gate; any mismatch → ship nothing, fall back CPU.
- First-call latency: kernel build + 1.5GB weight upload ≈ seconds — done
  once at startup, amortized over thousands of forwards.
- fp32 associativity: logits differ in ulps between paths; decisions must
  not. If the model has near-tie logits inside the window, top-3 order could
  legitimately flip between paths — the sweep measures this; if it fires,
  the honest resolution is documented (tie-margin threshold or CPU-only for
  TOPK runs).
