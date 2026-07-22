# CNET.Llm

Managed (C#/.NET 10) transformer inference stack living inside the CNET tree:
GGUF/SafeTensors loading, SIMD CPU kernels, CUDA PTX kernels, KV-cache, scheduler,
samplers, constrained decoding, speculative decoding, an OpenAI-compatible HTTP
server, and a CLI.

## Provenance

This subtree is vendored from **[dotLLM](https://github.com/kkokosa/dotLLM)** by
Konrad Kokosa, licensed **GPL-3.0-only** (see `LICENSE`). Upstream authorship and
license are preserved verbatim.

The port is **rename-only**. Every one of the 504 vendored files is byte-identical
to the upstream file after this mechanical substitution:

| upstream        | CNET               | applies to                                 |
| --------------- | ------------------ | ------------------------------------------ |
| `DotLLM.`       | `CNET.Llm.`        | namespaces, project + assembly names, paths |
| `DotLLM`        | `CnetLlm`          | bare identifiers (`MapCnetLlmEndpoints`, `AddCnetLlm`) |
| `DOTLLM_`       | `CNET_LLM_`        | environment variables                       |
| `.dotllm`       | `.cnet-llm`        | model cache dir (`~/.cnet-llm/models/`)     |
| `dotllm`        | `cnet-llm`         | CLI command, localStorage keys, AOT assembly name |
| `dotLLM`        | `CNET LLM`         | prose in docs / UI / comments               |

No algorithm, kernel, allocation strategy, or build flag was touched, so the
emitted IL — and therefore throughput — matches upstream. Three build files
intentionally differ, none of them affecting codegen:

- `Directory.Build.props` — MinVer (git-tag versioning) replaced with a pinned
  `0.1.0`, since CNET's tags are not dotLLM release tags; CNET product metadata.
- `src/Directory.Build.props` — MinVer `PackageReference` dropped.
- `Directory.Packages.props` — MinVer `PackageVersion` dropped.

`CNET.Llm.slnx` replaces upstream's `dotLLM.slnx`.

## Isolation from the rest of CNET

This subtree is self-contained and has **no project references to or from** the
pre-existing CNET managed projects (`dotnet/Cce`, `dotnet/CceHost`,
`dotnet/Cce.Tests`, `dotnet/CnetHarnessSmoke`, `dotnet/CnetMcpServer`).

`Directory.Build.props` and `Directory.Packages.props` sit at `dotnet/Llm/`, so
MSBuild's upward property-file walk from the older projects (which live one level
up at `dotnet/`) never reaches them. Central package management, GPL package
metadata, `TreatWarningsAsErrors`, and trim analysis apply to `CNET.Llm.*` only.
Nothing in the CCE build was modified.

The two halves are wired together only by `dotnet/CNET.slnx`, which builds both
in one command without creating a code dependency.

## Build & test

```bash
dotnet build dotnet/Llm/CNET.Llm.slnx -c Release     # this subtree
dotnet test  dotnet/Llm/CNET.Llm.slnx -c Release
dotnet build dotnet/CNET.slnx -c Release             # CCE + Llm together
```

## Run

```bash
# CLI
dotnet run --project dotnet/Llm/src/CNET.Llm.Cli -c Release -- --help
dotnet run --project dotnet/Llm/src/CNET.Llm.Cli -c Release -- run <model.gguf> -p "hello"

# OpenAI-compatible server + chat UI
dotnet run --project dotnet/Llm/samples/CNET.Llm.Sample.Server -c Release
```

Models resolve from `~/.cnet-llm/models/`, overridable with `CNET_LLM_MODELS_DIR`.
Benchmarks read `CNET_LLM_BENCH_MODEL_PATH`, `CNET_LLM_BENCH_DEVICE`,
`CNET_LLM_BENCH_PROMPT`, `CNET_LLM_BENCH_MAX_TOKENS`.

## Using it from CNET code

Add a reference to the piece you need — the packages are layered, so the CPU
inference path does not drag in the server or CUDA:

```xml
<ProjectReference Include="..\Llm\src\CNET.Llm.Engine\CNET.Llm.Engine.csproj" />
<ProjectReference Include="..\Llm\src\CNET.Llm.Cpu\CNET.Llm.Cpu.csproj" />
```

Note the license asymmetry before doing so: `CNET.Cce` is Apache-2.0 and this
subtree is GPL-3.0-only. Linking a CCE assembly against `CNET.Llm.*` makes the
combined work GPL-3.0 on distribution. That is why no such reference exists
today — the seam is left explicit rather than made implicitly.

## Layout

```
Llm/
├── src/
│   ├── CNET.Llm.Core/          # tensors, backends, model config, sampling interfaces
│   ├── CNET.Llm.Models/        # GGUF / SafeTensors loaders, transformer architectures, LoRA
│   ├── CNET.Llm.Tokenizers/    # BPE, SentencePiece, HF tokenizer.json, chat templates
│   ├── CNET.Llm.Cpu/           # SIMD kernels (TensorPrimitives + intrinsics)
│   ├── CNET.Llm.Cuda/          # CUDA driver-API backend, PTX kernels
│   ├── CNET.Llm.Engine/        # KV-cache, scheduler, samplers, constraints, speculative
│   ├── CNET.Llm.Diagnostics/   # inference hooks, activation capture, logit lens
│   ├── CNET.Llm.Telemetry/     # metrics, tracing
│   ├── CNET.Llm.HuggingFace/   # hub search + GGUF download
│   ├── CNET.Llm.Server/        # OpenAI-compatible API + chat UI
│   └── CNET.Llm.Cli/           # `cnet-llm` command
├── native/                     # .cu kernel sources + pre-compiled .ptx
├── tests/, benchmarks/, samples/, docs/, scripts/
```

`docs/` carries the upstream design documents (architecture, attention, KV-cache,
quantization, sampling, scheduling, tokenizers, …) with the same renaming applied.
