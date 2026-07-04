# Oracle Speed Ladder + Window Integrity — Design & Findings

**Date:** 2026-07-04
**Status:** Built and measured same session (user directive: "run the ladder…
keep the soul of the project alive. test it. now is the time").
**Topic:** Attack the 12B campaign bottleneck (48-layer monolithic CPU
traversal per oracle call, ~97% of unit time) without moving a single bit of
mined knowledge — and the two integrity defects the instrumentation exposed.

## The ladder (each rung digest-audited against a trusted-path base)

Baseline: gemma4-v2-Q4_K_M (48L/3840/262k vocab), CNET_ORACLE_INT8=1,
2-lane GPU pool, TOPK V=256: **251.6 s/unit** (projected 17h/256 units).

| rung | change | s/unit | cum. | why it cannot move bits |
|------|--------|--------|------|--------------------------|
| R1a | `-mno-avx` is a MinGW stack-bug workaround — now Windows-only; Zen 5 runs the matvecs AVX-512-wide | — | — | per-output accumulation is independent per lane; SIMD width can't reorder it |
| R1b | int8/trit/float matvec OMP gates were `out_dim >= 4096` — at hidden 3840 the q/k/v/o/down matvecs ran SINGLE-threaded; now work-based (`in*out >= 2^21`) | 169.8 | 1.48x | threading tiles disjoint outputs; per-output order unchanged |
| R2 | per-unit KV prefix reuse: all 256 calls of a unit share conditioning token t — compute its KV row once per lane, calls feed only the suffix (2→1 tokens argmax/topk, 3→2 pair). `CNET_ORACLE_PREFIX=0` opts out | 103.8 | 2.42x | the prefix KV row is a pure function of (token, weights); attending to the cached row is the same math as recomputing it |

Digest audits: R1 4/4 IDENTICAL, R2 4/4 IDENTICAL vs the base mined by the
untouched slow path. R1's modesty is diagnostic: the CPU matvecs are
memory-bandwidth-bound (~11 GB int8 streamed per call over ~80 GB/s DDR5),
which is why R5 (below) is the ceiling-raiser.

## Integrity finding 1: int8-on-load quantized the HEAD

`cce_gguf_add_linear_branch`'s CNET_ORACLE_INT8 path quantized EVERY branch
— including `qwen2.lm_head` — while the post-hoc quantizers deliberately
skip lm_head + MTP "for quality". Consequences: (a) the campaign oracle's
final decision layer was int8 (exactly the rankings being mined), and
(b) since int8 blocks bypass the clgemm seam, the head never ran on the
GPUs for 12B campaigns — the pool's gains there were pure CPU-lane
parallelism. FIXED (user decision): on-load now makes the same lm_head/mtp
skip; the FP head is clgemm-eligible again (4 GB resident per lane). The
int8-head-era base is archived (`soul_gemma4v2.int8head.cnb.bak`), fresh
era re-baselined.

## Integrity finding 2: the fixed window mines CONSTANT knowledge

The depth-saturation probe (below) returned "decisions identical to full
depth at layer 1" — too good to be true, and it was: with probe-vs-model
head validation (0/128 mismatches) and a distinct-decision count added,
window 2000..2255 shows **1 distinct top-3 over 128 probe contexts on BOTH
real models** (12B and the MTP draft), the exact `[first,second,third]`
tie-break pattern of flat logits. Those ids are vocabulary the models never
predict; every ARGMAX/TOPK unit mined from the fixed window — including
historical draft-era bases — is one constant function certified 256 times.
(Exhaustive mode deliberately skips the degenerate-pilot check: "a proven
constant function is legitimate knowledge." True per-unit; vacuous per-
campaign.)

FIX: PAIR's window discovery generalized (`tests/window_discover.h`, shared
by flagship_run and depth_probe so both derive the IDENTICAL window):
ARGMAX/TOPK now default to a discovered window built from the model's own
argmax attractors over a deterministic probe spread (resume-safe: same
window, same tags on every run). `CNET_WINDOW_DISCOVER=0` restores the
fixed window. PAIR behavior byte-identical (n_ctx=3, same formulas).

## The depth probe (tests/depth_probe.c + per-layer tap in cce_gguf.c)

