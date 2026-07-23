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

### Multiple accumulators: tried, 2-3%, reverted

Splitting the single-row Q8_0 dot across four independent accumulators, to break
the serial block-to-block FMA chain, measured 268.3 -> 261.2 ns at K=4096 and
707.4 -> 692.2 ns at K=11008. Two to three percent, and it changes float
summation order (addition is not associative), for a kernel that only handles
`m % 4` row tails. Not worth a numerics change; reverted.

### What the bottleneck actually is

Three optimizations in a row under-delivered — token blocking (0.2%), VNNI (1%),
multiple accumulators (2-3%) — so the kernel was finally decomposed rather than
reasoned about. `Q8DotAnatomyBenchmarks` strips one layer at a time off
`VecDotQ8_0Avx2` at K=4096:

| variant | time | delta |
| --- | ---: | --- |
| LoadsOnly | 32.9 ns | — |
| DotOnly (+ dpbusd) | 34.9 ns | +2 ns |
| IntAccum (+ sign normalisation) | 48.8 ns | +14 ns |
| ConstScale (+ cvt, fma) | 72.2 ns | +23 ns |
| **Full** (+ per-block fp16 scales) | **263.4 ns** | **+191 ns** |

**The per-block fp16 scale handling is ~72% of the kernel** — two `Half`->`float`
conversions, a scalar multiply and a broadcast. Everything the three failed
attempts touched lives inside the other 28%. That single fact explains all three
results, and is why they should not be retried.

Measured targets for fixing the actual cost:

| | time | vs Full |
| --- | ---: | ---: |
| activation scales precomputed to f32 | 146.8 ns | 1.79x |
| both scale sets precomputed to f32 | 93.3 ns | 2.82x |

With one caveat that matters for how much of this is available: the decode path
runs the **4-row** kernels, which already hoist the activation scale out of the
row loop — one activation conversion plus four weight conversions per block, so
1.25 conversions per row-block against the single-row kernel's 2. The cheap half
of the fix is therefore mostly already taken on the path that matters, and the
remaining cost is the *weight* scales.

### Weight scale plane: built, opt-in, off by default

The layout change was implemented: `WeightRepacking.RepackR4(..., buildScalePlane: true)`
builds an f32 plane mirroring the R4 interleave, and the decode kernel reads it
instead of converting an fp16 scale per block. It is bit-identical (13 tests
assert exact equality, and the generation goldens are byte-unchanged), and in
isolation it is a large win:

| | fp16 per block | scale plane | speedup |
| --- | ---: | ---: | ---: |
| R4 kernel, K=4096, M=256 (cache-resident) | 39.6 us | 22.9 us | **1.73x** |
| R4 kernel, K=4096, M=16384 (71 MB) | 2,780 us | 1,883 us | **1.48x** |

**It still makes real decode slower, so it defaults off.** End to end on
Llama-3.2-1B Q8_0, five runs per arm with no overlap between them:

| | decode |
| --- | ---: |
| plane off | **30.72 tok/s** |
| plane on | 29.08 tok/s (**-5.3%**) |

Decode is memory-bound — 1.3 GB of weights at ~30 tok/s is already ~40 GB/s —
and the plane adds 11.8% to the bytes streamed per token. The compute it saves
offsets roughly half of that, and the rest is a net loss.

The microbenchmarks disagree because they hammer one tensor in a loop and get
cache reuse. Decode touches every weight byte exactly once per token and gets
none. That gap is the single most useful thing on this page: a kernel
microbenchmark cannot tell you whether a decode optimization will work, because
it cannot reproduce decode's access pattern.

Turn it on only where weights are genuinely cache-resident. Note also that
`InterleavedMinRowBytes = 1024` means models with small hidden sizes never reach
this path at all — SmolLM-135M's projections are 612 byte rows, so none of this
applies to them.

### Vectorized fp16 convert: kept — 1.64x kernel, 0% end to end

