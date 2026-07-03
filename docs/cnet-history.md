# CNET History (v0.5.3 – v2.9)

**This file contains the detailed changelog for the mechanism/harness work.**
The main README now focuses on the thesis and the current open question.

## v0.5.3 – Attention Planner (No Synthetic Dashboard Overrides)
Removed forced/override constants. Pure row-derived predicates.

## v2.0 – Circuit Memory Hints (SHADOW_ONLY)
Typed execution evidence from strict blackboard. Never authority.

## v2.1 – Typed Engram Cache (SHADOW_ONLY)
Deterministic cache of verified facts. influence_on_planner always 0 in shadow.

## v2.2 – Frozen GRPO Rank Artifact (ORDER_ONLY)
Built from engrams + GRPO into immutable rank prior. Only traversal bias for full set.

## v2.3 / v2.3.1 – Rank Artifact Evaluation + Measurement Honesty
nodes_expanded from real search, rank movement, strict predicate:
`reduction_claimed = artifact_influenced && rank_improved && delta < 0 && all same_*`

Honest 0 when conditions not met. No forcing.

## v2.3.2 – Natural Search-Sensitive Fixtures
Benign (good prior) and lure-heavy (bad prior) cases. 0/Y on small fixtures.

## v2.3.3 – Search-Sensitive Positive Fixture
Scaled lures (16/8). Honest 0 until delta appears naturally.

## v2.3.4.1 / v2.3.4.2 – Scale Bump + Deeper Dead-Branch
Pinned good-pair rank movement with explicit fields. LURE_N=64 + dead_branch_depth=3.
First positive reduction_claimed row on benign (1/5).

## v2.4 – Formula IR Seed / Typed Spreadsheet Micro-Domain
Tiny typed ports (cell_ref, const_num, formula_value, ...). Synthetic BTNs.
Proved mechanism transfer to symbolic domain. Same machinery, same predicates.

## v2.5 – Evidence-Derived Formula Rank Artifact
Strict verified → blackboard → GRPO/engram → build_from_engrams → frozen reload.
"artifact source = strict verified engram/GRPO row"

## v2.6 – Formula Composition Depth
>=3 composed primitives in one DAG under the evidence loop (no parser yet).

## v2.7 – Formula Contracts / Laws (Evidence Only)
Typed regression laws (commutativity, identity, composition equivalence).
Finite domain enumeration + strict execution on both sides.
Laws do **not** certify, rank, register, prune, or mutate.

## v2.8 – Tiny Validated Parser Boundary
Closed tiny grammar → finite typed IR (no authority).
Parser refuses invalid syntax / unknown cells before planner.

## v2.8.1 – Parser Refusal / Canonicalization Hardening
Explicit policies for whitespace, case, leading zeros, integer bounds, parens depth,
trailing garbage, unsupported operators/functions. Deterministic errors.

## v2.9 – Formula Task Corpus
7 accepted + 7 refused formulas. All accepted paths go through parser → validate
→ normal planner → strict exec → match expected. Report shows planned/strict_ok/match
and parser_authority=0. Negative corpus refused cleanly.

**End of mechanism thread (harness quality validated).**
Next work is the learned-leaf habitat probe (see main README).

## v3.0 – Learned Leaf Habitat Probe (glyph)
Real BTN leaf on noisy 5x7 glyphs → onehot10 / dec_digit typed port.
Four paths (oracle, canonical snap+margin, raw argmax, margin-reject).
Key: formula_err_given_leaf_confident_correct = 0.

## v3.0.4 – Selective-Risk Frontier
Stress grid over noise × margin_floor.
CSV + per-noise best + decision table (selective vs safety boundary).

## v3.1 – Improve Leaf Only (no mechanism change)
Larger hidden (30), noise aug during train (0.2-0.5), 5000 samples.
Same frontier/metrics/CSV schema. Coverage improved at high acc_f.
Wrote separate v3_1_frontier.csv; v3_0_4 kept as baseline.

## v3.2 – Multi-Leaf Formula Depth
Multiple independent glyph leaves compose in one expression:
A+B, A*B, (A+B)*C.
Added: leaf_count, all_leaves_confident, all_leaves_confident_correct,
formula_err_given_all_confident_correct,
coverage/acc/raw/can by leaf_count.
Result: formula_err_given_all_confident_correct = 0 exactly.
As lc increases coverage drops predictably; accepted acc stays high;
errors only from bad leaves, never from clean symbol composition.

