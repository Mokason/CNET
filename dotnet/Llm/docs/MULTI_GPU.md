# Managed multi-device scope

Tensor/pipeline parallelism, NCCL deployment and ParallelismConfig examples
in the old guide were design proposals, not working managed CNET features.

[CpuBackend](../src/CNET.Llm.Cpu/CpuBackend.cs) and the vendor CUDA backend
throw for collective/send/receive operations. An interface method or device
list does not establish model-parallel execution.

CNET's supported two-AMD-device experiments live in the
[native GPU guide](../../../docs/GPU_TRAINING.md).
[Bounded inference offload](../../../docs/cnet_bounded_gpu_offload.md)
is another separate native harness policy. Neither implements the managed
design by implication.

Original topology diagrams and estimates are retained in the
[archive](../../../docs/MAINTENANCE.md). Any future implementation needs
device ownership, partial-failure cleanup, cache lifetime, numerical parity
and measured scaling; do not advertise an unexecuted speedup.
