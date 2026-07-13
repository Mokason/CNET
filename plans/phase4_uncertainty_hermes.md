# Phase 4: Uncertainty-Aware Activation + Hermes Integration

## Scope Completed

- Added `cce_specialist_get_uncertainty()` and `cce_specialist_get_uncertainty_ex()`.
- Added activation entropy, route entropy, route variance, and combined uncertainty reporting.
- Added `cce_compression_grads` as a reusable `Grads[]` buffer.
- Attached optional compression gradients to `cce_supra_decomposed`.
- Added `cce_supra_enable_gradient_accumulation()`, `cce_supra_accumulate_compression_gradient()`, and `cce_supra_clear_gradient_accumulation()`.
- Added MCP tool `cnet_compress_model(model_path, target_size, options)`.
- Added Hermes wrapper manifest generation under `hermes_wrappers/`.
- Documented deployment in `docs/hermes_hosting.md`.

## Verification

Commands run:

```sh
make phase4_uncertainty_test
dotnet build dotnet/CnetMcpServer/CnetMcpServer.csproj
make cce_safetensors_test
```

Results:

- `make phase4_uncertainty_test`: passed.
- `dotnet build`: passed with the existing `NU1603` package warning.
- `make cce_safetensors_test`: compiled aggregate CCE and the log ends with `ALL SAFETENSORS TESTS PASSED`.

## Remaining Work

- Wire the Hermes wrapper to an actual Hermes launch script once the target runtime contract is fixed.
- Run compression recovery on a real converted model and inspect `Grads[]` norms.
- Calibrate uncertainty thresholds on real specialist activations and route evidence.