## v3.3 – Learned Glyph Leaf as Real Typed Primitive
Wrapped the trained BTN as a normal CNET primitive:
- btn_set_io_ports: RAW "noisy_glyph" -> ONEHOT 10 "dec_symbol"
- Execution uses real route_execute + port_validate / port_canonicalize / port_margin
- Margin-floor reject aborts exactly like other unclean handoffs
- Canonical output identical to old ad-hoc snap
- Two- and three-leaf formulas composed by executing leaves through the executor,
  then pure formula on the snapped symbols: still formula_err_given_all... = 0
- All checks: GlyphLeafPrimitive_* pass. No planner used, no metric mutation.
Legacy v3.2 grid remains the unchanged habitat baseline.

## v3.4 – Planner-Visible Glyph Formula Path
glyph_leaf (typed ports) is registered and visible to the planner.
- route_plan + dag_plan_circuit discover paths: raw-glyph sources → glyph_leaf → dec_symbol goals.
- Multi-leaf "formulas" (A+B, (A+B)*C) assembled by planning multiple glyph applications over multiple raw sources; execution produces canonical symbols via the planned DAG.
- Pure checked formula math applied only to the symbols obtained from planned execution.
- Margin reject still enforced on leaf outputs before accepting formula result (planner does not bypass).
- All GlyphPlanner_* pass; formula_err... remains 0; no raw leakage, no semantics invented by planner, baseline metrics untouched.
This completes the arc: learned leaf → typed boundary → executor → planner composition.

## v3.5 – Planned Glyph Formula Depth Grid
Stress grid (same noises/floors, mixed lc=2/3 formulas) executed entirely through planner:
registry + dag_plan_circuit (over declared ports) + dag_execute_circuit.
- Separate "planned" metrics reported (cov by lc, acc_f on accepted, err on cc, raw).
- Planned path preserves shape: cov drops with lc, formula_err_on_cc = 0 (8688+ cases).
- All GlyphPlannedGrid_* checks pass.
- Old v3.2 direct baseline + CSVs untouched.
- Demonstrates: planner adds routing but no extra risk past the typed wall.

## v3.6 – Planned Glyph Grid Report / Artifact (read-only evidence)
After the v3.5 planned grid, write two artifacts:
- artifacts/glyph_habitat/v3_5_planned_grid_report.json
- artifacts/glyph_habitat/v3_5_planned_grid_summary.txt
Content records baseline + planned metrics, by-leaf-count, per-noise bests, and the formula-err invariant.
- Written after all computation; never read by planner/executor/registry.
- Missing/unwritable = non-fatal.
- Forged data = zero authority.
- Old v3_0_4/v3_1 CSVs untouched.
- All GlyphPlannedReport_* checks pass.
- No training or metric mutation.

## v3.7 – Glyph Leaf Contract + Margin-Floor Certification
The learned glyph leaf is now gated by an explicit contract:
- signature RAW noisy_glyph → ONEHOT dec_symbol
- finite low-noise exemplar set (canonical onehots)
- btn_certify_robust with margin_floor for headroom
- registry_add_certified + require_certified mode
- Certified leaf accepted in certified-mode planning; uncertified rejected.
- All GlyphContract_* checks pass.
- Planned grid evidence unchanged (0 err on cc leaves).
- Contract file emitted but has no runtime authority beyond the gate at registration time.

## v3.8 – Glyph Formula Property Contract / End-to-End Law
Certifies the end-to-end composition law over planned certified glyphs:
when all leaves yield confident-correct dec_symbol via the typed path,
the pure formula on the planned symbols equals ground-truth formula on the original digits
(for +, *, (A+B)*C).
- Law only evaluated on accepted symbols (low-margin cases are abstentions).
- Uses certified registry, margin gate before law eval, planner for glyphs.
- All GlyphFormulaLaw_* checks pass.
- No increase in artifact or planner authority.
- Preserves all prior evidence (including 0 semantic error on cc cases).

## v3.9 – Glyph Arc Closure / Regression Matrix
Compact regression closure for the entire v3.2–v3.8 arc.
Re-verifies all key invariants in one place:
- Baseline and planned 0 semantic error counts.
- Report artifacts still present and read-only.
- Leaf contract still certifies; uncertified rejected in certified mode.
- Formula law still holds.
- Low-margin still abstains.
- No authority leaks across artifacts, contracts, or planner.
Prints a summary matrix. All 8 GlyphArcClosure_* checks pass.
Arc is now frozen for future work.

