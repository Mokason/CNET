# Qwythos-9B (qwen35) end-to-end hybrid forward — design + validation record

Date: 2026-07-14
Scope: oracle readiness v1 (CPU; no generation loop, no GPU, no chunked
prefill; nextn/MTP draft head skipped).

## Model / reference provenance

- Model: `/home/marble/Downloads/Qwythos-9B-Claude-Mythos-5-1M-MTP-Q8_0.gguf`
  - sha256: `0d65d20a1a5a96600a0fa6f53dfe636f81e61de6e637cec45bc147be7a3c6544`
  - arch `qwen35`, GGUFv3, 442 tensors; 33 blocks with
    `nextn_predict_layers=1` → **trunk = 32 layers** (blk.32 is the MTP
    draft block, loaded by llama.cpp as an extra decoder block but never
    executed in the main pass — the CNET loader skips it entirely).
  - D=4096, FFN=12288 SwiGLU, vocab 248320 (derived from tok_emb; no
    vocab_size key), untied `output.weight`, rms_eps 1e-6,
    **no bos token** (bos = −1 everywhere; never synthesized).
  - Full-attention layers blk {3,7,11,15,19,23,27,31} (`(i+1)%4==0`);
    the other 24 are Gated-DeltaNet.
- Reference: llama.cpp checkout `/home/marble/llama.cpp` @ `ccb0c3422`,
  **build-cpu only** for all numeric gates. The pinned HEAD enables
  `-funsafe-math-optimizations` in ggml-hip, so the ROCm build is not an
  IEEE-faithful oracle; build-cpu also has no GPU backend compiled in, so
  dump provenance is unambiguous.

## Architecture semantics (pinned against llama.cpp source, not docs)

- **Attention layers**: fused `attn_q [4096→8192]` is per-head interleaved
  `[q_h(256) | gate_h(256)] × 16` (qwen35.cpp view strides); per-head Q/K
  RMSNorm BEFORE rope; kq_scale 1/√256; the softmax context is multiplied
  by `sigmoid(gate_h)` BEFORE o_proj. GQA 16:4 grouped (`h / (nq/nkv)`).
- **RoPE**: IMROPE with sections [11,11,10,0]. For text, llama.cpp fills
  all three used position streams with the same value
  (`llm_graph_input_pos::set_input`), so it reduces EXACTLY to NEOX over
  rope_dim=64 of the 256-dim head, pairing (i, i+32), base 1e7. The loader
  verifies the reduction (every sector maps to t/h/w) and refuses otherwise.
- **YaRN is active at every position**: factor 4 → freq_scale 0.25,
  ext_factor 1.0; the reference's context-level attn_factor cancellation
  (llama-context.cpp:203) means the kernel receives attn_factor 1.0 and
  re-derives mscale = 1 + 0.1·ln(4) ≈ 1.13863 internally; corr_dims from
  the ggml formula = [14, 22] (pair units) at n_ctx_orig 262144.
- **DeltaNet layers**: qkv `[4096→8192]` (q 16×128 | k 16×128 | v 32×128),
  z-gate `attn_gate [4096→4096]` **not convolved**; alpha/beta/z/qkv ALL
  project from the post-attn_norm input; causal depthwise conv `[4, 8192]`
  over the fused qkv pre-split, **tap 0 = oldest** (ggml ssm_conv), SiLU
  after; `ssm_a` stored as −exp(A_log) (loader refuses any entry > 0);
  gate g = ssm_a·softplus(alpha + dt_bias), write gate = sigmoid(beta);
  Q/K L2-normed per head, q scaled 1/√128 (inside the validated
  `cce_qwen35_deltanet_step` core).
- **K→V head broadcast is TILED** (`kh = h mod 16`, `ggml_repeat_4d`
  semantics; llama-model.cpp: "Qwen 3.5: [k0_v0, k1_v1, k0_v2, k1_v3]") —
  NOT the grouped `h/rep` the core applies internally. The runner
  pre-expands q/k to 32 heads with the tiled map and runs the core with
  hk == hv == 32, making its internal grouping inert.
