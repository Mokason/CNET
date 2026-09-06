# Legacy CUDA seam

This filename is retained for existing links and source archaeology.
CNET's current project policy is AMD/ROCm only. Do not use this page to add a
CUDA-only dependency, run NVIDIA experiments or change the supported backend.

Historical CUDA compatibility sources remain in the tree, including
[cce_cudagemm.h](../include/cce/cce_cudagemm.h). Their presence is not evidence
of current device verification or permission to deploy that backend.
This documentation cleanup does not remove executable compatibility code.

For supported device work, use [GPU training](GPU_TRAINING.md) and
[execution tiers](EXECUTION_TIERS.md). Specify the actual OpenCL/HIP/resident
trainer path, device and precision comparison. A skipped optional-device
test is never a measured GPU pass.

Earlier CUDA setup instructions are recoverable from the
[documentation archive](MAINTENANCE.md) as historical reference only.
