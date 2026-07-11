# Supra QAT Trainer Scope — CNET Packed 1.6-bit Quality Phase

Status: SCOPING (no trainer code yet). Date: 2026-06-28.
Prereq proven: the storage/format phase is closed (see "Proven state").

---

## 1. Proven state (storage/format — done)

CNET/Supra has a real, reloadable packed-ternary artifact:

| representation | size |
|---|---|
| Original Supra checkpoint | 118.9 MB |
| CCE FP specialists | 64.4 MB |
| INT8 CCE | 16.3 MB |
| **Packed ternary artifact (actual on disk)** | **6.95 MB** |

Verified facts (all reproducible from the harnesses used during the storage phase):
- in-memory packed parity vs ternary-debug: `max|Δlogit| = 0`
- reloaded-from-disk artifact parity vs ternary-debug: `max|Δlogit| = 0`
- packed `tok_emb`, `head`, and all specialist matrices (trits, 5/byte, base-3)
- no FP big tensors on reload; FP kept only for pos-emb, LayerNorm params, biases (by design)
- FP / INT8 baselines unchanged

**This proves storage and format. It does NOT prove quality.** Post-hoc ternary changes
the Supra argmax (FP `5811` → ternary `4077`). Quality requires QAT.

---

## 2. Goal

Build the **smallest honest Supra QAT trainer** that answers one question:

> Can a Supra ternary matrix recover quality when trained with FP shadow weights,
> ternary forward, and STE backward — beating post-hoc ternary on the same eval?

First milestone is NOT production training. It is:
> **Loss moves the right way, and QAT ternary clearly beats post-hoc ternary** on the
> same Supra prompt/token evaluation, with packed export/reload parity still exact.

---

## 3. Code reality (what exists vs what's missing)

Grounded in the current tree, because this is what determines the build order.

**Exists / reusable:**
- `cce_supra_gpt_forward` (src/cce/cce_safetensors.c) — verified FP/ternary/trit forward,
  **inference-only**. Specialists are frozen `cce_block`s loaded from safetensors.
- `head` is the `gpt.head` specialist: a single `cce_block`, `[in=256, out=50520]`.
  The forward already computes the head **on the last hidden row only**.
- `cce_block` has ternary/trit **forward** (`cce_block_quantize_ternary`,
  `cce_block_pack_trits`) — but **no trainable path** (no STE backward).
- `cce_wordlm` (src/cce/cce_wordlm.c) has a **proven** ternary BitLinear with FP shadow
  weights + STE for exactly a class-factored head + embedding — hand-rolled backprop,
  gradcheck'd, and shown to beat post-hoc (ppl 1.95 QAT vs 61.5 post-hoc; embedding-only
  even improved on FP). **This is the pattern to copy.**
- `cce_autograd.c` — reverse-mode tape + custom-op ABI, but **not wired** into the Supra
  forward.

**Missing (the work):**
- No backward through the Supra transformer (attention, LN, MLP, residuals, embedding, head).
- No FP shadow-weight storage / optimizer state for the Supra specialists in training mode.
- No corpus loader / training loop for Supra.

---

## 4. Key refinement: the first milestone needs NO transformer backward

The highest-risk matrix is the **head** (it touches every logit). It is also the most
**structurally isolated**: it is a single linear classifier `[hidden → vocab]` applied to
the final hidden vector.

**Head-only QAT can be done by freezing the entire transformer:**
1. Keep the FP transformer frozen. For each training example, run the existing forward to
   the final hidden vector `h = ln_f(blocks(embed(tokens)))[last]`. Cache `h`.
2. This yields a dataset of `(h, target_token)` pairs — a standalone linear-classifier
   training problem, identical in shape to `cce_wordlm`'s class/word head.
3. Train a ternary BitLinear head: FP shadow `W_head[vocab,hid]`, ternary forward
   (per-row absmean), STE backward, CE loss. **No attention/MLP/embedding backward needed.**