- Gated norm: per-v-head RMSNorm(out, ssm_norm[128]) · SiLU(z_h), then
  ssm_out.
- **No ffn_norm tensor**: `post_attention_norm` is the pre-FFN norm with
  the FFN residual taken pre-norm (llama.cpp graph) — mapped to
  `m->ffn_norm[l]`; `m->post_attention_norm` stays empty so the gemma
  sandwich path never fires.
- Known benign deviation: ggml `l2_norm` is `x/max(‖x‖,eps)` vs the core's
  `x/√(Σx²+eps)` — relative difference ≲5e-7 at these magnitudes; inside
  the parity envelope, core left untouched (53/53 validated).

## Integration shape

Extend `cce_gguf_qwen2` in place (all oracle consumers hard-bind to that
struct + `cce_gguf_qwen2_forward(_probes)` + `cce_gguf_set_layer_tap` +
`cur_pos` rewind + forest branch `"qwen2.lm_head"`):

- `include/cce/cce_gguf.h`: opaque `struct cce_gguf_qwen35_ext *qwen35`
  appended (NULL = classic transformer, legacy byte-identical);
  `cce_gguf_load_qwen35` declared.
- New `src/cce/cce_gguf_qwen35.c` (+ internal `.h`): loader, geometry,
  YaRN precompute, per-layer kind schedule (tensor structure primary,
  `full_attention_interval` cross-check, refuse mismatch), DeltaNet
  state + conv rings, whole-forward implementation, state protocol.
- `src/cce/cce_gguf.c`: whole-forward dispatch at the top of
  `cce_gguf_qwen2_forward`, free hook, export shims
  (`cce_gguf__apply_linear_rows` / `__rms_norm` / `__fire_layer_tap` /
  `__fire_capture` / `__global_clgemm` / `__get_scalar` / `__get_string`).
- `src/cce/cce_detect.c`: `probe_gguf` stamps naming `qwen35-blk` for
  arch qwen35 + fused qkv + ssm tensors; registry row → `open_qwen35`
  → `am->transformer`. `cce_gguf_load_model` also routes the arch string.
- Projections load via `cce_gguf_add_linear_branch` → **CNET_ORACLE_INT8
  works unchanged** (head branch named `qwen2.lm_head` keeps the FP skip;
  nextn tensors are never walked, so nothing MTP is ever mined).

### Oracle state protocol (the new mechanism)

The flagship rewinds `m->cur_pos` for KV-prefix reuse. A KV cache rewinds
positionally; recurrent state does not. The ext keeps `stream_pos` and ONE
checkpoint slot (~53 MB: 24 × [32×128×128] states + 24 × [3×8192] rings):

- `cur_pos == 0` → full state reset
- `cur_pos == stream_pos` → continue
- `cur_pos == ckpt_pos` → restore snapshot
- anything else → **loud refusal**

Committing forwards snapshot at ENTRY (a suffix run can never clobber the
prefix checkpoint). `forward_probes` rows run on scratch copies of state +
ring and leave live state untouched — bit-identical to serial
rewind-per-probe (proven in the hermetic test). `layer_cap` is refused on
the hybrid (a capped forward would desync recurrent state).

New env knobs (both opt-in, default behavior unchanged):
- `CNET_INFER_FP=1` — inference-only FP load: keep FP payloads, drop Adam
  moment buffers (9B FP forest 110 GB → 37 GB). For parity runs.
- `CNET_FOREST_NO_PERSIST=1` — skip the branch archive write (a 9B FP
  forest would write ~37 GB of scratch); branches stay HOT for the process
  lifetime, same posture as the int8 oracle path.

## Validation ladder (all gates green before any campaign)

- **S1 hermetic** (`make cce_qwen35_e2e`, `tests/cce_qwen35_e2e_test.c`):
  tiny self-written qwen35 GGUF (4 trunk = 3 deltanet + 1 attn, + nextn
  block; partial rotary, YaRN mscale≠1, tiled 2:4 broadcast, conv kernel >
  tokens/call), double-precision reference. **34/34 checks**: worst logit
  |diff| 6.53e-06 (tol 1e-4); bit-identical: reset+replay, batch==serial,
  prefix rewind, probes==serial + live state untouched, Q8_0 container ==
  FP container (grid-snapped weights), nextn skip exact, tap on/off;
  int8 head-FP skip; head-window bit-identity; refusals (wrong-arch
  loaders, missing ssm_a, arbitrary rewind).
