# Bounded AMD inference offload

The additive [harness API](../include/cnet_harness.h)
`cnet_harness_open_with_offload` accepts a versioned policy without changing
legacy ABI-v1 open. This is llama.cpp inference offload, not GPU training.

## Policy

A request selects a positive layer count strictly below the model's declared
transformer layer count and one to four distinct dedicated-device indices.
Indices refer to the backend's filtered enumeration, not an assumed PCI label.
Integrated devices are excluded by this path.

One device uses no split; multiple devices use layer splitting. Row/tensor
splits are unsupported. Tensor weights must be finite and nonnegative;
normalization and equal-weight defaults are handled by the policy.
An optional per-device VRAM delta cap is checked after model/context creation.

Invalid metadata, unavailable/duplicate devices, a full-layer request or an
exceeded cap closes/refuses without publishing a session. The cap measures a
steady-state free-memory delta, not a hard allocator quota or peak-process
memory bound. Other GPU users can affect that observation.

## Build and validation

The bounded hybrid benchmark uses a separate ROCm llama.cpp build with
`GGML_HIP_GRAPHS=OFF`. Its gate checks the CMake cache; do not silently
reuse a graph-enabled full-GPU deployment build.

`make cnet_harness_gpu_benchmark` needs its configured local model/backend
and occupies real GPU resources. It is not a routine documentation smoke test.
Record exact model bytes, prompt/context, threads, greedy output, warmup,
selected layers/devices, VRAM baseline/peak/recovery and repeated timing.

The original Bonsai-8B experiment found a single-device partial layer tier
useful; its equal 12-layer dual-device split was slightly slower and consumed
more combined board power. Those tables are retained in the
[archive](MAINTENANCE.md), not promoted to universal defaults.

Use [harness guide](cnet_dotnet_inference_harness.md) for ownership and
[GPU training](GPU_TRAINING.md) for the separate resident training workload.
