# Model Merge Scope — hybrid, non-monolithic composition of decomposed models

Status: SCOPING (no merge code yet). Date: 2026-07-03.
Origin: "if I can merge models, making it hybrid while still being
non-monolithic after conversion, it's a win for everyone."

---

## 1. The claim triage (grounded in the tree, not the pitch)

The pitch names four mechanisms as "already there." Verified against source:

| claimed | reality |
|---|---|
| content-addressed specialist identity (`cce_specgraph`) | **REAL** — digests + behavioral fingerprints, cross-container identity gated |
| epsilon-merge behind evidence (`cce_similar`) | **REAL but same-shape only** — `cce_similar.c:19` skips any pair with `in_dim/out_dim` mismatch; adversarial battery + refusal gated |
| content-addressed store + manifests (`cce_wstore`) | **REAL** — 100% reuse on re-ingest, fine-tune = 1 payload, bit-identical restore, all gated |
| "SSMax router selects the right specialists per query" | **REAL for CCE-learn forests** (`cce_router.c`: 0.7·centroid-sim + 0.3·goodness, temp+top-k) — but **decomposed transformers do NOT run through it**: their forward calls branches by fixed name (`qwen2.blk.N.q_proj`). Free per-query routing over a merged library is NOT how converted models execute today. |

Two claims in the pitch do not survive contact:

- **"LLM + VLM share early-layer specialists → dedup"** — weight-level
  sharing between independently-pretrained different-architecture models is
  ~zero (different shapes alone disqualify under `cce_similar`), and
  behavioral similarity does not make weights substitutable across bases.
  The 70B+70B → "~160 GB" table row is not credible. The RAM row (~16 GB
  tier-streamed) IS credible — but it comes from the tier runtime, not from
  merging.
- **"Bit-identical to originals, router-selected"** — holds only at
  MODEL-granularity routing (a query executes one original sub-library
  end-to-end). Mixing specialists across models inside one forward pass
  forfeits bit-identity and is unproven research.

The strongest case needs neither: **fine-tune families**. Same base, same
shapes, mostly-identical layers → exact-digest dedup for untouched layers,
evidence-gated epsilon-merge for near-identical ones, per-model manifests,
one store, tier-streamed serving. Every mechanism exists and is gated today.
(Monolith-world comparison to be honest about: multi-LoRA serving already
dedups base weights — but only when the deltas were TRAINED as adapters.
CNET's dedup is discovered post-hoc on full fine-tunes, and the epsilon
merge is verified, not hoped.)

## 2. Staged plan (each stage a gate, cheapest decisive experiment first)

### M0 — fine-tune-family merge (the enterprise case; ~all existing machinery)
Compose `cce_wstore` + `cce_similar` + `cce_tier_runtime` into one pipeline +
gate (`make merge_family`):
1. Ingest tiny base + N synthetic fine-tunes (perturb K specialists each —
   the wstore gate already does this for N=1).
2. Gates: storage = base + Σ small diffs (measured vs N× naive);
   each manifest's streamed forward BIT-identical to its standalone model;
   epsilon-merge dedups a near-identical fine-tune layer behind the probe
   battery and REFUSES a material one; the whole library serves under one
   HOT cap.
3. Report the honest dedup ratio + RAM measurements.
This is a composition test of proven parts, not new research. Highest
value-per-effort in the whole pitch.

### M1 — model-granularity hybrid catalog (different archs, one store)
Ingest two genuinely different tiny models (the tiny-llama fixture and the
mamba SSM fixture — both loaders exist and are gated) into ONE store with a
catalog + query-level selector (task tag → manifest; the contract router's
typed tags are the natural selector, not SSMax).
Gates: per-domain outputs bit-identical to standalone; single-store
accounting; and an HONESTY gate — measure and REPORT the actual cross-model
dedup (expected ≈ 0 for different archs; the point is to publish the truth,
not the fantasy).

### M2 — bridging specialists (research; toy scale only on this hardware)
The genuinely new capability: a typed-port contract unit mapping model-A
representation → model-B-consumable representation, acquired by the
gap-triggered loop (router detects the missing edge, oracle mines pairs,
train, certify SAMPLED + conformal reject, seal, register, replan). The DAG
planner composing image→features→text chains through typed ports is the
CNET-native version of "multimodal glue."
Reality check: a USEFUL vision→language bridge at modern-model scale is a
training project (this is what LLaVA-class projectors are) and does not fit
one 4070 Ti S. The honest first experiment is Supra-scale: bridge a tiny
second modality (e.g. the perceptual-leaf glyph domain, already in-tree)
into Supra hidden space, certified on the fuzzy tier. Mechanism proof, not
deployable quality — label it as such.

### Explicitly deferred / rejected
- Cross-architecture specialist DEDUP by behavioral alignment: rejected as a
  merge mechanism (weights aren't substitutable across bases); MAYBE later as
  a diagnostic ("these two libraries have behaviorally-overlapping members")
  via cross-arch probe adapters — which is itself the M2 bridging problem.
- Any 70B-scale claims: nothing on this box can verify them; do not print
  numbers we cannot gate.
- Free specialist-granularity routing across merged transformer libraries:
  forfeits bit-identity; revisit only after M0-M2 hold.

## 3. Order of work
M0 first (days, decisive, sellable), M1 second (small), M2 as the research
track behind them. Each lands as one `make` gate wired into `verify` +
`tests/verify_logs.sh`, same as every other capability in the tree.

---

## 4. M0 RESULT (2026-07-03) — BUILT + GATED

`tests/merge_family_test.c` (`make merge_family`, in `make test`): base + 3
fine-tunes (material gate[1] / epsilon gate[1] / material q[0]+down[1]) in ONE
store. 37 checks:
- ingest accounting exact: fine-tunes cost 1/1/2 new payloads respectively
- storage: family store 32% of naive 4x (3.12x smaller) at toy scale — the
  ratio only improves with model depth (diffs are a large fraction of a
  2-layer fixture)
- every member restores + tier-streams from the shared store BIT-identical
  to its standalone self under hot_cap=8 (high water never exceeds cap)
- epsilon member merges to canonical ONLY behind the 32-probe battery
  (max_rel_dev 1.37e-3), and its post-merge streamed forward is bit-identical
  to the base; the material member fails the signature filter, fails the
  battery, and its merge is REFUSED
M1 (hybrid catalog) is next; M2 (bridging) behind it.

---

## 5. M1 RESULT (2026-07-03) — BUILT + GATED

`tests/hybrid_catalog_test.c` (`make hybrid_catalog`, in `make test`):
tiny-llama transformer + tiny-mamba SSM in ONE store. 22 checks:
- NEW `cce_weight_store_restore_ssm` closes the header's stated limit
  ("ssm restore is a mechanical follow-up"): store round-trip |diff|max = 0
- catalog file (task -> family + manifest) selects per query; both selected
  models serve from the shared store BIT-identical to standalone; unknown
  tasks refuse; the WRONG family's restorer refuses the other's manifest
- HONESTY gate, measured and published: mamba reused 0 of 21 payloads
  against the llama store — cross-architecture dedup is exactly zero, as
  section 1 predicted. The hybrid win is one store + one runtime + verified
  per-domain identity, NOT cross-model weight sharing.
M2 (bridging specialists, toy scale, fuzzy-tier certified) is the remaining
research track.