`(float)Half` is not a cheap hardware conversion on .NET 10. Measured at the
kernel's own stride (one fp16 per 34-byte block), 128 conversions cost 86.5 ns
against 27.8 ns for a hand-written sequence — roughly 3x. `MatMul.HalfToFloat`
replaces it at all 34 Q8_0 scale-read sites, adding **zero bytes**.

Correctness is not assumed: it handles Inf/NaN and subnormals explicitly, and
`MatMulHalfConvertTests` checks all **65,536** fp16 bit patterns bitwise against
`(float)Half`. Subnormals are the range a naive shift-and-rebias gets wrong, and
a broken fp16 subnormal decode has already cost this project a debugging cycle
once.

| | before | after |
| --- | ---: | ---: |
| `VecDotQ8_0Avx2`, K=4096 | 263.4 ns | **160.8 ns (1.64x)** |
| Llama-3.2-1B decode, end to end | 30.72 tok/s | 30.58 tok/s |
| SmolLM-135M decode, end to end | ~270 tok/s | ~271 tok/s |

Kept because it is free: bit-identical, no added bytes, no regression, and a
strictly faster kernel that pays off on any workload not pinned at the memory
roof. It just does not help the one measured here.

### The wall, after five attempts

Token blocking 0.2%, VNNI 1%, multiple accumulators 2-3%, weight scale plane
-5.3%, vectorized fp16 convert 0%. Four of the five made the kernel genuinely
faster in isolation — the last by 1.64x — and none moved end-to-end decode.

The reason is one number. Decode throughput on this machine:

| model | weights | decode | effective bandwidth |
| --- | ---: | ---: | ---: |
| SmolLM-135M Q8_0 | 0.145 GB | 265 tok/s | **38.4 GB/s** |
| Llama-3.2-1B Q8_0 | 1.32 GB | 30.6 tok/s | **40.4 GB/s** |

Two models an order of magnitude apart in size land within 5% of the same
bandwidth. Decode streams every weight byte exactly once per token and is pinned
at the DRAM roof; the arithmetic between those loads is not what anyone is
waiting for. That is why compute optimizations keep winning microbenchmarks and
vanishing in practice, and why the scale plane — which bought compute by *adding*
bytes — actively regressed.

Anything that helps from here has to move fewer bytes, not do less work per byte:

1. **Lower-precision weights.** Q4_K instead of Q8_0 roughly halves the stream.
   The big one, and a model-quality tradeoff rather than a kernel change.
2. **Keep weights off DRAM.** GPU residency, or a model small enough to sit in
   L3 — the 71 MB microbenchmark still showed kernel wins because it partly fit.
3. **Move less per token.** MoE sparsity, or speculative decoding, which
   amortises one weight pass over several accepted tokens. `CNET.Llm.Engine`
   already has the speculative path.

Further Q8_0 dot-product micro-optimization is not on that list, and this file
now carries five measurements saying so.

### Speculative decoding: correct, and a 2.6-3.9x loss here

`CNET.Llm.Engine` implements greedy speculative decoding, and it works — output
is byte-identical to non-speculative greedy (same sha256), so the verification
and rollback logic is sound. It is also a large loss with the models available:

| | decode | acceptance |
| --- | ---: | ---: |
| baseline | **~170 tok/s** | — |
| K=1 | 66 tok/s | 1.00 |
| K=2 | 52 tok/s | 0.51 |
| K=3 | 44 tok/s | 0.35 |

Decode time goes 537 ms to 1,754 ms at K=2.

This is a model-availability problem, not an implementation one. Speculation wins
only when `draft/target < (n̄ - 1) / K`, where n̄ is expected accepted tokens per
step. At the acceptance rates measured that means the draft must be under 0.39x
the target at K=2, or 0.17x at K=3. The only same-vocabulary pair on this machine
is SmolLM-135M Q4_K_M drafting for Q8_0 — **0.73x**. Structurally impossible.

Two compounding problems:

