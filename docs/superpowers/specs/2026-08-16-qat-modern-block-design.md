# QAT Modern Block — RMSNorm, RoPE, GQA, SwiGLU, optional bias

Status: DESIGN (no code yet). Date: 2026-08-16.
Origin: the ternary creative hemisphere needs a teacher it can distil from,
and `cce_transformer_qat` is GPT-2 shaped — it cannot load any modern
checkpoint. Named as the long pole in
[2026-08-16-core-attribution-layer-design.md](2026-08-16-core-attribution-layer-design.md) §10.

Scope: the transformer **block** and its gradcheck only. Real-checkpoint
loading (HF/GGUF tensor-name mapping, the ne-order reversal, tokenizer) is a
separate spec — see §8.

---

## 1. What is actually there (grounded in the tree)

`src/cce/cce_transformer_qat.c` is 896 lines; `include/cce/cce_transformer_qat.h`
is 93. Verified against source:

| claimed | reality |
|---|---|
| hand-rolled forward + backward | **REAL** — `tr_forward`, `tr_backward`, Adam, full activation cache |
| gradcheck exists | **REAL** — `cce_transformer_qat_gradcheck` |
| gradcheck is per-parameter central differences | **FALSE** — it is a **directional** derivative over the whole parameter vector (Rademacher `u`), with the reason in a source comment: float32 forward noise cannot dominate as it does for individual near-zero params. Plus a per-group max-\|g\| pass. |
| GPT-2 shaped | **REAL** — LayerNorm (mean + rstd, weight *and* bias), learned `pos_emb`, fused QKV `[D][3D]`, MHA, tanh-GELU MLP (`up`→act→`down`), mandatory biases on qkv/proj/up/down |
| the QAT gate runs on the Windows box | **FALSE** — `make transformer_qat` does not build: `$(CCE)` pulls `cce_safetensors.c` (needs `curl/curl.h`, header absent), `cce_kv_page.c` (POSIX 2-arg `mkdir`, `fsync`), `cce_gguf.c` (`mmap`) |

Parameter bundle: `P { float *w,*g,*m,*v; int in,out; }`, `[in][out]`
row-major, bias represented as `in == 1`. Scale orientation matches
`cce_block_quantize_ternary` (per-OUTPUT column absmean) so a trained shadow
exports straight into the existing ternary pipeline. That orientation is
load-bearing and does not change in this work.

### 1.1 A live hole in the gate

`cce_transformer_qat_gradcheck` walks a hand-built array:

```c
P* groups[128];
...
if (ng + 12 > 124) break;   /* keep room for the 4 tail groups */
```

Two consequences, both silent:

- Any parameter group **not** appended to `groups[]` is never gradchecked. A
  stride or transpose bug in a new op would pass a green gate.
- SwiGLU adds a **13th** matrix per layer. The `ng + 12` bound and the
  `groups[128]` capacity are both wrong the moment a group is added, and the
  `break` drops the remaining layers *quietly*.

This hole exists today. It is fixed in §6 before any new op is written,
because every other gate in this spec depends on the gradcheck being total.

## 2. Design

Five ops become independently-composable units selected by config. **Zero-init
of the config yields exactly today's model**, so the existing Supra path and
its gate stay green without edits — that is the compatibility contract.

| config field | legacy (zero-init) | modern |
|---|---|---|
| `norm_kind` | `QAT_NORM_LN` = 0 — mean + rstd, weight + bias | `QAT_NORM_RMS` — rstd only, weight only |
| `pos_kind` | `QAT_POS_LEARNED` = 0 — `pos_emb`, frozen FP | `QAT_POS_ROPE` — `rope_theta`, no parameters |
| `mlp_kind` | `QAT_MLP_GELU` = 0 — up → tanh-GELU → down | `QAT_MLP_SWIGLU` — (gate, up) → SiLU(gate)·up → down |
| `n_kv_head` | `0` → treated as `n_head` (MHA) | `< n_head`, must divide it (GQA) |
| `no_bias` | `0` → biases present (legacy) | `1` — no qkv/proj/up/down bias |

The field is `no_bias`, not `use_bias`, deliberately: zero-init must mean
legacy, and legacy *has* biases. Naming it positively would require the
zero value to mean "off", inverting the contract in §2.

### 2.0 RoPE pair convention — pinned, not left open

Two conventions exist for which dimensions get rotated together: **interleaved**
(pairs `(0,1), (2,3), …`) and **half-split** (pairs `(i, i + hd/2)`). They are
not interchangeable, and picking wrong produces a model that trains fine and
matches no reference.

This spec implements **half-split** as the default, since that is what the
HF Llama/Qwen reference implementations use, and exposes the choice as
`rope_pairing` so the loading spec can flip it per-architecture **without
touching the backward pass** — the backward is the transposed rotation either
way, so pairing is purely an index mapping shared by both directions.

### 2.1 Backward cost of each op

- **RMSNorm** is strictly simpler than LayerNorm — the mean term disappears
  from both passes. No new parameter beyond dropping the bias.
- **RoPE** is a fixed rotation with **no parameters**. Its backward is the
  transposed (inverse) rotation; it contributes no gradient of its own, only
  correct propagation. Applied to Q and K after the QKV projection, per head,
  over the head-dim pairs.
