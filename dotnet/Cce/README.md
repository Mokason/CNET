# CNET.Cce managed bindings

This .NET 10 assembly wraps selected native CNET APIs. Its
[project](Cce.csproj) carries Apache-2.0 metadata; see the root
[license](../../LICENSE). The separate LLM bridge has different metadata.

## Build and test

From the repository root:

```sh
make cnet_dll
dotnet build dotnet/Cce/Cce.csproj -c Release
dotnet test dotnet/Cce.Tests/Cce.Tests.csproj -c Release
```

Restore may need network access. [CceNative.cs](CceNative.cs) binds the
unified library name `cnet`; tests copy `cnet.so` / `cnet.dll`.
The old `make cce_dll` prerequisite alone is insufficient. Keep the selected
native library discoverable through deliberate loader configuration.

## Compiled API and ownership

The library includes tensor/model/training helpers, router/perceptual bindings,
specialist/forest helpers and [CnetHarness](CnetHarness/) abstractions.
Dispose native-handle wrappers according to each type's contract.

`CceContract.cs`, `CceRegistry.cs` and `CceSupra.cs` are explicitly
excluded because their raw layouts/unexported symbols are unsafe bindings.
Their old examples are not a current usable API. `Examples/` is excluded too.

`CceModel.TryUseGpu()` attempts legacy CUDA; `SetDevice(OpenCl)` throws.
Neither exposes CNET's supported AMD training path. Use the
[native GPU guide](../../docs/GPU_TRAINING.md), the separate
[harness plugin](../../docs/cnet_dotnet_inference_harness.md), or
[CNET.Cce.Llm](../Cce.Llm/README.md) for managed CPU inference.

Benchmarks also need the unified library even if their project copies only
the legacy CCE artifact. P/Invoke success is not capsule certification.
Original tutorials and measurements remain in the
[archive](../../docs/MAINTENANCE.md).