- **The draft is barely smaller than the target.** Speculation assumes roughly a
  10x gap. Using a lower quantization of the same model gets 0.73x, so K draft
  passes cost more than the target pass they were meant to save.
- **Acceptance collapses with K** (1.00 / 0.51 / 0.35). A Q4 version of a model
  diverges from its Q8 self quickly, so deeper speculation drafts tokens that are
  mostly thrown away — and each rejected token still cost a full weight pass.

Even K=1, where the byte accounting predicts ~1.15x, measured 0.39x. The extra
~2.5x is the two models being resident at once: 239 MB of combined weights
competing for the same memory bandwidth the target is already saturating, plus
two KV caches. In a bandwidth-bound regime a second model is not free the way it
is in a compute-bound one.

**What would actually work is draft-free speculation.** Prompt-lookup / n-gram
decoding proposes continuations by matching the recent suffix against text
already in the context — no second model, so **zero extra bytes streamed**. In a
regime pinned at the DRAM roof that is the only variant whose accounting works
out: cost per step stays one target pass, and any acceptance above zero is a
straight win. It is not implemented here — `ISpeculativeDecoder` takes an
`IModel` draft — and it is the single highest-value thing left on this list,
especially for the repetitive, structured outputs CCE tends to request.

Failing that, a purpose-built draft sharing the target's vocabulary at ~0.1x its
size would satisfy the inequality above with room to spare.

### Prompt-lookup decoding: wired, measured, opt-in

`PromptLookupDrafter` + `PromptLookupDecoder`, wired into `TextGenerator.Generate`
via the optional `promptLookupDrafter` constructor argument. Draft-free: candidates
come from n-gram matches against the context, so no second model is streamed. On a
miss it falls through to a normal single-token step.

Measured on SmolLM-135M Q8_0, 48 greedy tokens, best config per case:

| context | baseline | prompt-lookup | |
| --- | ---: | ---: | --- |
| repetitive | 225 ms | **138 ms (1.63x)** | identical output |
| structured (JSON) | 211 ms | 229 ms (0.92x) | **diverges** |
| prose | 196 ms | 195 ms (1.00x) | identical output |

**It is off by default and should stay off unless the workload is genuinely
repetitive.** Two reasons.

First, it only wins where the output repeats context. Elsewhere it ranges from
break-even to 0.63x. The loss is not the n-gram scan — it is that a partial hit
still pays a `(K+1)`-position batched forward to accept one or two tokens, and the
batched path does not get the decode-specific kernels (fused QKV, R4 interleave)
that a single-token step does. Over-proposing is therefore *not* free, contrary to
the assumption the drafter was first written under.

Second, and more important:

### Batched and sequential forwards disagree by ~0.5 in logit space

`BatchedVsSequentialForwardTests` feeds the same tokens at the same positions two
ways — one at a time, and in a single batched call — and compares logits. They
differ by up to **0.5**. That is not rounding; they are different
implementations (`seqLen == 1` takes fused RMSNorm+quantize and the R4
kernels, `seqLen > 1` takes unfused norm and the tiled GEMM path).

The consequence is structural: **every** speculative decoder verifies with a
batched forward while the baseline it is compared against was produced
token-at-a-time. On a near-tie the two pick different argmax and speculation
stops being output-identical to plain greedy. Measured here on JSON-ish text,
which produces near-tied logits over plausible keys, output diverges around token
29-45 in five of six drafter configurations. Free-form prose and highly
repetitive text did not diverge.

This is pre-existing and applies equally to the model-based `SpeculativeDecoder`,
which carries the same "preserves the target distribution" claim in its own
docs. The test pins the current magnitude so that both a regression and a fix are
visible; reconciling the two paths would make speculation genuinely
output-identical and is the prerequisite for enabling any of it by default.

## Ghost memory: the persistent blob store

