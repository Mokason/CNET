# CNET.Cce.Llm — managed inference backend for CCE

Lets `CNET.Cce` run inference through the managed `CNET.Llm` engine instead of
the native `cnet.so` harness, behind one shared interface.

## The seam

`CNET.Cce` defines `ICnetInferenceSession` (in `CnetHarness/ICnetInferenceSession.cs`):

```csharp
public interface ICnetInferenceSession : IDisposable
{
    CnetHarnessGenerationResult Generate(CnetHarnessGenerateOptions options);
    CnetHarnessRouteInfo ProbeRoute(string role, CnetHarnessSamplingMode overrideMode = Auto);
}
```

Two implementations:

| backend | assembly | opens with | needs |
| --- | --- | --- | --- |
| native (default, unchanged) | `CNET.Cce` | `CnetHarnessSession.Open(config)` | `cnet.so` + llama context |
| managed | `CNET.Cce.Llm` | `CnetLlmInferenceSession.Open(config)` | nothing but the GGUF file |

Hosts hold the interface and pick a backend at the open call:

```csharp
ICnetInferenceSession session = useManaged
    ? CnetLlmInferenceSession.Open(config)
    : CnetHarnessSession.Open(config);

var result = session.Generate(new CnetHarnessGenerateOptions
{
    System = "You are terse.",
    User   = "The capital of France is",
    Role   = "probe",
    MaxTokens = 64,
    Sampling  = CnetHarnessSamplingMode.Deterministic,
});
```

Nothing downstream of `Generate` changes: same options type, same result record,
same `CnetHarnessException` status codes.

## Why this is a separate assembly

Two reasons, both load-bearing — do not "simplify" this by adding a
`CNET.Llm` reference directly to `Cce.csproj`:

1. **License containment.** `CNET.Cce` is Apache-2.0; `CNET.Llm` is GPL-3.0-only
   (vendored from [dotLLM](https://github.com/kkokosa/dotLLM)). A direct
   reference would put every CCE consumer under GPL-3.0 on distribution. With
   the bridge separate, only hosts that actually reference `CNET.Cce.Llm` take on
   that obligation. `CNET.Cce` still references nothing from `CNET.Llm`.
2. **AOT.** `Cce.csproj` sets `IsAotCompatible=true`. The LLM engine is not part
   of that guarantee, so this project does not claim it.

The only concession is an `InternalsVisibleTo("CNET.Cce.Llm")` in `Cce.csproj`,
which lets the bridge call `CnetHarnessSession`'s config/option validators. That
is visibility only — no reference in the other direction — and it exists so the
two backends cannot drift apart on what input they accept.

## What the managed backend does not do

Stated plainly, because the result records have fields it cannot fill honestly:

- **No AICIMO routing.** The native backend selects an adapter and reports route
  uncertainty. There is no AICIMO here: `SelectedAdapter` is always `0`,
  `RouteUncertainty` always `0.0`, and `AicimoOverride` always `false`. Those are
  "not measured", not "measured as zero".
- **`Auto` sampling has nothing to defer to.** On the native backend `Auto` hands
  the decision to AICIMO. Here it resolves to `Balanced`, and
  `EffectiveSampling` reports `Balanced` — so callers see what was applied
  rather than what they asked for. Full table in `CnetLlmSamplingProfile`.
- **CPU only.** `Open` *rejects* a GPU `ResourceMask` or any `GpuOffload` with
  `InvalidArgument` rather than silently running on CPU. Use the native session
  for GPU work. (`CNET.Llm.Cuda` exists but is NVIDIA-only and is not wired here.)
- **`Role` does not influence generation.** It is validated for parity with the
  native backend, and ignored.

Prompt construction uses the GGUF's own chat template when the model carries one
(`HasChatTemplate`); otherwise `System` and `User` are concatenated, which is the
honest fallback for a base model with no turn structure.

## Performance vs the native harness

Measured with `dotnet/Cce.Llm.Benchmarks`, both backends opened from one
`CnetHarnessConfig` and driven through `ICnetInferenceSession`. SmolLM-135M
Q8_0, CPU-only, Ryzen 9 9950X, native plugin linked against llama.cpp
`build-cpu`. Median of 5 reps after an untimed warm-up.

| | native (`cnet.so`) | managed (`CNET.Llm`) | native advantage |
| --- | ---: | ---: | ---: |
| decode | 287 tok/s | 85–99 tok/s | **~3x** |
| prefill, cold, 240-token prompt | 4,700–5,800 tok/s | ~166 tok/s | **~29x** |
| model load | ~50 ms | ~90–108 ms | ~2x |

Thread scaling (prefill tok/s, 4/8/16 threads): native 2,849 / 4,702 / 5,848;
managed 166 / 166 / 165. The managed path is essentially flat. This is not a
bridge defect — the `CNET.Llm` CLI scales just as weakly on the same model
(decode 83 / 94 / 117 tok/s at 1 / 8 / 16 threads), and the bridge's
`ThreadingConfig` mapping matches the CLI's.

Two cautions about these numbers:

- **Prefill must be measured with a cold prefix.** Repeating one prompt reports
  native prefill at 32,000–61,000 tok/s; varying only the prompt's *suffix*
  still reports ~40,000. Both are the prefix cache, not prefill throughput. Only
  when the variation moves to the *front* of the prompt does the real figure
  (~4,900 tok/s) appear. The benchmark's 6th argument (`1`) enables front-varied
  prompts — use it for any prefill claim.
- **This is not a port regression.** `CNET.Llm` is byte-for-byte upstream dotLLM
  modulo naming, and benchmarked identical to it (20 of 22 CPU kernels within
  ±1.1%, end-to-end decode within 0.34%). The gap here is dotLLM vs llama.cpp,
  and it predates the port.

Practical read: the managed backend is a **portability and deployment fallback**,
not a performance substitute. It costs roughly 3x on decode and ~29x on prefill,
so long-prompt workloads suffer far more than long-generation ones. Reach for it
when the native plugin cannot be built or loaded — not to make CCE faster.

```bash
make cnet_harness_plugin LLAMA_CPP_BUILD=/home/marble/llama.cpp/build-cpu
CNET_HARNESS_LIBRARY=$PWD/bin/libcnet_harness.so \
  dotnet run -c Release --project dotnet/Cce.Llm.Benchmarks -- <model.gguf> 5 64 8 12 1
#                                     model, reps, maxTokens, threads, promptRepeat, uniquePrompts
```

## Tests

`dotnet test dotnet/Cce.Llm.Tests` — 19 tests. The generation tests need a local
GGUF fixture and skip cleanly without one; they reuse the model the `CNET.Llm`
integration suite downloads to `~/.cnet-llm/test-cache/` rather than fetching
another copy.
