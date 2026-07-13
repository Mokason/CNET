# CNET Execution Tiers

This document describes the three execution tiers of the CNET CCE build:
core Specialist/SoulHost, model-kernel acceleration, and quarantined legacy
experiments. It exists to make the dual GPU backend roles honest and to
prevent accidental re-coupling of experimental code into the core aggregate.

## Tier 1 — Core CCE Aggregate (`$(CCE)`)

The `$(CCE)` Makefile variable is the set of source files compiled into every
CCE build, including `cce.dll` / `cnet.so` (the P/Invoke and MCP shared
libraries). It includes:

- Tensor, block, cascade, archive, forest, router, sparse-kv, uncertainty,
  compression, learn, patch — the core CCE engine.
- `cce_gpu.c` — the **generic GPU API** (CUDA-or-CPU fallback). This provides
  `cce_gpu_init` (which returns `CCE_GPU_NONE` or `CCE_GPU_CUDA`, never
  `CCE_GPU_OPENCL` in the default build) and `cce_gpu_init_cuda`.
- `cce_clgemm.c` — the **OpenCL model-kernel backend**. This is a separate
  acceleration path with its own tensor contract (`float*` row-major, not
  `cce_tensor`). It dynamically loads `libOpenCL.so.1` / `OpenCL.dll` and
  compiles/resident-caches CL kernels for matmul and int8-q8 matmul. It is
  linked into `$(CCE)` as a core source but is a distinct backend from
  `cce_gpu` — the two are **not unified** and have different tensor layouts.
- Model I/O, dataset, autograd, safetensors, GGUF reader, QGKP, detect, SSM,
  st_llama, specgraph, weight store, tier runtime, similar, transformer QAT.

### What is NOT in `$(CCE)`

The following sources are deliberately **excluded** from the core aggregate:

- `src/cce/cce_aicimo.c` — AICIMO adapter router (experimental, quarantined).
- `src/cce/cce_aicimo_bridge.c`, `cce_aicimo_preservation.c`,
  `cce_aicimo_role_slice.c` — AICIMO bridge/role-slice files (experimental).
- `src/cnet_lm.c` — second training/generation/head-routing path (legacy).

These are reachable only through explicit experimental/legacy targets.

## Tier 2 — Model-Kernel Acceleration

Two GPU backends exist, each with a distinct role:

| Backend       | Source            | API entry point       | Tensor contract        | Status     |
|--------------|-------------------|-----------------------|------------------------|------------|
| Generic GPU  | `src/cce/cce_gpu.c`  | `cce_gpu_init`       | `cce_tensor` (struct)  | CUDA-or-CPU |
| OpenCL kernel| `src/cce/cce_clgemm.c` | `cce_clgemm_open`  | `float*` row-major     | OpenCL     |

**These are not unified.** `cce_gpu_init` does NOT return `CCE_GPU_OPENCL` in
the default build (it returns `CCE_GPU_NONE` on CPU-only builds). The OpenCL
path is accessed through `cce_clgemm_open` / `cce_clgemm_matmul`, which has
its own device management and tensor layout. The `.NET` enum `CceDevice.OpenCl`
maps to the cce_clgemm backend, not to `cce_gpu_init`.

## Tier 3 — Quarantined Legacy Experiments

| Source                          | Target(s) using it          | Notes                        |
|---------------------------------|-----------------------------|------------------------------|
| `src/cce/cce_aicimo.c`          | `aicimo_smoke`              | Explicit compilation; not in `$(CCE)` |
| `src/cnet_lm.c`                 | `glyph_habitat`, `build_tool`| Legacy/experimental targets only |
| `src/cce/cce_aicimo_bridge.c`   | (not in Makefile)           | Placeholder/TODO code        |
| `src/cce/cce_aicimo_preservation.c` | (not in Makefile)       | Placeholder/TODO code        |
| `src/cce/cce_aicimo_role_slice.c` | (not in Makefile)         | Placeholder/TODO code        |

### Regression gate

`make alt_paths_gate` compiles `tests/test_alt_paths_gate.c` with `$(CCE)` and
verifies at link-time (via weak symbols) that AICIMO is not transitively in the
core aggregate, and at runtime that `cce_gpu_init` does not return OpenCL.

### Smoke builds

- `make aicimo_smoke` — compiles `$(CCE_AICIMO_SRC)` explicitly alongside
  `$(CCE)` and runs the AICIMO smoke test.
- `make cce_smoke` — compiles `$(CCE)` and runs the CCE engine smoke test.