`CNET.Cce.Llm.Memory` gives a session unbounded, cross-session memory while the
live context window stays small and fast. Past conversation is stored as
verbatim blobs in an append-only JSON-lines file; each turn rebuilds a budgeted
prompt from keyword recall plus the most recent turns. The window is not the
memory — the file is.

Both backends provide exact token counting for the budgeter —
`CnetLlmInferenceSession.CountTokens` (managed tokenizer) and
`CnetHarnessSession.CountTokens` (native `cnet_harness_count_tokens` ABI export;
no BOS/EOS, no special-token parsing — the cost of text as a prompt fragment).
The two agree on the same GGUF within BOS policy, and a test pins that.

```csharp
using var session = CnetLlmInferenceSession.Open(config);          // PrefixCache on by default
using var store   = BlobStore.Open("~/.cnet-llm/ghost/memory.jsonl");
var memory = new ConversationMemory(store, session.CountTokens);
var ghost  = new MemorySession(session, memory, session.EffectiveContextTokens);

var r = ghost.Generate(system: "You are terse.", user: "What did we decide about GEMM tiles?");
// r.UsedBlobIds  -> exactly which memories were in the prompt (resolve via store.Get)
// r.PromptSystemText -> the full audited context that was sent
```

### Why this architecture (measured)

- A conversation that accumulates history dies at the model window — measured:
  prompt past 2048 tokens on SmolLM is a hard `InvalidArgument`. With per-turn
  rebuild, each call stays inside the window forever; conversation length is
  unbounded.
- Big windows are not the answer even where the model has them: at 8.6k context
  Llama-1B decode fell 30 → 13 tok/s and prefill took 57 s. Small focused
  contexts are *faster*, not just safer.
- Rebuilding is cheap because both backends skip re-prefilling the longest
  unchanged prompt prefix (managed `PrefixCache`, native
  `cached_prompt_tokens`): measured 604 → 50 ms prefill for a stable ~700-token
  header. Hence the prompt layout: stable system text first, then recalled
  blobs chronologically, recent turns, question last.
- Store overhead at 10,000 blobs (1.9 MB): open+index 50 ms once per session,
  append 8 µs/blob (flushed durable), recall 320–630 µs/query — ~0.1% of one
  turn's prefill. An irrelevant query early-outs at ~0 µs.

### Anti-hallucination, mechanically

The layer cannot make the model truthful, but it makes memory *auditable* and
*precise*:

- **Verbatim or nothing.** Recalled text is quoted exactly as stored — never
  summarized, never rewritten. `NewlinesInText_RoundTripExactly` pins the
  round-trip byte-for-byte.
- **Provenance on every memory.** Blobs render as `[#id | date | role] text`,
  and `MemoryGenerationResult.UsedBlobIds` lists exactly what was injected, so
  a host can show receipts for any "I remember".
- **A relevance gate that prefers silence.** A blob is recalled only when the
  query shares a discriminative keyword (document frequency ≤ 25% of the store,
  df=1 always allowed) or at least two distinct query terms. Generic queries
  recall nothing rather than the least-irrelevant blob;
  `IrrelevantQuery_RecallsNothing` pins it. The prompt header additionally
  instructs the model to say when memory does not cover the answer.
- **Budgets are enforced with the live tokenizer** and the bridge now enforces
  the window exactly like the native harness (`prompt >= n_ctx` →
  `InvalidArgument`; `max_tokens` clamped to the room left — previously
  `ContextTokens` was silently ignored on the managed path).

### Forgetting

`BlobStore.Forget(id)` (TUI: `/forget <id>`) prunes a bad memory — the wrong
answer a model gave, a fact you typo'd. Deletion is an **event, not an
erasure**: a tombstone line (`{"del":id}`) is appended, recall and `Get` stop
serving the blob immediately and durably, index statistics shrink so scoring
does not drift — but the original line stays in the file as auditable history,
and the id is never reused, so receipts from any point in the past remain
unambiguous. Rewriting the file is exactly what this store never does.

### Store durability

