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
`build-cpu`, front-varied prompts (~240 tokens), median of 5 reps.

| threads | decode: native / managed | prefill: native / managed |
| ---: | ---: | ---: |
| 4 | 267 / 223 tok/s | 2,811 / 383 tok/s |
| 8 | 290 / 223 tok/s | 4,956 / 536 tok/s |
| 16 | 283 / 256 tok/s | 5,528 / 550 tok/s |

Model load: native ~57 ms, managed ~110 ms.

**Decode is near parity (~1.1-1.3x). Prefill is not (~9-10x).** That asymmetry is
the whole story, and it is worth understanding before choosing a backend.

### Two measurement traps, both of which caught earlier revisions of this file

1. **Threading must be passed at model load.** `TransformerModel.LoadFromGguf`
   builds its `ComputeThreadPool` from the `threading` argument of the *three*-arg
   overload. The two-arg overload silently defaults to
   `ThreadingConfig.SingleThreaded`, and setting `InferenceOptions.Threading`
   afterwards does **not** create a pool. This bridge originally called the
   two-arg overload, so every matmul ran on one thread — understating managed
   decode by 2.6x (85-99 -> 223-256 tok/s) and prefill by 3.3x. If you write
   another host against `CNET.Llm`, check this first.
2. **Prefill must be measured against a cold prefix.** Repeating one prompt
   reports native prefill at 32,000-61,000 tok/s; varying only the prompt's
   *suffix* still reports ~40,000. Both are the prefix cache. The real figure
   (~5,000) appears only when the variation moves to the *front*. The
   benchmark's 6th argument enables front-varied prompts.

### Where the remaining prefill gap comes from

Profiled with the upstream `GemmBenchmarks` (M=K=4096, single-threaded, so this
isolates the kernel from threading). Ratio is GEMM over N sequential GEMVs —
lower is a better batching win:

| N | Q8_0 GEMM / seq GEMV | F32 GEMM / seq GEMV |
| ---: | ---: | ---: |
| 4 | 0.78 | 0.45 |
| 16 | 0.95 | 0.30 |
| 64 | 0.94 | 0.27 |
| 256 | 0.94 | 0.27 |
| 512 | 0.95 | **0.26** |

**`GemmQ8_0` gets no batching win.** At N=512 it is 5% faster than doing 512
independent GEMVs; the F32 GEMM on the same shapes gets 3.8x. Since prefill runs
the quantized path (the model is Q8_0), prefill costs essentially the same per
token as decode — which is exactly what the end-to-end numbers show: managed
prefill is only ~2.1x its own decode rate, where the native harness gets ~19.5x.

The cause is not the tiling structure. `ComputeGemmTiled` and `GemmF32` have the
*same* shape — tile over M, loop tokens inside, calling a GEMV per token — and
both size their tile to a 256 KB L2 budget. The difference is what each kernel is
limited by. Effective bandwidth at N=1: F32 GEMV moves 64 MiB in 1,491 us =
**45.0 GB/s**; Q8_0 GEMV moves 17 MiB in 719 us = **24.8 GB/s**. The F32 kernel is
near the memory roof, so keeping a weight tile hot in L2 pays enormously. The
Q8_0 kernel is *not* bandwidth-limited — it is compute-limited on dequantize plus
int8 dot (per-row GEMV time ~168 ns matches `VecDotQ8_0_Avx2` almost exactly). No
amount of cache tiling helps a compute-bound loop; the dequantized weight block
has to be reused across several tokens in registers, which neither kernel does.

A consequence worth flagging: **beyond N=4, `GemmF32` beats `GemmQ8_0` outright**
— 1.64x at N=512 — despite F32 weights being 4x larger. At N=1 Q8_0 wins 2.1x, as
quantization should. So for prefill specifically, dequantizing a weight tile once
and running the F32 GEMM would be faster than the current quantized path. That is
the single highest-value fix, and it is upstream in `CNET.Llm.Cpu`, not in this
bridge.

Note this is not a port regression: `CNET.Llm` benchmarked identical to upstream
dotLLM (20 of 22 CPU kernels within +/-1.1%, end-to-end decode within 0.34%). The
gap is dotLLM vs llama.cpp and predates the vendoring.

Practical read: with threading configured correctly the managed backend is a
reasonable decode-side substitute, but it still pays ~9x on prefill. Long-prompt,
short-answer workloads (classification, routing, extraction) suffer most;
short-prompt, long-generation workloads are close to parity.

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
