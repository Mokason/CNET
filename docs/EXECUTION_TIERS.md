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
- `cce_gpu.c` — the **generic `cce_tensor` device API**. The default
  `cce_gpu_init` creates a CPU-fallback context; CUDA is requested explicitly
  through `cce_gpu_init_cuda`. It never owns the OpenCL backend.
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
| Generic tensor device | `src/cce/cce_gpu.c` | `cce_gpu_init` / `cce_gpu_init_cuda` | `cce_tensor` (struct) | CPU context or explicit CUDA |
| OpenCL kernel | `src/cce/cce_clgemm.c` | `cce_clgemm_open` | `float*` row-major | OpenCL |

**These are not unified.** `cce_gpu_init` always creates the CPU-fallback
context; explicit CUDA uses `cce_gpu_init_cuda`. The OpenCL path is accessed
through `cce_clgemm_open` / `cce_clgemm_matmul`, which has its own device
management and tensor layout. The high-level `.NET` `CceModel.UseDevice`
method does not expose OpenCL; `CceDevice.OpenCl` is reserved and rejected
rather than silently mapped to the wrong backend.

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
uses weak-symbol probes to reject both AICIMO and `cnet_lm` leakage into the
core aggregate. It also verifies that `cce_gpu_init` creates only the generic
CPU-fallback context; explicit CUDA and the separate OpenCL `cce_clgemm` API
remain distinct.

### Smoke builds

- `make aicimo_smoke` — compiles `$(CCE_AICIMO_SRC)` explicitly alongside
  `$(CCE)` and runs the AICIMO smoke test.
- `make cce_smoke` — compiles `$(CCE)` and runs the CCE engine smoke test.