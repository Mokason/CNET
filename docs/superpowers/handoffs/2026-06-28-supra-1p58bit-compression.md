# Session Handoff — Supra 1.6-bit Compression Arc (2026-06-28)

How to resume: read this + the memory files (`memory/supra-model-cnet-integration.md`,
`memory/mingw-avx-struct-copy-segfault.md`, `memory/make-test-dag-plan-crash.md`,
`memory/no-git-commits-on-cnet.md`). **No git on CNET** — verify via filesystem, run no git commands.

---

## TL;DR — where we are

Started from "run full analysis on the Supra mock test from HF." Ended with a full real-model
compression arc:

> A pretrained 119 MB HF transformer (`SupraLabs/Supra-A2A-Nano-Exp`) is decomposed into CNET
> specialists, run with a **bit-exact** pure-C forward, sped up **4.7 → 256 tok/s**, and compressed
> down a verified ladder to an **actual 6.95 MB reloadable 1.6-bit packed-ternary artifact**.
> **BitNet b1.58 QAT is proven** (in `cce_wordlm`). **Supra QAT is scoped but NOT built** — that is
> the next step.

The storage/format phase is **closed and proven**. The open phase is **quality (Supra QAT)**.

---

## The compression ladder (all measured, not projected)

| representation | size | quality |
|---|---|---|
| Original HF checkpoint | 118.9 MB | — |
| CCE FP specialists | 64.4 MB | exact forward (bit-exact vs numpy ref) |
| **int8 weight-only PTQ** | **16.3 MB** | near-lossless (cosine 0.99999, greedy 64/64) |
| post-hoc ternary | ~6 MB | **collapses** (cosine 0.83, argmax 5811→4077) |
| **packed 1.6-bit (trits)** | **6.95 MB actual on disk** | **bit-exact vs ternary** (storage proven; quality = QAT) |

---

## What was built this session (all working-tree only; no git)

1. **Fixed the Supra/safetensors path** (`src/cce/cce_safetensors.c`):
   - `__metadata__` header-parse blocker (real HF files now load).
   - **download-once cache** → `./supra_cache/` (model 118 MB + vqvae + tokenizer reused every session).
   - rewrote the broken inference path: sigmoid→`LINEAR_HEAD`, load biases, per-row linear application,
     MLP intermediate sizing, attention `kht` shape `[head_dim,T]`, head last-row-only, `sample_next_token`
     top-k id bug (returned array index → always token 0 `'!'`), real GPT-2 byte-level **BPE tokenizer**
     (vocab+merges+contractions), VQ shadowed-`d`, VQ stack→heap.
   - **Verified:** forward matches numpy/float64 **bit-exact** (top-8 ids, layer checksums); tokenizer
     **6/6** vs HF `tokenizers` + 6/6 round-trip; coherent greedy decode.
2. **Speed → 256 tok/s, flat O(T):** vectorizable mat-vec in `cce_block` (i-outer/o-inner, bit-identical);
   AVX re-enabled for CCE/Supra make targets (router keeps `-mno-avx`); **KV cache** (`kv_step` in
   cce_safetensors.c, greedy 64/64 identical to batched); OpenMP over the 50520-wide head.
3. **int8 PTQ** — `cce_block_quantize_int8`, `cce_supra_quantize_int8`, `--int8` flag.
4. **BitNet b1.58 ternary:**
   - post-hoc on Supra **collapses** (proven). QAT is mandatory.
   - **QAT proven in `cce_wordlm`** (`make wordlm_bitnet`): FP ppl 1.41 / QAT 1.95 (coherent) / post-hoc 61.5.
     Four-way (linear/embedding/both) → cost is in the *linear* matrices; embedding ternary ~free.
   - `make bitnet_qat` = self-contained STE demo (ring: FP 99.8 / QAT 82.2 / post-hoc 64.2).
5. **Trit-packing (5/byte, base-3, 1.6-bit):** `cce_block_pack_trits`, `cce_supra_pack_trits`,
   `cce_wordlm` packed export/reload, packed `tok_emb` (`supra_embed_row`).