Append-only JSONL, flushed per write, never rewritten. A crash costs at most
the final line: the loader skips unparseable lines (`CorruptLinesSkipped`) and
heals a truncated tail so the next append starts on a fresh line. Ids stay
monotonic across reopens. Single-writer is enforced with a sidecar `.lock`
held with `FileShare.None` (the only share mode Unix actually enforces); the
OS releases it automatically if the process dies, so no stale-lock deadlock.

### Review findings, fixed before they shipped far

An adversarial review fan-out (3 finder lenses, 2 verifiers per finding, each
required to refute or reproduce against the live code) confirmed and led to
fixing, in the follow-up commit:

- **Engine (pre-existing, HIGH):** `TextGenerator.ResolveKvCache` reused a
  prefix-cached KV that fit the *prompt* but not prompt+answer
  (`|| MaxLength >= promptLen`), silently truncating answers. Fixed; a
  regression test proves it bites against the old code. The bridge now also
  sizes the engine cache to the session window per call, so retained entries
  are always reusable and truncation is structurally impossible.
- `MemorySession` crashed on `maxTokens >= window` (negative budget, int
  overflow past 2³¹) instead of clamping — now clamps, leaving a 128-token
  prompt reserve.
- Recent-turn off-by-one injected the oldest recent turn twice (memory block +
  recent block).
- Budget under-counting: the `### Recent turns` header and per-blob newlines
  were appended but never costed.
- `BlobStore.Open` read the file *before* taking the writer lock (handoff race
  on ids), leaked the lock if a later step threw (wedging reopen in-process),
  and buffered failed appends that a later success could resurrect — all three
  reordered/fixed; writes are now unbuffered (still 8 µs/blob).
- Oversized-message splitting altered text (collapsed newline runs); the
  splitter now emits exact substrings whose concatenation reproduces the
  message byte-for-byte, with word-boundary fallback for punctuation-free runs.
- Lone UTF-16 surrogates are sanitized at append (UTF-8 cannot carry them), so
  in-session recall equals post-reopen recall; the verbatim guarantee is over
  valid Unicode.

### Keyword analysis

Terms are `[A-Za-z0-9_]+` runs, lowercased, stopwords dropped. Underscores stay
inside terms (`q8_0`, `cnet_harness` are single, highly discriminative keys)
and pure numbers are kept ("the vault code is 7291" must be recallable by
"7291"). Scoring is BM25 (k1=1.2, b=0.75) with a mild recency bonus; ties break
newest-first; results are deterministic.

## Model-directed recall (chain-of-thought over the store)

Gate recall is push-only: it guesses relevance from the user's exact words, and
it misses whenever the question is phrased differently than the fact was stored
("how do i get my laptop connected?" vs a memory that says "wifi password").
The `MemorySession` loop adds the pull direction — the model itself searches:

1. The system prompt teaches a one-line protocol: reply `RECALL: <keywords>`
   instead of answering when a referenced memory is not in context. The prompt
   also states the key epistemic fact — *what is shown is only what
   keyword-matched, not the whole store* — and tells the model to search with
   the words the original conversation would have used, not the user's current
   phrasing. (The first wording omitted that fact; the live model then treated
   the visible sample as exhaustive and never searched.)
