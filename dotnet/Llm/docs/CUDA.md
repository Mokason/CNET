# Historical vendor CUDA reference

[CNET.Llm.Cuda](../src/CNET.Llm.Cuda/) loads NVIDIA driver/cuBLAS libraries.
It is not ROCm. Generic `gpu` options must not be represented as using AMD.

CNET's project policy is AMD/ROCm only. Old PTX/toolkit setup instructions
remain in the [archive](../../../docs/MAINTENANCE.md) for source history,
not supported operation. No CUDA installation or new CUDA-only path belongs
to this cleanup.

Use [GPU.md](GPU.md) for boundaries and the
[native GPU guide](../../../docs/GPU_TRAINING.md) for supported work.
Old expected throughput/parity statements need actual device evidence.