## v4.0 – Second Perceptual Domain Transfer (7-segment / LED)
The complete boundary pattern transfers:
- New RAW input domain (7 noisy segments "noisy_7seg")
- Separate trained BTN
- Same output port type/tag ("dec_symbol")
- Contract + robust cert + certified registry
- Planner routes new sources to symbol
- Formula law holds on confident-correct outputs from the new leaf
- All transfer checks pass.
Demonstrates the CNET typed boundary is not glyph-specific.

## v4.1 – Mixed-Domain Composition + Claim Audit
- Mixed glyph + 7seg in same formulas (A+B, etc.) through shared dec_symbol.
- Load-bearing readout tables surface accepted_symbol_error (confident-wrong leak) per noise/floor and depth.
- Headline f_err_conf_corr demoted to handoff_sanity invariant.
- Brutal PASS/FAIL now centers on low accepted_symbol_error at useful coverage, and depth not amplifying leaks.
- Corrected claim: typed ports convert uncertainty to abstention; the leak rate among accepted is the key safety signal.

## v4.2 — Accepted-Leak Frontier Artifact
- Persisted row-derived CSV/JSON frontier (noise, floor, coverage, accepted_symbol_error, accepted_formula_error, leaf_count, coverage_depth_est, leak_depth_est).
- Compact printed frontier table.
- Recommended operating point derived from explicit row predicates only (e.g. sym_err <1% at cov>50%).
- No synthetic values, v4.1 mixed checks unchanged.
- README headline now references the frontier.

## v4.2.1 — Frontier Denominator + Scale Sanity
- coverage_depth_est / leak_depth_est stored as normalized fractions (0-1), matching "computed from symbol_coverage".
- All other v4.2.1 hardening (raw counters, [0,1] checks, v4_2_* rename) preserved.
- Frontier now fully frozen for v4.3+ scale.

## v4.3 — Third Domain + Frontier Comparison (with v4.3.1 provenance/strat)
- Added noisy 3x5 grid leaf.
- Frontier now written as v4_3_* with provenance (schema, run_version, leaf_set, domain_count, seed).
- Stratified per-leaf_type symbol counters and errors (in CSV rows with leaf_type, and per-type print).
- Per-cell mix-pair fml error matrix printed for lc=2 combinations.
- Note in artifact: formula stats in per-type rows are mixed-context (not purely type-local).
- Aggregate + per-domain + mix pairs.
- Per-type shows tail differences; mix pairs show which compositions contribute to error.
- Answers: err stays small; tail differs by domain; mixed preserves; specific pairs visible. v4_2 preserved as 2-domain baseline.

Current strongest claim:
The typed boundary is now audited by accepted-symbol leak rate, not by the sanity-zero. Mixed learned leaves compose through the shared symbolic port when accepted; uncertainty mostly becomes abstention; the remaining risk is the confident-wrong accepted tail, tracked directly.

## v4.3 — Third Domain Transfer + Frontier Comparison
- Added third perceptual leaf (noisy 3x5 grid, 15-feat RAW "noisy_grid" → dec_symbol).
- Frontier artifact used directly as comparison surface.
- With 3 domains mixed randomly per leaf:
  - accepted_symbol_error stays small (~0) at useful cov.
  - confident-wrong tail remains low (differs little by domain).
  - mixed composition preserves the leak/coverage frontier (no amplification).
- Answers the scale questions using the v4.2 artifact without new metrics.

The v4.2 frontier is now frozen.

## v4.4 — Fourth Domain Transfer
- Added low-res 4x4 block/silhouette digits (16-feat RAW "noisy_4x4" → dec_symbol).
- v4_4_accepted_leak_frontier.* using exact same schema, per-leaf counters, mix-pair matrix, provenance.
- 4 domains (glyph,7seg,grid,block) randomly mixed in sweep.
- Frontier comparison confirms:
  - accepted_symbol_error stays small/bounded at useful coverage across all 4.
  - confident-wrong tail visible per domain; no single domain dominates the mixed budget.
  - mix pairs identify bad compositions.
  - adding fourth domain preserves overall frontier shape.
- No new headline metrics; v4_2 baseline + v4_3 3-domain surface reused.

Current strongest claim:
Three (now four) learned perceptual domains compose through the same finite dec_symbol boundary. The accepted-leak frontier (v4_2 baseline + v4_3/v4_4 runs) is audited with raw counters, per-domain leak rates, and mix-pair formula contributions. The boundary converts uncertainty into abstention; remaining risk is confident-wrong accepted symbols, now visible by leaf family and by composition pair. No new domain silently dominates.

