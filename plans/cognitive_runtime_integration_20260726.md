# Cognitive Runtime Integration (Codex priorities + classification fix)

**Date:** 2026-07-26
**Source:** Codex roadmap in session after A-grade work
**Repo:** `/home/marble/AI/CNET`
**Do not push.** Local commits OK when green.

## Goals (all required)

1. **Capability certification** — machine-readable manifests + held-out runner; PRs/gates fail on silent claim PASS without evidence.
2. **Hybrid semantic cortex + shared workspace** — transformer residual (HTTP/hermetic) produces candidates/latents; CNET routes/verifies; elastic shared workspace for cross-specialist notes (not a second opaque LLM).
3. **Sleep-like memory consolidation** — episodic traces → semantic skills / consolidated tiles / graduated units with provenance.
4. **Calibrated abstention + provenance** — per-route reliability/margin thresholds; claim bundles name supporting memory/tool/specialist/model.
5. **Fix classification lane** — produce real `CLASSIFICATION_LANE_HEALTHY` (lift over majority + multi-class), not declared_open.

## Non-goals / traps

- Hundreds of specialists without shared representation.
- Training on CNET’s own answers without external validation.
- Shared workspace becoming another monolithic model.
- Optimizing throughput before nonzero task quality.
- Claiming AGI from routing infrastructure alone.
- CUDA-only paths (AMD/ROCm machine).

## Existing surfaces to reuse (do not rewrite)

| Need | Reuse |
|---|---|
| Hybrid tiers A/B/C | `hybrid_ai`, residual_http/gguf, personal_ai |
| Consolidation / graduate | `tilemem_consolidate`, `graduate_deterministic`, `consolidate_*` |
| Abstention / margins | `port_margin`, `acquire` Wilson, `hybrid_ai` soft min_margin, flagship |
| Provenance / evidence | `cnet_evidence_bundle`, CNB unit provenance, gap_lane teacher |
| Episodic memory | `agent_memory`, Cce.Llm MemorySession/KeywordIndex |
| Classification training | `cce_learn` classify=1, `CCE_BLOCK_LINEAR_HEAD`, EXACT + grad_clip |

## Implementation slices

### A. Classification lane fix (`tests/cce_train_bench.c` + learn path)

Claude diagnosis: MSE-on-soft-labels + sigmoid head + local credit → constant predictor.

**Required change:**
- Final block: `cce_cascade_add_linear_head` (logits, no sigmoid).
- `learner.classify = 1` (softmax CE).
- `learner.diff_mode = EXACT` (or documented path that works).
- `learner.grad_clip = 1.0f` (or env).
- Hard one-hot targets `{0,1}`.
- **Learnable label rule:** prefer linear-separable classes, e.g. class = argmax of first 4 input dims (or 4 well-separated Gaussian means). Drop the `((int)(sum*2)%4)` sawtooth if it remains unlearnable.
- Keep held-out eval N≥500, majority baseline, distinct classes.
- Success marker: `CLASSIFICATION_LANE_HEALTHY` and `CLASSIFICATION_GATE_PASS status=measured` under default env (no REQUIRE needed for green).
- Optional: if EXACT is expensive, document step count; keep bench under ~30s CPU.

Gate: `make cce_train_bench` → healthy classification markers; `make ci` still green.

### B. Capability certification

New (suggested paths — adjust if better fits tree):
- `include/cnet_capability_cert.h` + `src/cnet_capability_cert.c`
- `config/capability_manifests/*.json` (start with 3–5 caps)
- `tests/test_capability_cert.c` or Python runner + C helpers
- `make capability_cert` → `CAPABILITY_CERT_PASS`

Manifest fields (minimum):
- `id`, `title`, `owner`, `absolute_floor`, `regression_budget`
- `eval_command` or internal suite id
- `held_out_fixture` path (encrypted or plain under tests/fixtures)
- `evidence_artifact` path written on run
- `failure_envelope` string

