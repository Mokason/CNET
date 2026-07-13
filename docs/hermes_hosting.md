# Hermes Hosting for CNET-Compressed Models

## Status

Phase 4 adds the wrapper and metadata needed to hand CNET-compressed artifacts to Hermes. The wrapper is intentionally explicit: it records the source model, compression strategy, uncertainty method, and gradient-preservation policy before native compression is run.

## MCP Tool

Tool: `cnet_compress_model(model_path, target_size, options)`

Example arguments:

```json
{
  "model_path": "/models/source-model",
  "target_size": "1.6bit",
  "options": "sparse_kv narrative_contracts"
}
```

The tool writes a manifest under:

```text
hermes_wrappers/<model>.hermes.json
```

The manifest includes:

- source model path and existence check
- selected compression strategy (`quantize_int8`, `quantize_ternary`, or `pack_trits_1p6bit`)
- output artifact path
- uncertainty policy (`activation_entropy_plus_counterfactual_route_entropy`)
- `Grads[]` preservation policy for identity and residual paths
- suggested Hermes start command

## Native Compression Path

For Supra/safetensors paths, the decomposed model now carries:

```c
cce_compression_grads compression_grads;
```

and exposes:

```c
cce_supra_enable_gradient_accumulation(...);
cce_supra_accumulate_compression_gradient(...);
cce_supra_clear_gradient_accumulation(...);
```

Use these before or during compression-aware recovery passes so quantization and packing do not erase identity/residual backward signals.

## Deployment Steps

1. Generate the wrapper manifest with `cnet_compress_model`.
2. Run the matching native compression path (`int8`, ternary, or packed trits).
3. Verify the model with uncertainty and narrative/counterfactual checks.
4. Place the resulting `.cnetpack` artifact at the manifest `output_artifact` path.
5. Start Hermes with the manifest's `hermes.start_command`.

## Limits

The MCP tool currently prepares the Hermes wrapper manifest and compression plan. It does not perform heavyweight model conversion by itself; that remains a native/offline compression step because model paths, GPU availability, and target format differ per deployment.