- **S2 per-layer parity** (`make qwythos_e2e`, needs dumps from
  `qwen35_parity_dump`): layer tap (l_out) ladder vs llama.cpp CPU on the
  real model — 1-token pos-0 run + 8-token window-id run. Envelope: warn
  relL2 > 5e-3, hard-fail > 0.05; result_output argmax + ORDERED TOP-3
  identity (the oracle contract; top-8 reported informationally).
  **Measured 2026-07-14**: 1-token ladder relL2 5.9e-3 .. 3.1e-2 (worst
  L28), 8-token ladder 7.2e-3 .. 2.3e-2 (worst L19) — every layer under
  the 0.05 hard limit, no discontinuity at any layer (a wrong formula
  would spike O(0.1–1) at its first layer). Both argmaxes identical;
  ordered top-5 identical on the 8-token run. The envelope floor sits
  above 5e-3 because llama.cpp's CPU Q8_0 matmul quantizes ACTIVATIONS
  to Q8_0 (q8×q8 vec_dot) while CNET runs fp32 activations on exactly-
  dequanted weights — the reference itself carries that noise. Deep-rank
  observation, on record: top-8 rank 6/7 swapped (gap 0.036) and rank 8
  flipped (ref id 263 @ 7.8291 vs #9 id 83 @ 7.7713, gap 0.058 on
  ~10-magnitude logits); the reference is byte-self-consistent between
  batched and one-token-sequential evaluation (measured, identical
  top-10 logits), so the flip is the fp32-vs-q8-activation implementation
  delta, not reference instability. Oracle-relevant margins (top-3:
  ≥ 0.27) are an order of magnitude above this noise.
- **S3 token identity** (same binary, stage B): 32 greedy tokens from
  "The capital of France is" vs llama.cpp `gen_tokens.txt`.
  **Measured 2026-07-14: 32/32 token-for-token identical.**