- **SwiGLU** adds one parameter matrix per layer (`gate_w`) and a product rule
  across two branches: `d(gate) = dy · up · SiLU'(gate_pre)`,
  `d(up) = dy · SiLU(gate_pre)`.
- **GQA** changes head indexing only. Fused QKV `[D][3D]` splits into
  `q_w [D][D]`, `k_w [D][kv·hd]`, `v_w [D][kv·hd]`. Each query head `h` reads
  KV head `h / (n_head / n_kv_head)`. Backward accumulates into the shared KV
  head across every query head that read it — **this accumulation is the most
  likely place for a silent bug**, and is called out explicitly in §6.
- **No-bias** skips the bias accumulate; it is an omission, not a new path.

### 2.2 Cache changes

`qkv [L][T][3D]` becomes `q [L][T][D]`, `k [L][T][kv·hd]`, `v [L][T][kv·hd]`.
SwiGLU adds `mgate [L][T][M]` alongside `mpre`/`mpost`. Legacy config
allocates the legacy shapes, so its memory profile is unchanged.

## 3. The loader split (prerequisite)

`cce_transformer_qat_load_decomposed` moves verbatim to a new
`src/cce/cce_transformer_qat_load.c`, together with its two static helpers
(`copy_into_P`, `supra_last_block`). The trainer core then references no
safetensors/GGUF symbol.

That lets the hermetic gradcheck gate link **without** `$(CCE)`, so it builds
on any box. The test is already documented as hermetic ("no model files
needed"); this makes it hermetic in link terms too.

Public API is unchanged — same header, same symbol, different translation
unit. Callers are untouched.

## 4. Config validation is refusal, never clamping

`cce_transformer_qat_create` returns `NULL` (and the config-checking entry
point returns `CCE_ERR_INVALID_ARG`) when:

- `n_embd % n_head != 0`
- `n_kv_head != 0 && n_head % n_kv_head != 0`
- `n_kv_head > n_head`
- `pos_kind == ROPE && rope_theta <= 0`
- `mlp_kind == SWIGLU && mlp_hidden <= 0`
- `norm_kind`, `pos_kind` or `mlp_kind` out of enum range

A silently-clamped config trains a model that is not the one that was asked
for, and the resulting weights would be labelled as something they are not.

## 5. What does NOT change

- The `P` layout, `[in][out]` orientation, and per-output absmean scale
  orientation — the ternary export pipeline depends on all three.
- The STE/shadow-weight QAT recipe.
- Adam, the loss, the soft-KD path.
- The public header's existing symbols and their signatures.

## 6. Gates

Ordered; each depends on the one before it.

1. **`groups[]` registration audit.** Assert every allocated `P` in the
   trainer is reachable from the gradcheck's group array, for *every* config
   in §2. Implemented by having `create` register groups into a
   dynamically-sized list that both allocation and gradcheck read, so the
   audit is structural rather than a maintained duplicate list. Fixes the
   `groups[128]` / `ng + 12` hole in §1.1. **This runs first: every gate
   below is only as good as the gradcheck being total.**
2. **Progressive directional gradcheck**, one config per step: legacy →
   +RMSNorm → +RoPE → +GQA → +SwiGLU → +no-bias. A failure localises to the
   feature just enabled. Tolerance matches the existing gate's.
3. **GQA accumulation check.** With `n_kv_head < n_head`, assert the KV
   gradient equals the sum over the query heads that read it, against a
   direct recomputation. Gradcheck alone can miss a factor that cancels in
   the directional projection.
4. **Legacy byte-identity.** A fixed-seed model with zero-init config
   produces bit-identical logits to the pre-refactor build. This is the proof
   that the existing Supra path did not move.
5. **RoPE properties.** Rotation preserves vector norm, and
   `⟨q_m, k_n⟩` depends only on `m − n` (relative-position invariance).
6. **Existing gates unaffected:** `make transformer_qat` and
   `make supra_train` still pass — on the Linux box, since they need `$(CCE)`.

Gates 1–5 run in a new hermetic target buildable anywhere; gate 6 is
Linux-only and stated as such rather than assumed.

## 7. Known limitation, stated not hidden

Gates 1–5 prove the block's *math* is self-consistent. They do **not** prove
it matches any reference implementation of Llama or Qwen — that requires a
real checkpoint and belongs to the loading spec (§8). A block can be
internally consistent and still differ from the reference in convention.
§2.0 pins RoPE pairing to half-split and makes it switchable; the remaining
convention risks the loading spec must verify against a known-good runner
are RMSNorm epsilon placement (inside vs outside the sqrt) and whether the
norm weight is applied as `w` or `1 + w`. Neither is decidable without a
reference, so neither is guessed here.

## 8. What this unblocks

The follow-on spec is real-checkpoint loading: mapping HF/GGUF tensor names
onto the new `q_w`/`k_w`/`v_w`/`gate_w` matrices, honouring the ne-order
reversal already proven in the universal-runner work, and forward-equivalence
against `cce_st_llama` on identical weights. Only after that does ternary
distillation of a real teacher become possible.