After every layer it projects the last row through final-norm + the head's
WINDOW columns (same dequant math and accumulation order as the oracle's
own head — validated per probe against the model's returned logits) and
records the ordered top-3. Verdict table: per candidate cap K, the fraction
of probes decision-identical to full depth; plus the distinct-decision
count (degeneracy alarm). `m->layer_cap` exists in the forward (default 0 =
full depth, byte-identical); enabling it for mining REQUIRES the same
posture as the GPU path: startup full-vs-capped decision sweep + refusal +
digest audit — certification alone cannot catch capped-oracle drift because
units certify against the oracle that mined them.

## Rung 5 (planned): RDNA4-resident int8 layers

The CPU int8 matvec accumulates in FLOAT (`acc += a * (float)q`), so a
bit-identical GPU port needs no intrinsics: the clgemm kernel reading int8
weights (convert per element) reproduces the math exactly, with 4x less
traffic than float weights, on ~640 GB/s GDDR6 vs ~80 GB/s DDR5 — the
bound resource, ~8x. (`cl_khr_integer_dot_product` is absent on this ROCm;
irrelevant for v1. Packed-int8 dot = v2 only, since quantizing ACTIVATIONS
changes math → decision-gate territory.) VRAM: ~12 GB int8 + 4 GB FP head
per lane; GPU1 currently holds ~25 GB of llama-server residents — needs
relocation or asymmetric lanes when built.

## Integrity finding 3 (the root): the gemma4 forward was never real

Chasing the depth probe's too-good verdicts down three layers exposed that
NO gemma4 campaign ever computed real model outputs, on any box:

1. **GGUF data-section alignment** (cce_gguf.c load): the data base used the
   raw post-table file position instead of aligning up to general.alignment
   (32) — EVERY tensor read shifted (10 bytes on these files): NaN/garbage
   weights. A file whose table happens to end aligned loads fine, which is
   how small-file tests passed. FIXED (align-up honoring general.alignment).
2. **NaN vacuity of every gate**: NaN defeats every float comparison, so
   determinism checks, gpu_equiv (max|dlogit| = "0.0" — including the
   celebrated Windows-era number in 43a19ee), digest audits and PROOF
   certification all passed on a NaN-constant oracle. FIXED: NaN guards in
   gpu_equiv and the flagship startup sweep (a single NaN logit = hard
   FAIL); depth_probe prints logit forensics + distinct-decision counts.
3. **gemma4 attention dims + swallowed errors** (the root): attn_q emits
   H*HD (4096) while the forward allocates/consumes hidden-sized (1024 /
   3840) buffers; apply_linear_rows' error return was IGNORED, so q (and
   k/v on the 12B) were UNINITIALIZED HEAP in every campaign — found via
   CNET_FWD_TRACE stage checksums (q(l0) checksum 0 on fresh pages, random
   on recycled heap, varying across processes via ASLR). FIXED for honesty:
   projection failures now REFUSE the forward loudly (gemma4 models refuse
   to run rather than improvise); the faithful gemma4 attention
   implementation (per-layer kv-head list, dual head_dim global=512 /
   swa=256, q/k norms, sliding window) is the NEXT work item, to be
   validated token-level against llama.cpp before any campaign.

Also fixed en route: the OpenCL kernel now builds with FP_CONTRACT OFF —
the compiler's FMA fusion drifted ulps from the CPU's separate mul+add and
flipped near-tie argmax decisions; with contraction off the GPU seam is
BIT-identical to CPU (tests/clgemm_unit.c: 450/450 calls across
single/dual/split placements), so mixed CPU/GPU fallback can never change a
decision. Debug instruments kept (env-gated, zero cost off):
CNET_GPU_VERIFY=1 (per-call CPU recompute + GPU-twice nondeterminism check),
CNET_FWD_TRACE=1 (stage checksums).

**Consequence for prior results in this spec**: the R1/R2 digest audits and
all campaign numbers above were measured against the NaN-era oracle — the
TIMINGS stand (same arithmetic), the digest-identity claims must be
re-proven against a real oracle once the gemma4 forward is faithful.

## Verification discipline (unchanged, restated)

Every rung: (1) startup gates (per-lane equivalence sweep, cross-lane
determinism), (2) cnb_audit behavior-digest fidelity vs a trusted-path
base, (3) refusal beats silent divergence. A fresh trusted reference is
re-mined whenever the ORACLE definition changes (FP head, discovered
window) — speed changes must never be conflated with oracle changes.