- **S4a depth_probe preflight**: `CNET_WINDOW_FILE=english_window_256_qwythos.txt`
  (depth_probe gained the window-file mode — mandatory on a BOS-less
  model), V=256 N=128, int8 campaign config. Gates: 0 probe/head
  mismatches, distinct decisions ≫ 1, head banner `w_q=no w_trit=no`.
  **Measured 2026-07-14**: validity **0/128 mismatches**; distinct
  full-depth top-3 **123/128** (the english window is ALIVE on this
  model — contrast gemma's bos-less collapse); head stayed FP under
  int8; 1.60 s/probe single-thread. Depth-cap verdict: **DEAD** —
  top-3 saturates only at 32/32 (argmax partially saturates much
  earlier than gemma4-v2: 61/128 at cap 28 — the recurrent layers do
  settle many decisions early — but never completely below full depth).
  Full table in `scratchpad`/session log; mine at full depth only.
- **S4b flagship startup gates** (smoke campaigns, topk V=256):
  **Measured 2026-07-14**: window file loads (256 ids validated);
  **determinism spot check OK in FP and int8 modes** (exercises the
  DeltaNet checkpoint/restore under interleaved queries on the real
  model); golden battery written, 32 probes each mode; **FP and int8
  goldens are IDENTICAL** — with the head FP in both modes, int8 layer
  quantization flips none of the 32 argmax decisions on this model;
  restricted head engages for mining. Smoke-unit note: 60 s wall
  produced `deferred (certify_failed)` (mine+teach+certify cannot fit
  60 s at D=4096 — a wall-clock artifact, not an oracle defect); the
  prefix A/B runs use a 900 s wall.
  Prefix A/B (ON vs OFF, 900 s smokes, **measured 2026-07-14**):
  determinism OK and **golden battery 32/32** in BOTH modes; deferral
  traces, `.cnb` bases and gaps ledgers byte-identical. Caveat, on
  record: both runs deferred both units (`certify_failed`), so the
  unit-level artifact comparison is vacuous (empty bases) — the prefix
  bit-identity claim rests on the golden replay + the hermetic test's
  exhaustive rewind identity. The `certify_failed` itself is a
  TEACHING-side finding, not an oracle defect: the student did not
  certify against a high-entropy teacher (123/128 distinct top-3) at
  D=4096 within the attempt budget. First item for the campaign
  session: teach/escalation budget (and CNET_CERT_MARGIN posture) for
  hybrid-scale units; re-run the unit-level A/B once units certify.

## certify_failed diagnosis + campaign posture (2026-07-14, same day)

The smoke runs' `certify_failed` deferrals were DIAGNOSED, not tuned away:

```
CERT_DIAG acq_tk2107q2107: 252/256 exemplars exact, verdict=REFUSED, worst_margin=0.0001
```

The memorization-scale student (64→256 hidden, 12k epochs, adaptive)
reproduces 252/256 mined exemplars exactly; the 4 misses sit on ordered
top-3 boundaries the teacher protects by a **1e-4 logit gap** — the
model's own coin-flip points, which no student can memorize and no
epoch/capacity budget can close. This is the gemma4 wall verbatim
(254–255/256, worst_margin ~0.001, "no sharp boundary"): on the
exhaustive/PROVEN tier a real (non-constant) oracle certify_fails by
construction. The smoke posture (margin 0, PROVEN tier) was the
strictest possible configuration — the deferrals were the tier working
as designed, not an oracle or teach-budget defect.

**Campaign posture** (the completed soul_english_v1 recipe, applied):
`CNET_CERT_MARGIN=0.02` (≈200× the observed coin-flip scale:
teacher-ambiguous contexts abstain from the cert domain at mining time)
+ `CNET_CERT_SAMPLED=1` / `CNET_CERT_SAMPLE_COUNT=96` (Wilson ≥ 0.95,
exactness-on-sample) + student 128→512 hidden / 20k epochs / adaptive.
Recorded and replayable via `qwythos_english_v1.cnb.manifest.json`
(committed; `CNET_MANIFEST=<file>` replays the full recipe including
window fnv and the int8 golden battery). Note: the manifest's
`build_rev` reflects the binary's compile-time rev — rebuild
flagship_run after pulling before the full campaign so provenance
records the current tree.

**Verified 2026-07-14**: 4-unit run under this posture —
`attempted 4, acquired 4, deferred 0`, every unit 100% exact on its
certification domain (101/101, 101/101, 98/98, 96/96 exemplars; the
count spread is the margin gate abstaining on teacher coin-flip
contexts), all SAMPLED-certified at Wilson ≥ 0.95. The unit that was
stuck at 252/256 under PROVEN (tk2107) converts cleanly.

**Full campaign completed 2026-07-15** (`tools/mining_campaign.sh`,
3 passes, converged dry at 07:24): **acquired 207/256 (80.9%),
deferred 49/256 — every deferral `oracle_unfit`, and the SAME 49 every
pass** (0/49 converted on retries with fresh seeds: the residue is a
property of the teacher, not the students). Those 49 window tokens
condition distributions where >10% of the 96-point sample falls under
the 0.02 top-3 margin — genuinely ambiguous conditioning contexts for
this model. Base: `qwythos_english_v1.cnb` (221 MB, 207 certified
units); ledger committed. Two launcher fixes landed en route
(`e8a7020`): absolute-path requirement for the systemd log sink, and
ledger-v4-aware acquired/deferred counting (the v1 rule read every v4
row as a deferral and dry-stopped the retry loop after one pass).
Possible yield lever for the 49, deliberately NOT applied blind:
`CNET_TOPK_SET=1` (top-3 SET semantics — the abstention margin narrows
to the rank-3/4 boundary, so ordered ties inside the top-3 stop
abstaining); mining under set semantics changes what a unit claims, so
it belongs to a deliberate follow-up campaign, not a retry pass.

## Unit-level prefix A/B: two oracle-integrity bugs found + fixed (2026-07-15)

The certifying-units prefix A/B (ON vs OFF, same posture/seed, fresh
bases) initially DIVERGED (tk2590 acquire/defer flip) and exposed two
independent bugs, both of the same class — a deterministic,
self-consistent chimera oracle that certifies its own students and is
invisible to every same-mode gate (goldens, determinism check,
same-shape bitwise probes):

1. **qwen35 runner — stale checkpoint across resets**: a stream reset
   (new unit's prefix) did not invalidate the rewind checkpoint, so
   suffixes 2..N of every unit after a process's first restored the
   PREVIOUS unit's recurrent state. Fixed (`ckpt_pos = -1` on reset);
   hermetic unit-boundary regression added (fails without the fix,
   36/36 with).
2. **flagship — lane prefix cache survives foreign forwards**: the
   determinism check primes `prefix_token` with `vocab[0]` (always
   unit 1's token), the golden battery then clobbers the stream with 32
   direct forwards, and unit 1's `fs_prefix` trusts the stale cache —
   unit 1 mines against the LAST GOLDEN PAIR's state. **This bug
   predates the hybrid: every goldens-enabled campaign poisoned exactly
   its first unit (gemma soul bases included — their vocab[0] units are
   suspect).** On the hybrid, the runner's rewind refusal turned the
   silent corruption into a loud oracle_unfit, which is what exposed
   it. Fixed: lanes invalidate `prefix_token` after the battery.

Forward-path exoneration (measured): prefix+rewound-suffix vs fresh
2-token contexts are bit-identical across the full window in BOTH the
re-prefix-per-probe shape and the exact flagship restore-chain shape
(0/256 rows differ, 0 abstention flips, t=2107 and t=2590). Lesson
recorded: prefix-identity gates must ALTERNATE conditioning tokens and
include foreign-forward interleavings; same-t comparisons are
structurally blind to both bugs.

**Post-fix A/B (final): ON and OFF produce byte-identical bases and
ledgers (sha256 22be1575…, equal to the pre-fix OFF truth reference)
with identical acquire/defer patterns.** Prefix reuse is now a proven
pure speed change. The 2026-07-15 207/256 campaign base was mined
pre-fix and is POISONED (quarantined as
qwythos_english_v1.cnb.POISONED-stale-ckpt; ledger commit 939780e is
superseded); the campaign re-ran on a fresh base with both fixes.

**CLEAN campaign final tally (2026-07-15 20:49, supersedes 939780e):
acquired 89/256 (34.8%), deferred 167 — all oracle_unfit, provably
seed-independent** (pass 2 converted exactly the 4 certify_failed
units and nothing else; the 167 re-deferred identically, so the
campaign was stopped before pass 3 re-churned them a third time). The
true teacher is far less decidable than the chimera made it look: the
poisoned run's 207 was inflated by mismatched recurrent state
sharpening genuinely-flat distributions. 89 units = the model's
honestly-confident subset under ordered-top-3 at margin 0.02 /
evidence 0.9. Base: qwythos_english_v1.cnb (95 MB). Yield levers for
v2, to be MEASURED before chosen (one margin-distribution sweep over
the window): CNET_TOPK_SET=1 set semantics (ordered ties inside the
top-3 are the dominant abstention driver) and/or CNET_WINDOW_SCREEN
decisiveness screening. Either is a new campaign with new goldens —
unit claims change; never a retry pass.

## Deferred (out of v1)

Generation quality/coherence (chat template + sampling + the missing
prompts TSV), GPU/clgemm equivalence + dual-R9700 pool, MTP draft-head
execution, long-context YaRN regime (contexts ≤ 64 in v1), chunked
DeltaNet prefill (prefill = N recurrent steps), int8 decision QUALITY
(mechanics gated only; int8 goldens are a separate, mode-specific file),
llama.cpp-anchored 32-pair golden cross-check via a `pairs:` dump mode
(decision identity is currently anchored by S3's 32-step greedy chain +
the 8-token argmax/top-8 gate).
