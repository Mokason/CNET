# Hermes Hosting for CNET-Compressed Models

## Status

Phase 4 supplies compression metadata, but a file path is not a Hermes model identifier and Hermes does not load `.cnetpack` directly. Deployment is therefore admission-gated: `tools/render_hermes_wrapper.py` selects only an admitted candidate or the reference fallback from a real-model acceptance report, and `tools/run_hermes_wrapper.py` starts that GGUF through CPU-only `llama-server` before routing an isolated Hermes custom-provider session to it.

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
- legacy launch intent (not deployment authority; the acceptance-selected wrapper supplies the executable command)

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

1. Run `tools/run_real_model_acceptance.py` against a reference and candidate.
2. Require campaign `overall_pass=true`; a rejected candidate remains quarantined.
3. Render the selected wrapper:

```sh
python3 tools/render_hermes_wrapper.py \
  --report reports/qwythos_real_model_acceptance.json \
  --server /path/to/cpu/llama-server \
  --output hermes_wrappers/model.selected.hermes.json
```

4. Launch and probe the real Hermes path:

```sh
python3 tools/run_hermes_wrapper.py \
  --manifest hermes_wrappers/model.selected.hermes.json
```

The launcher verifies GGUF magic and SHA-256, binds only to loopback, clears CUDA/HIP/ROCR visibility, fixes `--n-gpu-layers 0`, disables model-side thinking for the exact probe, creates an ephemeral Hermes home, caps output at 64 tokens, and removes the server after the probe. It does not alter or restart the active Hermes gateway/profile.

## Limits

The legacy Phase 4 `hermes --model <artifact-path>` suggestion is not executable: Hermes expects a catalog model name, not a GGUF path. Native conversion remains an offline step. The selected wrapper currently requires at least a 65,536-token llama.cpp context for Hermes' core prompt and permits a bounded 180–600 second CPU query timeout.

## See also

- Doc hub: [`INDEX.md`](INDEX.md)
- Use-loop: `make cnet_use_loop_acceptance`


## AICIMO / RouteOnRole honesty

MCP `RouteOnRole` may use a deterministic role-hash fallback when the native
Drole slice is unavailable. Clients must not treat the wrapper name as proof of
neural role routing — check the returned mechanism field when present.
