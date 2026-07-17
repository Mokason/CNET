# Bounded GPU Offload for the CNET .NET Harness

## Goal

Add an opt-in ROCm generation mode that keeps most GGUF weights on CPU while offloading a caller-bounded, strictly partial set of transformer layers to one or more explicitly selected dedicated GPUs.

**Place:** the `.NET -> libcnet_harness.so -> llama.cpp` session-open path.

**Dilemma:** full offload is fast but creates large, persistent VRAM and power pressure; CPU-only generation is resource-quiet but slow.

**Consequence:** an unbounded or implicit GPU toggle can evict foreground workloads, accidentally select the integrated GPU, or claim acceleration without proving residency and output behavior.

## Architecture decision

Preserve `CNET_HARNESS_ABI_VERSION == 1` and the exact layout and behavior of `CnetHarnessConfig` and `cnet_harness_open`.

Add an independent, versioned `CnetHarnessOffloadPolicy` plus:

```c
int cnet_harness_open_with_offload(
    const CnetHarnessConfig *config,
    const CnetHarnessOffloadPolicy *policy,
    CnetHarnessSession **session_out);

int cnet_harness_get_offload_info(
    CnetHarnessSession *session,
    CnetHarnessOffloadInfo *info_out);
```

Legacy callers continue through `cnet_harness_open` unchanged. Managed callers use the additive entry point only when `CnetHarnessConfig.GpuOffload` is non-null.

## Policy contract

- `gpu_layer_count`: positive and strictly smaller than the GGUF's declared transformer block count.
- `device_count`: 1–4.
- `device_indices[4]`: distinct indices into llama.cpp's dedicated-GPU enumeration after `ROCR_VISIBLE_DEVICES` / `HIP_VISIBLE_DEVICES` filtering. Integrated GPUs are excluded.
- `split_mode`:
  - `NONE` for one selected GPU;
  - `LAYER` for two or more selected GPUs;
  - row/tensor parallel modes are rejected.
- `tensor_split[4]`: finite, non-negative weights with positive total for selected devices. One-device mode normalizes to 1. Equal weights are used when multi-device weights are all zero.
- `offload_kqv`: boolean. It controls llama.cpp K/Q/V offload without changing the weight-layer cap.
- `max_vram_bytes_per_device`: optional hard steady-state delta cap. Zero disables this secondary cap. The primary no-full-load safety remains the layer-count invariant.

The selected policy is copied into the private session before backend open. No environment variables or process-global mutable policy are used.

## Strictly partial preflight

Before `llama_model_load_from_file`:

1. Resolve/materialize the actual GGUF path.
2. Parse metadata using CNET's `cce_gguf_load` and `cce_gguf_get_n_layer`.
3. Reject unknown/non-positive block count.
4. Reject `gpu_layer_count >= model_layer_count`.
5. Enumerate only `GGML_BACKEND_DEVICE_TYPE_GPU` devices.
6. Resolve every requested index; reject duplicates or missing devices.
7. Snapshot each selected device's free VRAM.

Then call llama.cpp with:

- explicit NULL-terminated `params.devices`;
- `params.n_gpu_layers = gpu_layer_count`;
- `LLAMA_SPLIT_MODE_NONE` or `LLAMA_SPLIT_MODE_LAYER`;
- `params.main_gpu = 0` within the explicit selected-device list;
- normalized tensor split for multi-GPU mode.

After model and context creation, measure per-device free-VRAM deltas. If any configured cap is exceeded, close transactionally and return `CNET_HARNESS_ERR_BACKEND` without publishing the session.

## Hybrid ROCm backend build

The pinned llama.cpp default enables `GGML_HIP_GRAPHS`. That is useful for stable full-GPU graphs, but a measured four-layer hybrid run repeatedly warmed changing scheduler subgraphs and exceeded 180 seconds for the fixed 64-token acceptance workload. The CPU control completed the same workload in 5.02 seconds of decode time.

Bounded hybrid execution therefore uses a separate llama.cpp build with graph capture disabled:

```bash
cmake -S /home/marble/llama.cpp \
  -B /home/marble/llama.cpp/build-rocm-nographs \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=ON \
  -DGGML_HIP=ON -DGGML_HIP_GRAPHS=OFF \
  -DGGML_NATIVE=ON -DGGML_OPENMP=ON \
  -DLLAMA_BUILD_TESTS=OFF -DLLAMA_BUILD_EXAMPLES=OFF \
  -DLLAMA_BUILD_SERVER=OFF -DLLAMA_BUILD_TOOLS=OFF
cmake --build /home/marble/llama.cpp/build-rocm-nographs --target llama -j16
```

`make cnet_harness_gpu_benchmark` links the plugin to this build and refuses to run unless its CMake cache proves `GGML_HIP_GRAPHS=OFF`. The normal `build-rocm` tree remains unchanged for full-GPU services.

## Managed API

