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
quantization should.

### Fix applied: dequantize weight tiles to f32 for prefill

`MatMul.GemmQ8_0` now expands each weight tile to f32 **once** and runs the f32
GEMV across all tokens, whenever n >= `DequantF32TokenThreshold` (16). Below that
the quantized kernel is kept — it wins by ~2x at n=1, where a per-tile
dequantization has nothing to amortize against.

| N | Q8_0 GEMM before | after | speedup |
| ---: | ---: | ---: | ---: |
| 1 | 733 us | 722 us | unchanged (below threshold) |
| 4 | 2,891 us | 2,780 us | unchanged (below threshold) |
| 16 | 10,983 us | 8,071 us | **1.36x** |
| 64 | 43,174 us | 30,561 us | **1.41x** |
| 256 | 171,704 us | 118,871 us | **1.44x** |
| 512 | 343,492 us | 237,354 us | **1.45x** |

End-to-end, like-for-like (Balanced, 64 generated tokens both builds, 8 threads):

| | before | after |
| --- | ---: | ---: |
| prefill | 499 tok/s | **1,286 tok/s (2.58x)** |
| decode | 220 tok/s | 235 tok/s (unchanged — decode is n=1) |

That closes the prefill gap to the native harness from ~9.2x to ~3.7x. Decode was
never expected to move and did not.

**This changes numerics for prompts of 16+ tokens**, though not — as far as
measured — the tokens that come out. The quantized path rounds activations
through Q8_0 before the dot; the f32 path does not, so it is strictly *closer* to
exact `dequant(W) · x`. `MatMulDequantF32Tests` asserts that direction against a
double-precision reference, rather than merely that the two paths are near each
other.

Observed effect on generation, via the golden set in
`CNET.Llm.Tests.Integration/Goldens/prefill_greedy.tsv` (six prompts from 5 to 47
tokens, 24 greedy tokens each):

- **Greedy output is identical on every case**, on both sides of the threshold,
  with the dequant path enabled or disabled. The logit shift is well below what
  it takes to change an argmax selection here. It can still diverge where the
  top-two logits are near-tied, so this is "no observed change", not a guarantee.
- All 85 integration tests, including the Llama/Qwen/Q4_K/Bielik forward passes,
  pass against their references.

`DequantF32TokenThreshold` set to `int.MaxValue` restores the previous numerics
exactly, if a certification path needs bit-identical arithmetic rather than
merely identical output.

### Rejected: token-blocked GEMM (tried, reverted, do not repeat)

The intuitive fix — block the token axis so each loaded weight block serves
several tokens — was implemented for AVX2 and AVX-512, fully tested, and
**delivered nothing**: 343,492 -> 342,799 us at N=512 (0.2%, against 0.3%
StdDev), and no end-to-end gain. Weight reloading was never the bottleneck. The
quantized inner loop spends ~6 vector ops per 32 MACs (sign, maddubs, madd,
cvtdq2ps, scale broadcast, fma) = 0.1875 ops/MAC against f32's 1 op per 8 MACs =
0.125, predicting a 1.50x gap where 1.63x was measured — it is
instruction-throughput-bound. Hoisting one load and one sign per four tokens is
~8% of the op budget at best. That analysis is what pointed at dequantization
instead, since the f32 loop simply needs fewer ops per MAC.

### Also tried: VNNI (`vpdpbusd`) — kept, but it buys ~nothing

The Q8_0 kernels now fuse their `maddubs` + `madd(ones)` pair into a single
`VPDPBUSD` via `MatMul.DotBytesToInt32` when `AvxVnni` is available (.NET exposes
VNNI only through `AvxVnni`; there is no `Avx512Vnni` class, and every call site
here is 256-bit anyway). It is bit-identical, not merely close: `maddubs`
saturates its int16 pair sums where `VPDPBUSD` does not, but Q8_0 operands cap a
pair sum at 128*127*2 = 32512, inside int16. `MatMulVnniTests` pins that, and
the generation goldens are byte-unchanged.

Measured with a controlled same-binary A/B (`DOTNET_EnableAVXVNNI=0/1`):

| | VNNI on | VNNI off |
| --- | ---: | ---: |
| decode, end-to-end (median of 3) | 263 tok/s | 270 tok/s |
| `GemvQ8_0` micro, K=4096 | 10,821 ns | 10,899 ns |
| `GemvQ8_0` micro, K=11008 | 29,292 ns | 29,553 ns |

Around 1%, in both directions — noise. The earlier ~15% estimate in this file was
wrong, and it was wrong the same way the token-blocking estimate was: it assumed
the kernel is limited by the count of vector ops. It is not.

`VecDotQ8_0_Avx2` at K=4096 runs 128 blocks in 253 ns = **7.9 cycles/block** on
L1-resident data (4,352 bytes, 17 GB/s — nowhere near memory). Five vector ops
would issue in ~2 cycles on this core, so the kernel is neither issue-limited nor
bandwidth-limited: it is **latency-limited on the serial FMA accumulator chain**
across blocks. Removing one op off that chain's side cannot lower a floor set by
FMA latency. End-to-end decode has the opposite problem — 143 MB of weights per
token at 265 tok/s is ~38 GB/s, near the DRAM roof, so it is bandwidth-bound.

The change is kept because it is bit-identical, never slower, and a net
simplification (21 two-line idioms collapse into one documented helper with an
explicit saturation-safety argument). It is not kept because it made anything
faster.

The lead it points at, unexplored: give the single-row Q8_0 dot **multiple
independent accumulators** so the FMA chain is not serial — the 4-row kernels
already have four and are correspondingly less latency-bound. Separately, the
dequant path still carries ~11% overhead versus a pure f32 GEMM (237,354 us vs
209,890 us at N=512) from writing the dequantized tiles.

A latent detail surfaced while testing the reverted kernel, recorded because it
is easy to trip over: the legacy per-token kernels take `abs` of the *activation*
side and compute `sign(w, x)`, which for `w = -128` and `x < 0` needs +128 in a
signed byte and wraps to -128, flipping that term's sign. `QuantizeF32ToQ8_0`
scales by `maxAbs / 127`, so -128 is not reachable from this quantizer and the
bug is currently unobservable — but a hand-built or third-party Q8_0 block
containing -128 would hit it.

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