## post-v4.4 — Residue study: set-theoretic held-out disjointness
`residue_study.c` now samples the flat monolith's training strings from `b^N` **minus** the
held-out set (bitset over the 65536-string N=8 space), replacing the prior RNG-stream-only
separation. The old scheme had ~7% expected train/held-out overlap at N=8 (generous to flat); the
fix makes "held-out" literally leakage-free. Measured post-fix (same build, zero leakage, 17181
distinct held-out strings cleared): composed 100% (20000/20000), flat **95.2%** at N=8 — still
mid-90s%, so the qualitative claim holds. The leak-attributable delta is *not* cleanly quantified:
no same-build pre-fix run was captured, and the remembered leaky figure (~96.4%) was a different
build/seed, so any "shift" estimate is a model, not a measurement; the direction (clean ≤ leaky) is
as expected. The gate (`test_residue.c`) is unaffected — it verifies the
deterministic certified scan against ground truth, with no trained baseline and so no disjointness
to enforce. The claim-repositioning record this relates to is
`docs/superpowers/specs/2026-06-18-residue-reach-claim-repositioning.md` (deliberately unchanged:
the headline number is non-load-bearing there).

## 2026-06 → 2026-07 — The CCE / Autonomy Arc (dated log)

The dated milestone log for the CCE runtime, universal model layer, and
autonomy loop. Current state (undated) lives in the README's *Status &
Caveats*; this is the chronology.

### 2026-06 — CCE depth + real generation
- Branches/leaves + perceptual sub-forests; deeper cascades (4-block default);
  patch blocks; header-based archive dir; zero-copy WARM cascade views.
- TinyStories generation fixed (5 causes: skipped samples, MSE→CE, word-level
  mode, decoder/LR sanity, 13× Adam hoist); word-level free-running sentences.
- O(V·d) factored word-LM head (773× smaller @50k vocab, exact-gradient
  trained, gradcheck-verified).
- Real model compression: 119 MB HF Supra decomposed to CNET specialists,
  bit-exact pure-C forward, ~256 tok/s, int8 PTQ (16.3 MB, near-lossless),
  6.95 MB packed 1.6-bit artifact (storage bit-exact; quality gated on QAT).
  BitNet b1.58 QAT proven in cce_wordlm (ΔNLL=0 packed reload).

### 2026-07 — Universal layer, hardening, autonomy
- **Universal model layer:** structural autodetect (format sniff + arch
  fingerprint), universal runners (GGUF + HF-llama safetensors, mamba-1 SSM),
  content-addressed specialist identity + weight store (models = manifests),
  bounded-RAM tier streaming (bit-identical under HOT cap), evidence-gated
  SIMILAR_TO merge. Found + fixed: GGUF dims are ne-order (reverse of torch).
- **Contract hardening:** behavior-only digests, certification cache, sealed
  v2 contract files (tamper-refused), cert-to-weights binding + audit
  demotion, one-file sealed .cnu units (3.0× smaller) in registry_save.
- **Autonomy loop:** gap-triggered acquisition (ledger → oracle → train →
  certify → seal → register → replan, DEFER-total), unified CNB1 base with
  mint-once tag governance, thermal-governed flagship harness.
- **2026-07-03 — flagship campaign complete:** 256/256 slices extracted from
  gemma MTP at 100%, 0 deferrals, ~7h CPU, verified three ways (recertify /
  audit / digest-identical re-mine); 13 MB base vs 465 MB source.
- **Fuzzy tier:** sampled-tier extraction (Wilson floors + split-conformal
  probe), ranked-preference ("soul") units, pilot-scheduled mining (16×
  cheaper refusal of degenerate slices). Fixed rc-vs-verdict drain bug that
  deferred every SAMPLED cert.
- **GPU forward:** self-contained OpenCL (cce_clgemm, no SDK/CUDA toolkit),
  11.9× with bit-identical logits, equivalence-gated (gpu_equiv, CNET_GPU=1).
  Recorded honestly: the old CCE_USE_CUDA path never compiled (Makefile typo).
- **2026-07-03 — router restoration:** the June "SRP split" of src/router.c
  had silently replaced the DAG planner/executor/blackboard/engram/
  rank-artifact machinery (~3,500 lines) with stubs; the 51 legacy test
  failures + segfault were that missing code, misfiled as rot. Restored from
  pre-split history into src/router/dag_full.c (registry/route keep the newer
  digest-audit features); a dropped expand_in_low condition in entry_usable
  also restored. Full legacy test_all green, now a verify gate (legacy), plus
  an allocation-balance leak gate (leakcheck, --wrap, CRT-baseline-aware).
