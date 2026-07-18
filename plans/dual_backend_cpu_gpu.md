# Dual backend: DS + GGUF, CPU then GPU

## Goal

Keep **DeepSeek-style residual host (DS)** and **GGUF token generation** as
**separate** inference backends with a shared session façade
(`cce_infer_backend`), so each can grow CPU and GPU paths and tests without
coupling runtimes or leaking llama.cpp / external DeepSeek trees into CNET.

## Kinds

| Kind | Runtime | Weights | Token step |
|------|---------|---------|------------|
| **DS** | `cce_ds_host` | synthetic / `.cnetpack` (forest leaves) | residual MLA+MoE+DSA |
| **GGUF** | `cce_gguf_qwen2` | synthetic fixture or real `.gguf` | full transformer forward |

## Devices

| Device | DS | GGUF |
|--------|----|------|
| **CPU** | live (`cce_ds_host_bench`) | live (`cce_gguf_qwen2_forward`) |
| **GPU** | reserved → `CCE_ERR_UNSUPPORTED` until MLA/expert clgemm wire | **OpenCL `cce_clgemm`** primary (AMD dual R9700); soft-skip if none |

**AMD decision:** pure-C **OpenCL first**, optional **hipBLAS** later, **Vulkan deferred** for CCE
(belongs to whole-engine runners / llama harness). Full write-up: [`plans/amd_gpu_backend.md`](amd_gpu_backend.md).

## Gates (current)

| Target | Marker | Role |
|--------|--------|------|
| `make ds_stack` | `DS_STACK_PASS` | isolated DS residual stack |
| `make cnet_ds_bench` | `DS_BENCH` | DS microbench |
| `make gguf_stack` | `GGUF_STACK_PASS` | synthetic GGUF residual/token stack |
| `make cnet_gguf_bench` | `GGUF_BENCH` | GGUF microbench |
| `make dual_cpu_bench` | `DUAL_CPU_BENCH_PASS` | both CPU backends side-by-side |
| `make gguf_gpu` | `GGUF_GPU_PASS` / `SKIP` | CPU↔OpenCL argmax identity |
| `make dual_gpu_bench` | `DUAL_GPU_BENCH_PASS` / `SKIP` | DS CPU + GGUF CPU/GPU |

## Next (GPU integration per backend)

1. ~~**GGUF GPU tests**~~ — `make gguf_gpu` (OpenCL decision identity).
2. **DS GPU path**: latent/MLA matmuls and MoE expert apply via clgemm;
   keep forest leaf residency on host until upload policy is explicit.
3. ~~**Dual GPU bench**~~ — `make dual_gpu_bench`.
4. **hipBLAS opt-in** (`CNET_GPU_BACKEND=hip`) for large GEMM on R9700 — see amd plan.
5. **No shared process-global GPU handle** for concurrent sessions — per-session
   `cce_clgemm` already preferred for oracle pools.

## Isolation rules

- No DeepSeek repo / no llama.cpp runtime in the DS or GGUF CCE paths.
- GGUF is optional import only; CNET identity remains forest names.
- Runtime blobs (`.gguf` fixtures written by tests, `.cce` archives) are not
  committed; gates clean up after themselves.
