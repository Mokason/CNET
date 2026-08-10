# Optional CUDA (NVIDIA) backend

CNET accelerates the transformer linear seam on AMD via OpenCL
(`cce_clgemm`) and hipBLAS (`cce_hipgemm`), both loaded **dynamically at
runtime**. The CUDA backend is a peer of those: a pure-C module that
`dlopen`s `libcudart` / `cudart64_*.dll` + `libcublas` / `cublas64_*.dll`
and runs cuBLAS `Sgemm`. It requires **no CUDA toolkit, no nvcc, and no
CUDA headers to build**, and it is completely inert unless an NVIDIA GPU +
driver are present at runtime.

**Default policy is AMD-first.** Dual-R9700 Linux production stays on
OpenCL (+ hip). CUDA is opt-in for NVIDIA test machines.

## Files

- `include/cce/cce_cudagemm.h`, `src/cce/cce_cudagemm.c` — dlopen cuBLAS
  GEMM (FP32 + int8 weight-only), peer of `cce_hipgemm`
- Inference wiring: `cce_gguf.{h,c}` (`cudagemm` handle + setters in
  `apply_linear_rows`) and `cce_infer_backend.c`
- Training kernels: pre-existing opt-in `CCE_USE_CUDA=1` + `cce_cuda.cu`
  (separate; still requires nvcc)

## Build

`src/cce/cce_cudagemm.c` is in the default `CCE` list and compiles as
ordinary C — no toolkit:

```sh
make            # CUDA seam compiles in, stays inert without NVIDIA
make cudagemm_res   # smoke gate (skips without driver; CNET_REQUIRE_CUDA=1 fails skip)
```

## Runtime (inference)

```sh
# Linux AMD (default auto): OpenCL + hipBLAS — unchanged
CNET_GPU=1 ./bin/flagship_run <model.gguf> ...

# Force CUDA on an NVIDIA box (Windows/Linux test)
CNET_GPU_BACKEND=cuda CNET_GPU=1 ./bin/...

# Explicit AMD backends
CNET_GPU_BACKEND=opencl CNET_GPU=1 ./bin/...
CNET_GPU_BACKEND=hip CNET_GPU=1 ./bin/...

# Try every vendor (rare)
CNET_GPU_BACKEND=all CNET_GPU=1 ./bin/...
```

`auto` (default / unset): try OpenCL, then hip; if both miss, try CUDA as
last resort (so a CUDA-only laptop still works without setting the env).

Shared knobs: `CNET_GPU_DEVICES`, `CNET_GPU_COUNT`, `CNET_GPU_SPLIT_MB`,
`CNET_GPU_MIN_FLOPS`, `CNET_CUDA_Q8_MB` (int8 expand-cache VRAM budget,
default 8192 MB).

## Numerics

cuBLAS uses FMA → **decision-identical, not bit-identical** to CPU — same
contract as hipBLAS. Do not mint certified/golden bases on the CUDA path
without re-verification.
