# CNET.Llm managed components

This .NET 10 subtree derives from dotLLM by Konrad Kokosa.
Original attribution and the [GPL-3.0-only license](LICENSE) are preserved.
Subsequent CNET changes mean this is no longer a rename-only, byte-identical
upstream import.

For new host integration, start with [CNET.Cce.Llm](../Cce.Llm/README.md).
That bridge references this subtree's Engine, Models, Tokenizers and Cpu;
the older “no project references between halves” statement is obsolete.
Separate build-property files do not eliminate code dependencies.

## Build and bounded tests

From the repository root:

```sh
dotnet build dotnet/Llm/CNET.Llm.slnx -c Release
dotnet test dotnet/Llm/tests/CNET.Llm.Tests.Unit/CNET.Llm.Tests.Unit.csproj -c Release
```

Restore may contact package servers. Full integration tests can download large
missing model fixtures and are not an offline unit-test command.

## Local CPU model

```sh
dotnet run --project dotnet/Llm/src/CNET.Llm.Cli -c Release -- \
  run /absolute/local/model.gguf --device cpu --prompt hello
```

Use an existing compatible file. The sample server also needs a model argument;
read [SERVER.md](docs/SERVER.md) before starting its unauthenticated service.

Vendor `gpu` means CUDA, not ROCm. Supported CNET AMD work follows the
[native guide](../../docs/GPU_TRAINING.md).
[Architecture](docs/ARCHITECTURE.md) distinguishes implementations from
interfaces; [roadmap](docs/ROADMAP.md) records unimplemented scope.
[Benchmarks](docs/BENCHMARKS.md) describes model-download and measurement controls.

Old guides and review findings remain recoverable through
[maintenance](../../docs/MAINTENANCE.md). Historical performance is not current
model parity or a new capsule certification claim.