Initial capabilities:
1. `cce_classification` — requires HEALTHY marker
2. `honest_memory_retrieval` — wire existing Cce.Llm anti-confab tests or a hermetic C proxy
3. `hybrid_skill_serve` — `make hybrid_ai` or subset checks
4. `calibrated_abstention` — margin abstain unit test
5. `sleep_consolidation` — sleep cycle gate

Runner emits `logs/capability_cert.json` with per-cap status + digests.

### C. Hybrid semantic cortex + shared workspace

Suggested:
- `include/cnet_shared_workspace.h` + `src/cnet_shared_workspace.c`
  - Fixed-cap ring of entries: `{kind, source_tag, text_or_latent_ref, score, ts}`
  - Ops: `push`, `query_keyword` / recent-k, `clear`, `snapshot_digest`
  - No network; hermetic.
- `include/cnet_semantic_cortex.h` + `src/cnet_semantic_cortex.c`
  - Backend enum: HERMETIC | RESIDUAL_HTTP
  - `propose_candidates(query)` → fills workspace with residual top-k window tokens or hermetic bag-of-chars embedding + dummy candidates
  - CNET remains authority: cortex proposals are uncertified until hybrid/soul admits

Gates:
- `make shared_workspace` → `SHARED_WORKSPACE_PASS`
- `make semantic_cortex` → `SEMANTIC_CORTEX_PASS` (hermetic; HTTP optional smoke)

Wire one path into hybrid or personal_ai smoke if cheap; else keep standalone + docs.

### D. Sleep-like consolidation

Suggested:
- `include/cnet_sleep_consolidate.h` + `src/cnet_sleep_consolidate.c`
- Input: episodic file or agent_memory export / synthetic episodes
- Steps:
  1. Ingest episodes into tile memory / keyword blobs
  2. `tilemem_consolidate` (or equivalent)
  3. Optional `graduate_deterministic` when regularity found
  4. Write evidence bundle / provenance note for outputs
  5. Prune redundant episodic entries (count reported)

Gate: `make sleep_consolidate` → `SLEEP_CONSOLIDATE_PASS`
Docs: one paragraph in README or plans pointing to episodic→semantic→procedural.

### E. Calibrated abstention + provenance binding

Suggested:
- Extend or add thin API:
  - `cnet_route_abstain(route_id, margin, threshold, reliability)` → ANSWER | ABSTAIN
  - thresholds table loadable from JSON (default safe)
  - `cnet_claim_bind(claim, evidence_refs[])` wrapping evidence_bundle fields
- Tests: low margin abstains; high margin answers; claim requires ≥1 evidence ref

Gate: `make calibrated_governance` → `CALIBRATED_GOVERNANCE_PASS`
Or fold into capability_cert / evidence_bundle if cleaner.

### F. Umbrella

`make cognitive_runtime` runs A–E gates and prints `COGNITIVE_RUNTIME_PASS`.
Add to README table + short CHANGELOG row.
Do **not** make full `make ci` depend on HTTP teacher or live GPU beyond existing ci_rocm optional.

## Verification checklist

- [x] `CLASSIFICATION_LANE_HEALTHY` on default `cce_train_bench`
- [x] `make capability_cert` PASS with JSON evidence
- [x] shared workspace + semantic cortex hermetic PASS
- [x] sleep consolidate PASS with merge/graduate counts
- [x] abstention/provenance PASS
- [x] `make cognitive_runtime` PASS
- [x] existing `make test` / critical suites not regressed (run what you touch + `cce_train_bench` + new gates)
- [x] no CUDA-only; no secrets; no push
- [x] no commit of `cce.dll`/`cnet.so` noise unless intentional PORTABLE rebuild chore separate

## Deliverable report structure

1. Changes (files)
2. How each Codex priority maps to code
3. Tests + markers
4. Classification before/after numbers
5. Remaining risks
6. Commit SHAs
