# Bounded GPU Offload Execution

Source contract: `docs/cnet_bounded_gpu_offload.md`

| Slice | RED evidence | GREEN authority | Status |
|---|---|---|---|
| Additive native ABI | new offload policy/open/introspection symbols absent | hermetic contract test preserves legacy ABI and proves fail-closed policy validation/copy | PASS |
| Managed projection | no per-session layer/device/cap option | fake-native tests prove exact additive call selection, projection, validation, and introspection | PASS |
| Single-R9700 path | harness smoke is CPU-only | identical deterministic output, strict partial-layer proof, nonzero bounded VRAM, lower generation latency, cleanup | PASS: 16/36, 1.66x decode |
| Dual-R9700 option | no explicit filtered device list or split | equal layer split is correctness-valid; recommendation requires measured gain over one GPU | VALID, NOT RECOMMENDED |
| Closure | no GPU umbrella | one clean managed/native + real ROCm acceptance with process/VRAM sweep | PASS |

## Hypotheses

- **H0:** bounded partial ROCm offload provides no useful generation advantage within a minor residency envelope.
- **H1:** at least one strictly partial single-GPU layer count produces deterministic-equivalent output and lower generation latency while remaining below the selected VRAM cap; all resources return on close.

Dual-GPU is a separate measured decision. A valid but slower PCIe layer split does not reject H1 and will not be recommended.