6. **On-disk packed Supra artifact:** `cce_supra_export_packed` / `cce_supra_load_packed`
   (rebuilds HOT forest branches with `add_packed_branch`, NULL FP weights, reuses the verified forward).
   **6.95 MB actual file, reloads bit-exact** (`max|Δlogit|=0`), no FP big tensors.
7. **Supra QAT scoping doc:** `docs/superpowers/specs/2026-06-28-supra-qat-scope.md`.
8. **README updated** (new "Real Model Compression" section + make targets + repo layout).

---

## Key files / functions

- `src/cce/cce_safetensors.c` — Supra loader, `cce_supra_gpt_forward`, KV cache (`kv_step`), tokenizer,
  `cce_supra_quantize_int8/ternary`, `cce_supra_pack_trits`, `supra_embed_row`,
  `cce_supra_export_packed` / `cce_supra_load_packed` / `add_packed_branch`.
- `src/cce/cce_block.c` / `.h` — int8 + trit forward paths; `cce_block_quantize_int8`,
  `cce_block_quantize_ternary`, `cce_block_pack_trits`; fields `w_q/w_scale/w_trit/w_trit_bpr`.
- `src/cce/cce_wordlm.c` / `.h` — `cce_wordlm_set_ternary`, `cce_wordlm_set_ternary_embed`,
  `cce_wordlm_export_trits`, `cce_wordlm_packed_*` (BitLinear QAT + trit pack/reload; FP path unchanged,
  gradcheck still 1.19e-2).
- Make targets: `supra_console` (`--int8`, `--mode text/chat`), `cce_safetensors_test`, `bitnet_qat`,
  `wordlm_bitnet`. Build flags for these: `-march=native -fopenmp` (AVX on; they don't link the router).
- `supra_cache/` holds the downloaded model — already cached (don't re-download).

---

## Verification status (green)

- forward bit-exact vs numpy ✓ · tokenizer 6/6 HF ✓ · int8 cosine 0.99999 / greedy 64/64 ✓
- packed Supra reload `max|Δlogit|=0` ✓ · wordlm packed reload `ΔNLL=0` ✓
- `make cce_safetensors_test` PASSES ✓ · FP `make wordlm` gradcheck 1.19e-2 ✓
- `cce_safetensors.c` / `cce_block.c` / `cce_wordlm.c` warning-clean ✓

---

## Honest caveats / known issues

- **Supra ternary is POST-HOC → quality collapsed** (argmax 5811→4077). Packing is bit-exact; *quality* needs QAT.
- **trit runtime kernel** is correct but **not SIMD/threaded** (smallest, slowest). Keep int8 as the speed path; SIMD-trit is a deferred optimization.
- **Pre-existing, UNRELATED:** `make test` (`test_all`) segfaults in the `dag_plan` fallback. It does **not** link the CCE stack (deps are `$(SRC) $(ROUTER) $(CONTRACT)…`, not `$(CCE)`), so it's independent of all this work. Use CCE-specific targets to validate CCE.
- README's "`make test` builds and passes" line was left as-is (pre-existing claim about the router binary; observed crash is on this machine only and out of scope here).

---

## NEXT STEP — build the head-only QAT smoke test

Scope is done (the spec above). The recommended first build, which needs **ZERO transformer backward**:

1. Freeze the FP Supra transformer. For each training example, run `cce_supra_gpt_forward` to the final
   hidden vector `h = ln_f(blocks(embed(tokens)))[last]` and **cache it**.
2. You now have `(h, target_token)` pairs — a standalone linear-classifier problem (same shape as
   `cce_wordlm`'s class/word head).
3. Train a ternary BitLinear head: FP shadow `W_head[vocab,hid]` + per-row absmean ternary forward + STE,
   CE loss. Reuse the **proven `cce_wordlm` STE pattern**.
4. Compare **QAT head vs post-hoc head** on the same eval. Pass = QAT clearly beats post-hoc; loss decreases;
   packed reload still bit-exact.

The head is highest-risk (every logit depends on it) but structurally isolated (single `cce_block`
`gpt.head` `[256→50520]`, already run last-row-only). If head-only QAT can't beat post-hoc, full all-ternary
won't without a better loss — and we learn it cheaply. Second build: `tok_emb`-only QAT. Only after those
return a signal do we invest in attention/MLP backward (joint QAT).