Consequences:
- The first, highest-value, highest-risk experiment (head-only) reuses the **already-proven**
  `cce_wordlm` STE machinery and requires **zero transformer backward**.
- It directly answers: *does QAT beat post-hoc on the head?* If yes, the quality phase has a
  real path. If no, full all-ternary training is unlikely to work without a better loss/setup
  — and we learn that cheaply.
- Same trick applies to `tok_emb`-only (cache the input-side gradient target) but embedding
  is lower-risk, so head-only goes first.

Transformer backward (attention/MLP/LN) is only required once we move past frozen-context
component QAT into joint training (steps 7–10 below).

---

## 5. Non-goals (this milestone)
- Do NOT optimize the trit kernel speed (it's correct; slow is fine here).
- Do NOT ternarize small quality-sensitive tensors (norms, biases, pos-emb stay FP).
- Do NOT claim deployable quality from a smoke test.
- Do NOT build a generic training framework unless a step forces it.
- Do NOT target Gemma-scale until Supra QAT has a measured result.

---

## 6. Tensor policy

| QAT/ternary candidates | keep FP (for now) |
|---|---|
| `tok_emb` | positional embeddings |
| `head` | LayerNorm / RMSNorm params |
| block MLP up/down | biases |
| block attn `qkv` | regenerated attention masks |
| block attn `proj` | metadata |

---

## 7. Required backward paths (full list, for when we go joint)

Listed so the effort is explicit; **only "Head" is needed for milestone 1.**

- **Embedding** — fwd: ternary row lookup `γ·code`; bwd: scatter grad into FP shadow row (STE = identity).
- **Head** — fwd: `h × ternary_head → logits`; bwd: CE/distill grad → hidden (via ternary head) + STE outer-product into FP shadow head. *(Milestone 1.)*
- **MLP** — up/down linear fwd/bwd, activation (GELU) bwd, residual-add bwd, STE into shadows.
- **Attention** — qkv proj bwd, score bwd, softmax bwd, value-aggregation bwd, output proj bwd, causal mask, residual-add bwd, STE into qkv/proj shadows.
- **Norms** — LayerNorm fwd/bwd, params kept FP, grad flows through into the residual stream.

---

## 8. Loss

**Milestone 1 (smoke):** next-token cross-entropy on a tiny corpus.
Pass: QAT loss decreases; QAT final < post-hoc; FP ≤ QAT; no NaNs; deterministic seed.

**Stage 2 (distillation):** teacher = FP Supra.
```
loss = CE(student, target)
     + α · KL(student_logits, teacher_logits)
     + β · hidden_mse(student_hidden, teacher_hidden)   # start with β = 0
```

---

## 9. Four-way (six-variant) diagnostic

Train/eval: (1) FP, (2) blocks-only, (3) `tok_emb`-only, (4) `head`-only,
(5) `tok_emb+head`, (6) all-big-matrices.

Report per variant: CE/perplexity · top-1 agreement w/ FP teacher · top-10 overlap ·
greedy sample · max logit Δ · cosine · file size · packed-reload parity holds.

Expected risk order: `tok_emb` easiest → block matrices moderate → **head highest** → all hardest.

---

## 10. Smallest smoke test
- fixed seed; 32–256 short sequences; short context; few steps
- cache FP teacher hidden states (enables the no-transformer-backward head path)
- train ONE component group at a time first
- first pass condition:
```
post-hoc ternary loss : bad
QAT ternary loss      : improves meaningfully
FP loss               : best or near-best
packed reload         : bit-exact vs ternary-debug QAT model
```

---

## 11. Engineering order

1. FP shadow storage + Adam state for selected Supra matrices (training mode).
2. Ternary forward wrappers reusing per-row absmean (already have the quantizer).
3. **Head backward only** (CE → STE into shadow head), transformer frozen, hidden cached.
4. **Run head-only QAT smoke** → compare vs post-hoc head. ← first real decision point.
5. `tok_emb` backward (frozen context).
6. Run `tok_emb`-only and `tok_emb+head` smokes.
7. MLP backward.
8. Attention backward (the big one).
9. Blocks-only joint QAT.
10. All-big-matrix joint QAT.
11. Export packed artifact (reuse `cce_supra_export_packed`).
12. Reload + prove bit-exact parity (reuse `cce_supra_load_packed`).
13. Compare quality vs FP / INT8 / post-hoc / QAT.

---

## 12. Acceptance criteria — Phase 4 (milestone 1)
- ≥1 Supra component group trains with ternary forward + STE
- loss decreases
- QAT beats post-hoc ternary on the same eval
- packed export/reload parity stays exact
- FP/INT8 baselines unchanged
- result labeled **smoke-test quality**, not production

---

## 13. Risks + mitigations
- **Trainer complexity** (Supra fwd is inference-only) → start head-only/embedding-only with
  frozen context (no transformer backward); defer attention backward until simpler paths prove out.
- **Head ternary quality** (every logit depends on it) → test head-only first; allow hybrid
  fallback (ternary blocks + ternary `tok_emb` + INT8 head) if head won't recover.
- **Slow trit runtime** → out of scope here; keep INT8 as the speed path; SIMD/threaded trit later.
- **Tiny-corpus overclaiming** → label smoke-test; require a real corpus before any deployable claim.

---

## 14. Decision

Do **not** start with full Supra QAT.

**First build: head-only QAT smoke test.**
Reason: the head is high-risk and structurally isolated as one large linear classifier, and
(per §4) it can be trained with the proven `cce_wordlm` STE pattern and **zero transformer
backward** by freezing the transformer and caching hidden states. If head-only QAT cannot beat
post-hoc, full all-ternary is unlikely without a better loss/wider setup — and we learn it cheaply.

**Second build: `tok_emb`-only QAT smoke test.**
Reason: embedding QAT already worked in `cce_wordlm`; fast sanity check for sparse STE updates.

Only after these two have a measured result do we invest in attention/MLP backward (joint QAT).

---

## 15. Milestone 1 RESULT — head-only QAT smoke (2026-06-28) — DONE

Built: `tests/supra_head_qat.c` (`make supra_head_qat`, 7/7). Core accessors added to
`cce_safetensors.c`: `cce_supra_hidden_last` (cache the final hidden vector, no head)
and `cce_supra_head_fp` (borrow the FP head matrix). Trainer reuses the proven
`cce_wordlm` ternary recipe (per-row absmean, `γ·{-1,0,1}`, STE: FP-shadow grad =
identity, activation grad = ternary). FP-path backward gradient-checked (8.6e-6).

**Methodology correction (important):** the scope's literal "next-token CE on a tiny
corpus" is **ill-posed at smoke scale** — a 12.9M-param head over ~75 inputs just
memorizes, so QAT trivially "beats" FP (overfitting), which is meaningless. The
well-posed milestone-1 question is **distillation: does QAT recover the FP head's
behavior under the ternary constraint, better than post-hoc?** Target = FP argmax.

**Result (75 pairs, 60 train / 15 held-out):**
- distill CE 1.38 → 0.0012 (machinery works; gradcheck'd).
- **In-sample FP-argmax agreement: post-hoc 42/60 (70%) → QAT 60/60 (100%).** QAT fully
  recovers the head behavior post-hoc loses. **Gate PASSED: QAT beats post-hoc on the head.**
- Held-out: post-hoc 10/15, QAT 10/15 (tie); QAT held-out CE-to-FP slightly worse
  (overfit). Reported, not asserted.

**Conclusion:** the QAT path is real (the STE machinery recovers the head). The smoke's
honest limit is **generalization needs a real corpus** (§13) — 60 inputs vs a 12.9M head
is memorization. Next: (a) `tok_emb`-only smoke, and/or (b) run head QAT on a real corpus
(reuse the PDF/tile corpus pipeline) to test held-out recovery before joint QAT.

---

## 16. Milestone 1b RESULT — head QAT on a REAL corpus (2026-06-28) — DONE

Built: `tests/supra_head_qat_corpus.c` (`make supra_head_qat_corpus`, 5/5). Reads the real
`pdf_corpus.txt` book; `cce_supra_hidden_all` (new accessor) caches a hidden at every
position (one forward → many pairs); WHOLE sentences are held out (every 8th) so eval is
truly unseen. Distill the FP head into the ternary head; metric = FP-argmax recovery on the
held-out split vs post-hoc.

**The objective decides generalization (key finding):**

| objective | TRAIN recovery | HELD-OUT recovery | vs post-hoc (held-out) |
|---|---|---|---|
| post-hoc (baseline) | 60.1% | 61.1% | — |
| **hard-argmax distill** | 98.0% | 59.6% | **loses** (overfit/memorized) |
| **soft KD (FP softmax)** | 85.3% | **64.5%** | **beats (+3.4%)** |

Hard-argmax is a 1-dim, easily-memorized target → the 12.9M head overfits the train hiddens
and does NOT transfer. Soft KD's ~50k-dim-per-example target resists memorization → the
ternary head learns a generalizing approximation of the FP function and **beats post-hoc on
unseen sentences**. (1235 train / 265 held-out pairs, 12 epochs.)

**Conclusion:** head-only QAT is VIABLE — it recovers FP head quality on held-out data
beyond post-hoc, *provided the loss is soft distribution distillation at LOW temperature.*

**Temperature sweep (held-out recovery, 2026-06-28):** T=1 → 64.5% (+3.4%); T=4 → 56.6%
(LOSES); 4000 pairs @ T=4 → 59.1% (still loses). **Temperature went the WRONG way: T>1
HURTS**, because the recovery metric is FP-argmax agreement and a hot teacher flattens the
distribution, de-emphasizing the argmax the student must match. **T=1 is correct.** More
data / epochs at T=1 is the only remaining head-only lever and the gain stays modest — i.e.
head-only QAT recovery is fundamentally bounded; **bigger recovery needs JOINT training**
(the rest of the network adapting), not more head data. The quality-phase path is open.
Next: `tok_emb`-only QAT, then joint (attention/MLP backward) using the same soft-KD recipe.

---

## 17. Steps 7–8 RESULT — the transformer backward (2026-07-03) — BUILT + GATED

Built: `src/cce/cce_supra_train.c` + `include/cce/cce_supra_train.h` +
`tests/test_supra_train.c` (`make supra_train`, in `make test`). Hand-rolled
reverse pass mirroring the verified Supra forward op-for-op (LayerNorm, fused
QKV, causal softmax attention, output proj, residuals, tanh-GELU MLP, final LN,
head, embedding scatter), following the proven `cce_wordlm` recipe: FP shadow
weights, per-OUTPUT-column absmean ternary forward (cce_block orientation, so
shadows export straight into the packed pipeline), STE backward, double-
accumulation dots, Adam. Per-group QAT knobs (`qkv/proj/mlp/head/emb`);
pos-emb frozen FP; LN params + biases FP but trained (§6/§7 policy).

Gate (hermetic, tiny synthetic model, no files): **directional-derivative
gradcheck over the whole parameter vector: rel 6.2e-4**; per-group max-|g|
central differences all < 5e-3; same-seed bit-identical logits; FP CE
3.087 → 0.0001; soft-KD QAT loss falls under ternary forward.

**Gradcheck methodology note:** naive per-random-param central differences
false-alarmed at ~2e-1 — float32 forward noise dominating near-zero individual
grads, NOT a backward bug. The directional derivative (signal = full gradient
norm) is the reliable primary check; per-param spot checks must sample the
largest-|grad| param per group.

Open (steps 9–13): joint QAT on the REAL Supra weights (loader from
`cce_supra_decomposed` shadows), soft-KD vs the FP teacher on the PDF corpus,
export via `cce_supra_export_packed` + parity, quality table vs FP/int8/post-hoc.
