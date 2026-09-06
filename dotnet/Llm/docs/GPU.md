# Managed device boundary

The vendor [CUDA assembly](../src/CNET.Llm.Cuda/) is NVIDIA-specific;
it is not an AMD backend even when an option is named `gpu`.
[CNET.Cce.Llm](../../Cce.Llm/README.md) runs managed inference on CPU and
rejects GPU masks/offload policies.

Native CNET has separate AMD OpenCL/HIP inference and resident training.
Use [GPU training](../../../docs/GPU_TRAINING.md) or
[bounded inference offload](../../../docs/cnet_bounded_gpu_offload.md).

Collective/send/receive methods in managed backends currently throw.
Their interfaces do not establish multi-GPU execution.
Original kernel/layout notes and expected-performance tables are retained
in the [archive](../../../docs/MAINTENANCE.md), not current AMD measurements.