```csharp
new CnetHarnessConfig
{
    Resource = CnetHarnessResource.Gpu0,
    GpuOffload = new CnetHarnessGpuOffload
    {
        LayerCount = 8,
        DeviceIndices = [0],
        OffloadKqv = true,
        MaxVramBytesPerDevice = 2UL * 1024 * 1024 * 1024,
    },
    // existing fields unchanged
};
```

`CnetHarnessSession.OffloadInfo` reports the applied layer count, model layer count, split mode, selected device count, and observed per-device VRAM deltas. It is null for legacy sessions.

## Validation and TDD slices

1. **Additive ABI RED -> GREEN**
   - old `CnetHarnessConfig` size and ABI version remain unchanged;
   - new policy rejects bad version/size, zero/negative layer count, duplicate/out-of-range indices, invalid split mode/weights, CPU resource plus GPU policy, and full-layer requests;
   - legacy open still reaches the same backend fake without offload state.
2. **Managed projection RED -> GREEN**
   - null policy uses `Open`;
   - non-null policy uses `OpenWithOffload` and projects every field exactly;
   - invalid policy fails before P/Invoke;
   - introspection projects exact native evidence.
3. **Real ROCm RED -> GREEN**
   - CPU and bounded GPU sessions use identical model bytes, prompt, context, seed, greedy sampler, and output cap;
   - generated token count is nontrivial (at least 64-token limit);
   - deterministic text is identical for the accepted benchmark model;
   - `0 < applied_gpu_layers < model_layers`;
   - observed VRAM delta is nonzero and below the configured cap;
   - bounded GPU generation latency is lower than the warmed CPU baseline;
   - session disposal returns selected devices to baseline within an explicit tolerance;
   - no model server or harness process remains.

## Benchmark matrix

On an idle host, benchmark single GPU layer counts `4, 8, 12` with a 2 GiB/device cap. Warm each session once, then measure an identical 128-token deterministic generation. Select the smallest layer count that produces a repeatable latency advantage while remaining under the cap.

Only after single-GPU success, test the same selected layer count across devices `[0,1]` with an equal layer split. Accept multi-GPU mode as recommended only if it beats the single-GPU result under the same output and residency constraints. PCIe transfer overhead is expected to make a small split neutral or slower; a valid but slower multi-GPU path remains optional and is reported honestly.

## Evidence classification

- Functional/ABI tests: `contract_pass`.
- Bonsai-8B timing, VRAM, and power samples on the live dual-R9700 host: `measured_pass` or `measured_fail`.
- Results on other models/topologies: `withheld` until measured.

Systematic components are model bytes, layer count, device list, sampler, context, prompt, and output cap. Irreducible/noisy components are OS scheduling, PCIe contention, page cache, allocator caching, and power-sampling cadence; record them rather than hiding them behind a single ratio.

## Measured acceptance: dual R9700 host

Artifact: `logs/cnet_harness_gpu_benchmark.json`. Three independent processes per case used Bonsai-8B (36 layers), a 151-token prompt, 64 generated tokens, deterministic sampling, two CPU threads, a 512-token context, and a 2 GiB/device policy cap.

| Case | Prompt median | Prompt speedup | Decode median | Decode speedup | Harness VRAM | External peak VRAM | Peak power |
|---|---:|---:|---:|---:|---:|---:|---:|
| CPU | 9333.09 ms | 1.00x | 5002.34 ms | 1.00x | 0 | 57 MiB baseline | 41/42 W idle cards |
| GPU 4/36 | 1545.96 ms | 6.04x | 4477.44 ms | 1.12x | 356 MiB | 554 MiB | 122 W |
| GPU 8/36 | 1388.05 ms | 6.72x | 4050.74 ms | 1.23x | 468 MiB | 666 MiB | 136 W |
| GPU 12/36 | 1178.81 ms | 7.92x | 3497.49 ms | 1.43x | 580 MiB | 778 MiB | 126 W |
| GPU 16/36 | 1004.63 ms | 9.29x | 3010.91 ms | 1.66x | 690 MiB | 888 MiB | 120 W |
| Dual GPU 12/36 | 1211.61 ms | 7.70x | 3514.70 ms | 1.42x | 334 + 412 MiB | 609 MiB/device max | 103 + 60 W |

All 18 measured runs produced exact text equality, remained below 150 W/device, and returned both cards to their pre-run VRAM baseline with a measured zero-byte delta five seconds after close. Real fail-closed probes also rejected 36/36 layers and a four-layer request under a 128 MiB cap, each with zero residual VRAM.

**Recommended policy:** 16/36 layers on one R9700. This is 44.44% of transformer layers, uses under 1 GiB external peak VRAM, raises decode throughput from 12.79 to 21.26 token/s, and raises prompt throughput from 16.18 to 150.30 token/s. It is the fastest measured strictly partial tier while remaining a minor residency/power event.

**Dual-GPU decision:** reject for this model. The equal 12-layer PCIe split was 0.492% slower than one GPU at the same layer count and raised measured peak board power from 126 W to 163 W combined. Keep the API available for future models that exceed one card's useful bounded tier; do not enable it by default.