2. A reply whose first non-empty line starts with `RECALL:` triggers
   `ConversationMemory.Lookup` — same store, same precision gate, minus blobs
   already visible — and the verbatim results (or an explicit "no stored memory
   matches") are appended as a `### Lookup` block. Then the model is asked
   again.
3. At most `maxLookupRounds` (default 2) rounds per generation, each one extra
   inner generation. Repeated queries are answered "(already searched)" without
   re-searching; if the model still wants to search after the last round, one
   forced "answer now" generation prevents scaffolding from becoming the
   user-visible reply.

The anti-hallucination stance is unchanged: the model can only *request* a
search, every result is a verbatim receipted blob, lookups land in
`MemoryGenerationResult.UsedBlobIds`/`Lookups`, and the RECALL exchanges are
never stored as memory. Recalled blob text is data — a stored "RECALL:" string
cannot steer the loop (tested). Models that ignore the protocol simply never
trigger it, at zero extra cost; `maxLookupRounds: 0` removes even the protocol
text.

Measured live (minimax-m3:cloud, fact stored in another session as "the wifi
password at the cabin is grendel-999", question sharing zero keywords):

```
you> what was that secret code i told you i needed for getting my laptop connected at the lodge?
RECALL: lodge laptop WiFi password secret code network
  🔍 model searched "lodge laptop WiFi password secret code network" → #2 #1
I don't have a record of a WiFi password for the lodge specifically. What I do
have in memory is the WiFi password for the cabin: grendel-999.
  ── memory: #2 #1 | prompt 389 tok | 507 tok in 8.5s ──
```

The receipts stay honest even when the model is not: in one pre-fix run the
model invented memories outright ("an old AIS alarm system", a date) — and the
receipt line said `memory: none`, exposing the fabrication mechanically.

## Continuation of truncated answers

A reply that stops because `GeneratedTokens` hit the budget is unfinished, not
done. Observed live: a 512-token quest JSON cut mid-stage, and a bare
"continue" made the model restart from the top and truncate again — recent
turns showed it the text, but nothing said *where the cut was*, so it guessed.

`MemorySession` now arms a continuation when an answer is budget-capped (and
non-whitespace). If the very next message is a resume request ("continue…",
"go on", "keep going", "resume", "finish" — deliberately narrow, and
`continue <anything>` counts), the prompt gets a `### Continuation` block
quoting the final ~600 characters of the cut text with the instruction to
resume exactly there, output only the remainder, repeat nothing. The lookup
protocol is omitted on resume turns (the two instruction blocks conflict).
Chained continues re-arm on each new chunk; an intervening completed turn
clears the pending continuation, so a stale resume is impossible.
`MemoryGenerationResult.Truncated` exposes the state; ghost-chat prints
`(cut off by --max-tokens — type "continue" …)` after any capped answer.

Measured live (minimax-m3:cloud, 400-token cap): the first chunk ended
mid-sentence inside a JSON string — `"journal_entry": "Drevis sent me to the
Arch-Mage.` — and the resume began ` He asked me to confirm the tremors were
magical in nature…"`, completing the same sentence and structure with zero
repetition.

## Auto-continue: unfinished answers resume themselves

Manual "continue" still left the user stitching by hand, so `MemorySession`
now runs the resume loop itself (`maxAutoContinues`, library default 3,
ghost-chat default 8, `--auto-continue N`, 0 = manual only). Two signals mark
an answer unfinished: it stopped at the token budget, or it ends inside an
open ``` fence — a model that emits a natural stop mid-code-block believes it
is done but structurally is not (observed live: EOS with 11 unclosed braces).
Either way the layer resumes, stitches the chunks into one text, stores the
whole as one memory turn, and reports `AutoContinues` (ghost-chat:
`auto-continued ×N` in the receipts). Still unfinished after the cap →
`Truncated` stays true and the manual path is armed with the stitched whole.

Getting seams right took five live failure rounds against minimax-m3:cloud,
each fix earned by a transcript:

- **Structural resumes.** Quoting the partial answer in the system prompt
  degrades as it grows — by round ~5 the model abandoned the resume and
  started a fresh answer mid-string. `ContinueFrom` +
  `ICnetInferenceSession.SupportsContinuation` (Ollama: true) render the
  partial as a real assistant turn — the shape chat models are trained to
  continue. Backends without it keep the quote fallback.
- **Thinking burnout.** think:false is plumbed per-call but the cloud model
  ignores it and can spend an entire round deliberating, emitting nothing. An
  empty round is retried once with a no-deliberation nudge and double budget;
  two empty rounds stop the loop.
- **Seam artifacts** are cleaned mechanically (`CleanResumeChunk`): fence
  churn (a resume inside an open block re-emits ``` markers) is stripped, and
  short re-typed overlaps — including re-typed-with-fresh-indentation — are
  trimmed (6-char minimum so real text is never eaten).
- **Resume sampling.** Deterministic answers resume byte-exact but greedy
  drives long structured output into degenerate repetition loops (observed:
  endless `"flags"` objects), so ghost-chat's default sampling is now
  Balanced; resume rounds drop to Focused (0.3) because at 0.7 the model
  occasionally drops tokens at the seam. Explicit Deterministic is respected.

What the layer guarantees: mechanical stitching is seamless and the loop is
cost-bounded. What it cannot guarantee: the model's own long-form syntax —
minimax-m3 makes occasional JSON typos deep inside a single chunk
(`"type": "choice": "branch"`) with no seam involved. Fewer seams help:
larger `--max-tokens` per round beats more rounds for strict structured
output.

## Consolidation: the ghost store teaches the gap lane

Recall remembers; it never generalizes. CNET's native gap lane generalizes —
it trains a fresh certified specialist per gap against an LM teacher — but had
no episodic feed. `GhostConsolidator` is the seam (hippocampus → cortex):

1. **Extract** (pure read, precision-first like the recall gate itself):
   explicit "remember this" imperatives; user corrections that open by
   overruling an adjacent assistant turn; facts re-queried in a *later*
   session (proven to matter beyond one conversation); and blobs served into
   ≥2 prompts across ≥2 distinct sessions. That last signal comes from new
   durable usage events — `{"used":[ids],"sid":…,"ts":…}` lines the store now
   appends whenever memories earn a prompt slot. Forgotten blobs never
   surface: /forget is anti-teaching by construction.
2. **Emit**: each teachable is noted into the gap-lane inbox via the native
   `cnet_auto_learn_note_skill` from cnet.so — the canonical tagging every
   other producer uses, no managed reimplementation to drift. Skill names are
   deterministic (`gh_<8-hex FNV of text>_<term>`, goal tag within
   PORT_TAG_MAX), so re-consolidating the same memory coalesces in the ledger
   as `times_hit++` — and teachers bind in times_hit order, so repetition
   literally raises teaching priority. Replay strength, mechanically.
3. **Teach**: nothing new — the existing `gap_lane_run` daemon drains the
   inbox, mines the teacher, trains a per-gap MLP, certifies it, and seals it
   into the base with provenance.

ghost-chat: `/consolidate` previews (dry run), `/consolidate commit` emits to
`--gap-inbox <path>` or `CNET_GAP_INBOX`. First live run: 9 teachables
extracted from the real store (1 genuine correction + 8 cross-session
re-queries of the recurring quest-JSON topic), 9/9 noted into the active
personal-ai lane's inbox through cnet.so.

## The orchestrator: self-governance from state, not notifications

`ghost-orchestrator` (dotnet/GhostOrchestrator) closes the loop around the
whole stack. It is a reconcile loop, not an event handler: every tick it
re-derives the world — store growth via lock-free `BlobStore.Snapshot`
(works while a live session holds the writer lock), inbox age, ledger gap
statuses, which lane daemons systemd reports alive — plans whatever actions
close the observed/desired gap, and executes them. A missed event costs
nothing; a restart costs nothing; the next tick reaches the same conclusions
from the same facts. That is what "reinitiate tasks from the info at hand"
means mechanically.

Self-governance under governors (`Orchestration/GhostOrchestration.cs` — the
policy layer is a pure function of observations × state × config, fully
unit-tested):
- every decision journaled with its evidence (`orchestrator.journal.jsonl`);
- per-action cooldowns; exponential backoff on consecutive failures (no
  flapping); a per-tick action budget; `--dry-run` plans without touching
  anything;
- resource-costing actions are permission-gated: the orchestrator will
  *recommend* starting the GPU teacher lane but only starts it under
  `--allow-teacher-start`;
- durable state is only what cannot be re-derived (the consolidation
  watermark, last-run times, failure counts) — atomic tmp+rename saves,
  corrupt state degrades to a conservative restart, never a crash.

Policies today: consolidate when ≥N new blobs pass the watermark; start (or
recommend) the teacher lane when ghost gaps sit `waiting_oracle`; warn when
the inbox has no active drainer or a drainer stopped draining; warn on store
corruption. Deployed as `config/cnet-ghost-orchestrator.service` (5-minute
ticks, consolidation + advisories only by default).

Overnight proof that the pipeline the orchestrator governs actually closes:
the 9 ghost skills consolidated on 2026-07-22 were picked up by the
personal-ai lane, taught against the gemma4 LM teacher, certified, and sealed
into the base at 00:12 — all nine `acq_skill_gh_*` units present, including
`acq_skill_gh_03dc611b_wrong`: a certified specialist distilled from the
user's live correction in the TUI.

## Rung 1: record-as-oracle — learning past the teacher

Everything the gap lane taught before this was distillation: the LM teacher
was ground truth, so the LM was the ceiling. The record teacher
(src/cnet_record_teacher.c, `make record_teacher` gate) breaks that bound for
the first, narrowest case: **user corrections**.

The loop: `ExtractCorrections()` reconstructs (question → wrong answer →
correction) triples from the ghost store; `EmitCorrections()` writes each
correction verbatim as a record file (`<base>.records/skill_corr_<fnv8>.txt`)
and notes a k=1 skill gap. At the lane, `cnet_record_bind` compiles the
record's in-window word transitions into a total function over the window
vocabulary (identity where the record is silent — never abstains, so the
oracle_unfit gate stays quiet, and per-point-distinct, so class_imbalance
cannot trip) and registers it as the gap's oracle with the record bytes as
its identity digest. Standard machinery does the rest: exhaustive 256-point
mine, SGD, `CERT_PROVEN`, sealed unit.

What is different, precisely: the certifying truth is the RECORD — words the
user typed, which the LM teacher never produced. The hermetic gate proves the
sealed unit reproduces the record's transitions exactly, from weights, with
no model loaded. The unit's provenance names `rec_skill_corr_*` as teacher,
not `lm_*`. Narrow as it is (a word-transition function over a 256-word
window), the structural property is the real cargo: **the admission criterion
no longer requires a model's opinion** — any source of verified truth can now
certify a specialist. Rung 2 (generate-and-verify against mechanical
checkers) is the same seam with a different verifier.

## Rung 2 (first verifier): the exact-arithmetic lane

Ported from AICIMO (SkillRouterDriver, 5f7b374 — measured there: escalation
90%→30%). Arithmetic questions short-circuit the model entirely:
`MemorySession` runs `ExactArithmetic.TryAnswer` before building any context,
answers in microseconds by decimal computation, stores the exchange as memory
like any turn, and reports `Exact` (ghost-chat: `── exact: computed — no
model, cannot be wrong ──`; `--no-exact` restores the model path for A/B).

The AICIMO decline discipline is preserved verbatim — a wrong exact answer is
worse than escalating: character whitelist, full-consumption parse ("47 times
89 apples" declines rather than answering 4183), every exception path
declines. One deliberate tightening: division must terminate (reduced
denominator 2^a·5^b, decided by BigInteger factor arithmetic because
decimal's own rounding defeats a round-trip check) — 100/8 answers 12.5,
1/3 declines instead of shipping 28 rounded digits labeled "exact".

This is the template verifier for generate-and-verify: truth by construction,
model opinion not consulted.

## Tests

`dotnet test dotnet/Cce.Llm.Tests` — 19 tests. The generation tests need a local
GGUF fixture and skip cleanly without one; they reuse the model the `CNET.Llm`
integration suite downloads to `~/.cnet-llm/test-cache/` rather than fetching
another copy.
