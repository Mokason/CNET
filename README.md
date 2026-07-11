# CNET — Compositional Neural Primitives in C

**CNET tests one idea:**

> Neural networks can compound knowledge the way software does — **when the
> intermediate representations are finite, typed, and recoverable.**

The pure-C realization of this idea is the **Contract Cascade Engine (CCE)** — a forest of contracted specialists (branches) built from leaf blocks (linear + patch), with partial mmap loading, SSMax routing, goodness-gated learning (deeper local credit), and a clean embedding ABI.

That condition is the whole game. When it holds, small frozen contracted parts
compose into larger behavior instead of forcing a retrained monolith — a
planner wires them by contract, an executor restores the signal at every
handoff, evidence ranks the alternatives, and a proven plan distills into a new
part. The core stays domain-agnostic; knowledge grows by *adding primitives*,
not by changing the engine. The demos run at toy scale (hex digits, bytes,
decimal arithmetic, noisy glyphs) on purpose — the point is the mechanism, and
**Knowing the Edge** maps where the mechanism stops working.

## The Loop

```
input
  │
  ▼
typed source ports     ── finite, validated interfaces (the contract)
  │
  ▼
planner picks parts    ── reads contracts + reliability, no hand-wiring
  │
  ▼
executor runs the plan ── validates + canonicalizes every handoff
  │
  ▼
reliability evidence   ── per-primitive success/failure, scored globally
  │
  ▼
proven plan distills   ── a proven plan becomes one new primitive
  │
  ▼
library grows          ── reachable in a single hop; abstractions compound
```

Each **primitive** is a tiny feed-forward network, trained once and frozen.
Everything else is composition over those frozen parts.

## Verified Today

Every current claim in this table names its executable gate or evidence log;
historical measurements elsewhere remain explicitly dated. "In `make test`"
means the gate runs in the full verification chain on every `make test`.

Rechecked end-to-end on this host (2026-07-11, post gap-lane fixes): `make
test` — all 25 suites green in **3:54.31** wall (99% CPU, 3.15 GiB peak RSS);
`make unified` — `CNET_UNIFIED_PASS`. The unified run requires a fresh, strict
**15/15** in-scope claims ledger; the physical-GPU lane is reported explicitly
as out of scope rather than accepted from an older log.

| Area | Verified by | Status |
|---|---|---|
| CCE runtime (tensor → block → cascade → archive → forest → router → learn) | `make cce_smoke`, `make cce_train_bench` | passing (standalone gates) |
| CCE storage + loaders (C ABI/DLL, safetensors, autograd, model save/load, zero-copy WARM views) | `cce_dll`, `cce_safetensors_test`, `cce_autograd_test`, `cce_model_test`, `cce_view`, `forest_view` | passing, in `make test` |
| Contract security + one-file sealed units | `make contract_secure`, `make contract_unit` | passing, in `make test` |
| Contract correctness + robust promotion quality/speed | `make contract_optimized` | malformed authored/frozen contracts refused atomically; stronger certified margin wins with one replay per model |
| Unified contract runtime (BTN + CCE + Oracle v2 evidence-carrying specialists + CNB version 2 semantics under stable CNB1 magic + planner/executor + `SoulHost` + .NET/MCP projection) | `make unified` | CPU-only, model-free vertical acceptance gate; Oracle descriptors remain provenance until independently certified/admitted |
| **One `Specialist` type across kinds** — a native BTN, a real CCE model, and an Oracle unit as ordinary nodes of ONE certified plan (route + DAG), admitted through the single door (`specialist_admit`), trust/residency/role axis views | `make specialist_unit`, `make unified_specialist` | `SPECIALIST_UNIT_PASS`, `HET_PLAN_PASS`, in `make unified`; edge refusals, lifecycle axes, and the heterogeneous strict-exact plan are gated |
| **Runtime health optimizer** — one opt-in maintenance pass that fixes (audit → contract/teacher fault labeling → heal via retrain + passing re-certify) and improves (evidence promotion, shadow hot-swap) through existing certified paths only | `make specialist_health` | `SPECIALIST_HEALTH_PASS` (17 checks), in `make unified`; healthy registry = proven no-op, zero-init config = total no-op; demoted adapters stay RESET — their repair path is re-acquisition  Wired into SoulHost/MCP: `soul_health_tick` C ABI, `cnet_health_tick` tool, opt-in `CNET_HEALTH_TICK_SECONDS` timer |
| **The gap lane** — the 24/7 learning runtime: serving misses land in a gap inbox, the lane ingests them into the persistent ledger, bridges unhealed demotions to HEALTH gaps, and drains by teaching from a bound local model (dynamic-growth students, PROOF/SAMPLED certification, sealed into the base); checkpoints are atomic for base AND ledger; a fresh lane resumes from disk | `make gap_lane` | `GAP_LANE_PASS` (22 checks), in `make unified`; daemon `bin/gap_lane_run` (`make gap_lane_run_build`) deployed as the `cnet-gap-lane` user service teaching from gemma4-v2 12B (CPU, int8-on-load) Novel goals flow in by explicit typed signature: the `cnet_request_capability` MCP tool (`soul_request` ABI) serves the request now when a certified plan exists, answers capability probes without executing, and otherwise queues the full signature to the inbox — a goal no longer needs an existing unit to become the lane’s work. The wildcard-identity plan (length 0) counts as no plan, same rule as `soul_route` Teaching is corpus-drawn: `CNET_WINDOW_FILE` maps the one-hot alphabet to real corpus token ids (the english window) and `CNET_LANE_CONTEXT_FILE` pins a real tokenized prose prefix in the KV once — every teaching call is an independent probe of prefix+token (`cce_gguf_qwen2_forward_probes`, KV never modified), with the head restricted to the window ids (bit-identical, fraction of the cost). Bare single-token contexts remain the fallback Multiple named contexts per lane: every `<name>.ids` in `CNET_LANE_CONTEXT_DIR` is a pinned-able context selected by goal-tag prefix (`<name>_…`); the teacher re-pins the KV on context switch (once per gap, not per point), and each context carries its own provenance fingerprint into the ledger — proven live with lamp/storm units closed in one tick whose transitions differ by context on identical inputs |
| Generated claims ledger (unified gate logs → machine-readable run-scoped verdicts) | `make claims`, `make unified`; inventory only: `make claims_all` | exact terminal markers required; current verification refuses missing, failed, or pre-run logs for its 13 CPU/model-file-free claims and labels the GPU-only lane `OUT_OF_SCOPE`; the all-log inventory does not claim freshness |
| Work-conserving Oracle v2 async lanes (backpressure, cancellation, deadlines, telemetry) | `make unified_async` | `ASYNC_RUNTIME_PASS` |
| Oracle v2 governed invocation | `make oracle_v2_bench` | semantic validation at 12.446 ns/call; ~80.3M calls/s, validator delta 0.676 ns (re-measured 2026-07-11; supersedes the July 10 preliminary 12.432) |
| Two physical R9700 lanes, exact CPU parity, iGPU excluded | `make unified_gpu` | 1.984× over the serial lane fixture; `ASYNC_GPU_LANES_PASS` |
| Model-universal residency (Dense + MoE + SSM descriptors) | `make unified_models` | 90 lifecycle/resource checks + 12 catalog checks; lazy load, leases, deduplicated cold loads, LRU, atomic multi-resource admission |
| Qwythos (Qwen3.5 architecture) QGKP v3 lossless envelope | `make qgkp_envelope_test` (synthetic format), `make qwythos_qgkp_acceptance` (real artifact) | `QGKP_ENVELOPE_PASS`; 1,048,576-token context metadata and payload SHA-256 preserved; mixed TQ1_0+Q4_K passes 3/3 coherence at 70.97 tok/s on GPU1 |
| Universal model layer (detect, SSM + llama runners, specialist graph, weight store, bounded-RAM tiers, evidence-gated merge) | `make cce_detect` / `cce_ssm` / `cce_st_llama` / `cce_specgraph` / `cce_wstore` / `cce_tiers` / `cce_similar` | passing, in `make test` |
| Autonomy loop (gap-triggered acquisition → unified CNB version 2 base under stable CNB1 magic, with v1 read compatibility → flagship harness) | `make acquire`, `make base`, `make flagship` | passing, in `make test`; real gemma4-v2 12B campaign **253/256** certified (SAMPLED, Wilson ≥ 0.984), 93% live-model fidelity when queried (`soul_query`) — the earlier "256/256 at 100%" was a NaN-oracle artifact, retracted |
| Restored legacy aggregate + allocation-balance leak gate | `make legacy`, `make leakcheck` | passing, in `make test` |
| Loader robustness (byte-flip + truncation sweeps over every artifact loader) | `make mutate` | passing, in `make test`; sealed formats refuse every mutation, unsealed probes never crash |
| GPU forward (self-contained OpenCL, **multi-GPU + int8**) | `make gpu_equiv_build`; `make` `clgemm_unit` | optional; dual-R9700 oracle pool + `q8` layers, 4.28× on 12B, **bit-identical proven** (`clgemm_unit`, 360 calls), NaN-hard-failed |
| Supra int8 PTQ | `make supra_console` (`--int8`) | near-lossless |
| Packed 1.6-bit ternary | `make wordlm_bitnet` + export/reload tests | storage bit-exact (`ΔNLL=0` reload); *deployable quality still needs QAT at model scale* |
| Representation walls / planner scaling | `make margin` / `fuzzy` / `stochastic`, `make planner_scale_study` | documented **limits**, not claims |

Not claimed: beating PyTorch/TensorFlow globally, free-running clean prose from
the tiny char-LM, or steering the 3M-param Supra model into long-form output —
see *Knowing the Edge* and the caveats inside each section.

## Optimization Ledger (cumulative)

This ledger consolidates the optimizations recorded across the repository history,
implementation plans, acceptance logs, and the June–July 2026 working sessions.
It distinguishes shipped/gated behavior from experiments and preserves negative
results so an old headline cannot silently become a current claim. Measurements
from different fixtures or hardware are not directly comparable; the newest
correctness-gated result supersedes an older preliminary number.

### Contract, planner, and knowledge-composition optimizations

| Era | Optimization | Effect / invariant | Gate or evidence |
|---|---|---|---|
| v0.5.3–v2.9 | Attention planner stripped of synthetic dashboard authority; circuit hints, typed engram cache, and rank artifacts kept `SHADOW_ONLY`/`ORDER_ONLY`; formula IR, contracts, parser refusal, and task corpus added | Advisory evidence can rank or report but cannot invent validity, mutate metrics, or bypass execution | historical aggregate; [`docs/cnet-history.md`](docs/cnet-history.md) |
| v3.0–v4.4 | Learned glyph, 7-segment, 3×5-grid, and 4×4-block leaves share typed `dec_symbol` handoffs; robust margin certification and certified-only planning | Four perceptual domains compose through one finite interface; low-margin cases abstain instead of leaking raw values | `make glyph_habitat`, `make margin`, `make sevenseg` |
| v4.1–v4.4 | Accepted-leak frontier, normalized denominators, provenance, per-domain stratification, and mixed-domain pair matrices | Replaced flattering aggregate accuracy with coverage-vs-confident-error evidence and explicit tail behavior | generated `v4_2_*`, `v4_3_*`, `v4_4_*` frontier artifacts |
| June 2026 | Typed ports, DAG source assignment/backtracking, learned reliability persistence, strict execution, machine-checkable contracts/laws, circuit plans, and chunk consolidation | Planner composition became recoverable and auditable; successful multi-step plans can distill into one reusable primitive | `make certify`, `make property`, `make circuit`, `make chunk`, `make library` |
| June 2026 | Neural lifecycle spine: provisional/frozen/shadow states, self-healing, cost-aware dual track, residue scans, and fast DAG/circuit decode lanes | Avoids retraining frozen parts, isolates experiments, and gives explicit cost/accuracy trade-offs | `make lbench`, `make throughput`, `make test_fastpath`, `make compounding_bench` |
| June 26–27 | Dynamic PDF/JSON ingestion, compounding kNN memory, HOT/WARM tile memory, TF-IDF, discounted-PPMI synonyms, inverted index, semantic dedup, capped/pruned consolidation candidates, and graduation to certified units | Reuse-before-retrain retrieval path with bounded candidate work and byte-identical linear-oracle checks | `make pdftest`, `make jsonstory`, `make compound`, `make tiermem_test`, `make tfidf`, `make synonyms`, `make tileindex`, `make consolidate`, `make graduate` |
| v5.0–v5.1 | No-authority proposal sidecar, below-beam recovery probe, and derivation-lock structural preference | Candidate exploration snapshots/restores reliability evidence; official planner remains unchanged unless an explicit existing ORDER_ONLY gate is enabled | `make belowbeam_chars`, `make probe_overhead`, `make struct_pref` |

### CCE learning, storage, compression, and inference optimizations

| Date / era | Optimization | Measured or structural result | Gate or evidence |
|---|---|---|---|
| June 2026 | Deeper cascades, patch blocks, per-block goodness/freezing, micro-split growth, header-based archive directory, mmap zero-copy WARM views | Many frozen specialists can remain cold and page in only when touched; learning stays local to active blocks | `make cce_smoke`, `make cce_train_bench`, `cce_view`, `forest_view` |
| June 2026 | Train every sample, switch classification heads from MSE to cross-entropy, use a word-level mode, repair decoder/LR, and hoist Adam bias correction out of the per-weight loop | Fixed five generation failures; Adam hoist reduced the cited 30-epoch char run from ~100 s to ~8 s (~13×) | `test_tinystories`, `make wordlm` |
| June 2026 | Tied-embedding/class-factored O(V·d) word LM with isolated exact gradients | At V=50,000: 9.7M parameters instead of 7.5B (773× smaller), with numeric gradcheck | `make wordlm`, `cce_wordlm_gradcheck` |
| June 2026 | Full Supra checkpoint decomposition and compression ladder | 118.9MB source → 16.3MB near-lossless int8 PTQ; 6.95MB trit-packed artifact is storage-exact, while post-hoc ternary quality collapse remains explicitly rejected | `make supra_console`, `make bitnet_qat`, `make wordlm_bitnet` |
| July 3 | Trit LUT decode, int8-shaped tiling, and OpenMP | 256×50,520 head: 6.14 → 0.22 ms/forward (27.9×), bit-identical to int8 ternary | `make trit_bench` |
| July 3 | Universal structural detection, GGUF/HF-llama/Mamba runners, sharded safetensors, content-addressed weights, bounded-RAM LRU tiers, and evidence-gated family merge | Header-first detection, manifest reuse, HOT-cap bit identity, and merge refusal on material behavior change; corrected GGUF ne-order dimension handling | `make cce_detect`, `cce_ssm`, `cce_st_llama`, `cce_wstore`, `cce_tiers`, `cce_similar`, `merge_family`, `hybrid_catalog` |
| July 3 | Contract digests/seals, certification cache, certificate-to-weight binding, and bit-packed one-file `.cnu` units | Tamper refusal and audit demotion; `.cnu` measured 3.0× smaller than the equivalent text pair | `make contract_secure`, `make contract_unit` |
| July 3 | Loader mutation sweeps, hard-failing positive-marker log scan, restored ~3,500-line DAG implementation, and allocation-balance gate | Fixed two GGUF crash bugs, prevents masked test failures, restored 51 legacy failures/segfault, and guards allocation balance | `make mutate`, `make legacy`, `make leakcheck`, `tests/verify_logs.sh` |
| July 3–5 | Self-contained OpenCL float/q8 kernels, per-GPU oracle instances, column splitting, KV-prefix reuse, and window-restricted head | Current honest Gemma4-v2 result: 4.28× forward on two R9700s; 360-call CPU/GPU bit-identity gate; any NaN hard-fails | `clgemm_unit`, `make gpu_equiv_build` |
| July 3–5 | Gap-triggered acquisition, CNB base, sampled/Wilson certification, split-conformal abstention, pilot scheduling, margin-aware refusal, OpenMP mining, and dual-GPU oracle pool | OpenMP campaign path measured 29×; dual-GPU campaign measured 2.7× with identical digests; corrected real campaign is 253/256 certified and 45/48 (93%) live-query fidelity | `make acquire`, `make base`, `make flagship`, `cnb_audit`, `soul_query` |
| July 4–9 | Geometry-driven Gemma attention, real Q4_K dequantization, fp16-subnormal handling, mmap-backed GGUF loading, int8-on-load, Q6_K/KV caps, and leak fixes | Replaced a broken/NaN oracle with faithful finite inference; old vacuous 256/256 and 11.9× claims were retracted | oracle integrity trilogy, `gpu_equiv`, `make flagship` |

### Previous-session optimizations (July 8–11, 2026)

| Date | Optimization | Verified result / boundary | Gate or evidence |
|---|---|---|---|
| July 8 | Explicit token windows, line-buffered progress, variable retry seeds, adaptive staged training, configurable cert sampling, set-valued top-k, golden-oracle checks, warm start, screening, PAIR windows, memory diet, lane oversubscription, and run manifests | Faster recovery and less wasted training while preserving replayable run identity | flagship/acquisition gates and manifest replay |
| July 8 | Batched oracle probes, token-string identity across teachers, drift recertification, and cross-teacher set agreement | Batch path is equivalence-gated; recertification compares semantics rather than assuming token-ID identity across tokenizers | `CNET_ORACLE_BATCH`, `CNET_MANIFEST`, `CNET_RECERT`, `CNET_RECERT_SETCMP` |
| July 10 | Unified BTN/CCE/Oracle specialist runtime with CNB version 2 semantics under stable CNB1 magic and v1 read compatibility, `SoulHost`, `cnet.so`, .NET host, and stdio MCP projection | One contract/evidence lifecycle; Oracle descriptors remain provenance until independent certification and registry admission | `make unified` → `CNET_UNIFIED_PASS` |
| July 10 | Oracle v2 status/evidence/identity ABI and centralized semantic invocation | Dated preliminary: governed v2 + semantic validation at 12.432 ns/call, ~80.4M calls/s; validator adds 0.721 ns/call. The July 11 measurement above supersedes it | `make oracle_v2_bench`; `logs/oracle_v2_bench.log` |
| July 10 | Work-conserving bounded async lane pool with copied inputs, ordered collection, backpressure, cancellation, deadlines, and telemetry | Two portable lanes beat the serial lower bound; expired/cancelled work remains explicit evidence | `make unified_async` → `ASYNC_RUNTIME_PASS` |
| July 10 | Independent physical GPU0/GPU1 queues with iGPU exclusion | Synthetic exact-output lane fixture: 3.423 → 1.725 ms, 1.984×, 9,274 jobs/s; both R9700s performed useful work | `make unified_gpu` → `ASYNC_GPU_LANES_PASS` |
| July 10–11 | Backend-neutral model catalog and residency manager | 90 runtime + 12 catalog checks cover header-only GGUF descriptors, budgets, lazy load/reuse, generation-stamped leases, concurrent cold-load dedup, sticky failure/retry, true LRU, pinning, and atomic Dense/MoE placement | `make unified_models` |
| July 10 | Resumable DS4 dual-R9700 staging with plan/identity/start separation and 1GiB chunk-tree identity | Keeps DS4 as a replaceable backend and CNET as lifecycle/policy authority; does not claim DS4 numerical parity before endpoint verification | `make unified_ds4_launcher`, `scripts/run_cnet_ds4_dual.sh` |
| July 10–11 | Qwythos/Qwen3.5-architecture mixed TQ1_0+Q4_K repair after naive TQ1_0 destroyed coherence | Protected mixed artifact restored 3/3 coherence at 69.37 tok/s; the failed 0/3 TQ1_0 artifact remains recorded as a negative result | `logs/qwythos_tq1_fixed_gpu1_score.log`, `logs/qwythos_cnet_tq1_comparison.json` |
| July 11 | QGKP v3 lossless CNET envelope for the hybrid Qwythos/Qwen3.5-architecture artifact | 5,578,649,440-byte payload materializes with SHA-256 `a6eee1573d145d61b895cf0a575681ab48452d39fba56f77343fe4cdf31315ee`; envelope retains mixed quantization and 1,048,576-token metadata. This is a governed bridge to the hybrid backend, not a claim that the legacy QGKP-v2 native runner implements Mamba-2/attention mixing | `make qgkp_envelope_test` → `QGKP_ENVELOPE_PASS`; real-artifact evidence: `make qwythos_qgkp_acceptance` |
| July 11 | Qwythos QGKP execution acceptance | 3/3 causal/constraint/narrative cases coherent at 70.97 tok/s on GPU1; envelope file SHA-256 `f4e68af7…f5f2654` | `logs/qwythos_qgkp_gpu1_score.log`, `logs/qwythos_qgkp_artifact_sha256.log` |
| July 11 | Contract authority-boundary hardening and promotion simplification | Atomic refusal for malformed names/counts/widths/tables; pre-hash signature validation prevents forged-shape OOB; finite RAW and normalized EVIDENCE round-trip at `%.17g`; non-finite refusal; certified minimum margin replaces useless canonicalized MSE | `make contract_optimized` |
| July 11 | Removed the second candidate-scoring replay | Exactly one certification replay per model: 4,000 forwards for 2,000 comparisons, a structural 50% reduction from the former certify-plus-score path | `CONTRACT_OPT_BENCH`, `CONTRACT_OPTIMIZED_PASS` |
| July 11 | Unified `Specialist` type over the adapter ABI (`include/specialist.h`): kind (BTN/CCE/Oracle), ONE admission door (`specialist_admit` — certification only, identical for every kind), and trust/residency/role axis views folding `PrimitiveState`, `certified`, shadow/recipe flags, CCE tiers, and model-catalog states into one vocabulary | Heterogeneous-plan acceptance plus edge units: one ordinary route/DAG plan mixes a trained native BTN, an exact CCE linear-head model, and an external oracle; unknown kinds, adapter/native confusion, and unknown axes are refused; lifecycle trust remains state-authoritative | `make specialist_unit` → `SPECIALIST_UNIT_PASS`; `make unified_specialist` → `HET_PLAN_PASS` |
| July 11 | Runtime health optimizer (`include/specialist_health.h`, `specialist_health_pass`): audit → label → heal → promote → shadow-swap as ONE pass with an exact machine-readable report (counts + trust histogram) | Tampered certificates demoted and restored FROZEN only via a passing re-certify; parked faults labeled from the contract exemplar table first, a clean teacher second; FUZZY promotes to PROVISIONAL on evidence, never further; a shadow that out-scores its incumbent AND certifies hot-swaps it; a healthy registry is a gated no-op | `make specialist_health` → `SPECIALIST_HEALTH_PASS`  Ticked from the running system: `soul_health_tick` rematerializes contracts from the sealed base blobs, the MCP server exposes `cnet_health_tick`, and `CNET_HEALTH_TICK_SECONDS` (opt-in, 0 = off) runs it periodically under one dispatch gate |
| July 11 | The gap lane (`include/gap_lane.h`, `src/gap_lane.c`, daemon `tests/gap_lane_run.c`): detect (serving-side gap INBOX via `CNET_GAP_INBOX` in `soul_route`, rename-then-read ingest, health-bridge with a no-churn re-note guard) → acquire (`acquire_drain` per open gap: oracle-mined, dynamic-growth students, certified, sealed) → persist (atomic base AND ledger checkpoints — the ledger save gained tmp+rename) | Hermetic gate closes the whole circle: live miss answered-and-harvested by the oracle, inbox ingested, PROOF-certified student under the structure budget, strict-exact replan, tamper→audit→HEALTH-gap→rebuild, resume from disk with an idle no-op steady state. Field notes baked in: default f32 GGUF load is ~48 G for 12B — `CNET_ORACLE_INT8=1` is the diet (~20 G resident); a healthy-but-demoted incumbent defers `incumbent_healthy` (churn refused) | `make gap_lane` → `GAP_LANE_PASS`; `make gap_lane_run_build` ; novel-goal requests exposed end-to-end (`soul_request` → `cnet_request_capability`), exercised inside `make unified` (tool call → `gap_noted` → inbox line verified) ; corpus-drawn teaching contexts landed the same day: window files (flagship convention) + pinned real-prose prefixes via the probe path, id-file parsing strict and gated (refuse, never default) — answering the campaign's own caveat that meaningful extraction needs real-context conditioning |
| July 11 | The first REAL request through the whole loop found three genuine defects, all fixed and gated: `cnb_load_registry` stored borrowed unit names in a growable row array (every earlier `RegistryEntry.name` dangled on realloc — latent for months, exposed live at 254 units; now per-name heap buffers + a 13-unit growth regression); the teacher never reset the runner's persistent KV cache (all forwards failed after `CNET_MAX_CTX` calls, masquerading as `oracle_unfit`; superseded by the probe path, which never writes KV); average-loss early stopping left one stubborn point wrong on near-constant maps (`certify_failed` at 255/256; `CNET_ACQ_TARGET_LOSS=1e-7` measured 256/256, min margin 0.9881) | The deferral ladder along the way was the gates working: dead special-token window → `insufficient_exemplars`, flat-logit ties → `oracle_unfit`, capacity wall → `certify_failed`; the 253-unit base was byte-untouched through every refusal | `make soul_host_test` (growth regression), `make unified` |
| July 11 | First runtime-grown units in the deployed soul: `acq_lane_wnd256_e2e` (unit 254, bare-context) and `corpus_next_v01` (unit 255, corpus-context, closed FIRST attempt) — both requested via MCP, taught by gemma4-v2 12B on CPU, PROOF-certified, sealed, and `served:true` by the certified plan | The corpus unit's probed transitions are real English bigrams (of→the, to→the, is→the) — real-context conditioning extracts linguistic behavior, answering the campaign's own caveat; requests use `in_tag` `w_cur` (both tags required: a wildcard input trivially "already satisfies" any same-shape goal and is unservable by construction) | ledger provenance in `soul_gemma4v2_final.cnb.gaps.txt`; live `cnet_request_capability` round-trips |
| July 11 | Generated claims ledger | `make claims` first executes the unified CPU gate, then emits machine-readable records with a run sentinel, exact markers, timestamps, and host fingerprint; missing, failed, or pre-run evidence aborts. `make claims_all` is explicitly an unscoped inventory and does not imply freshness | `make claims` → `logs/claims.jsonl`, `docs/verified-today.generated.md`; inventory: `make claims_all` |

The detailed mechanism-by-mechanism chronology remains in
[`docs/cnet-history.md`](docs/cnet-history.md); this ledger is the current README
index of what was optimized, what evidence supports it, and which negative or
retracted results must not be revived.

## The Unified Specialist (one type, one door, three axes)

The framework's own thesis — typed, certified, composable units — now applies
to the framework: `include/specialist.h` is the one type every backend becomes
before it can plan, execute, accrue evidence, or certify.

- **One type.** A `Specialist` is a planner-visible node plus its **kind**
  (native BTN / CCE model / Oracle unit). Execution was already unified by the
  BTN adapter ABI (`nn.h`); the Specialist names it, so "which backend is this
  node" is a field, not archaeology.
- **One door.** `specialist_admit` is the only admission path, identical for
  every kind: certify against a typed contract, then register. An impostor of
  any kind is refused the same way at the same place.
- **Three axes.** The scattered lifecycle vocabulary factors into orthogonal
  views read off existing state (never stored twice, so they cannot drift):
  **trust** (uncertified → evidenced → certified → demoted, mirroring
  `PrimitiveState`), **residency** (hot/warm/cold — one enum for the CCE
  forest tiers, tile memory, and the model catalog's states), and **role**
  (active / shadow / recipe / advisory — the CNET-D no-authority lane keeps
  its boundary; advisory artifacts never enter the registry by design).
- **The acid test.** `make unified_specialist` plans ONE ordinary certified
  plan (route and DAG) whose nodes are a trained native BTN, a real CCE
  linear-head model, and an external oracle — `HET_PLAN_PASS`. Integration
  would let the three coexist; only unification lets them chain inside a
  single plan under one lifecycle.
- **Health.** `specialist_health_pass` (`make specialist_health`) is the
  maintenance loop over the type: audit → label faults (contract exemplars
  first, teacher second) → heal (retrain + passing re-certify, the only way
  back to FROZEN) → promote on evidence → shadow hot-swap. It drives only
  existing certified paths and reports exact counts plus a trust histogram;
  a healthy registry is a gated no-op, and adapters that cannot retrain stay
  RESET for the acquisition loop — fix by evidence, never by fiat.
  The pass is wired into the running system: `soul_health_tick` on the
  SoulHost ABI (the sealed base is the contract source, so heal re-certifies
  against the same truth the unit was admitted with), the `cnet_health_tick`
  MCP tool, and an opt-in periodic timer (`CNET_HEALTH_TICK_SECONDS`,
  0 = off — zero-init changes nothing).
- **Generated claims.** `make claims` first executes `make unified`, then writes
  `docs/verified-today.generated.md` + `logs/claims.jsonl` from exact terminal
  markers under a run sentinel: every in-scope log must be fresh, and missing,
  failed, or pre-run evidence aborts. The GPU-only lane remains visible as
  `OUT_OF_SCOPE`. `make claims_all` inventories all known logs but labels that
  output as unscoped evidence, never as current verification.

## Quick Start

```sh
make                  # build nn_demo
make run              # train all primitives, write weight files
make decimal          # second domain: train + run the decimal arithmetic acts
make circuit          # discover, verify and distill circuits; measure pruning
make glyph_habitat    # the current frontier: 4 learned perceptual domains (v4.4) + interactive agent + build console + own internal LLM models + HF contract training (speech/generative) + 5B/6A mitigations
make glyph_habitat --demo 5A   # full 5A interactive memory agent episode (queries, memory, branching stories, summary table)
make glyph_habitat --demo 5B   # 5B Evidence ports + late binding (finite distribs avoid early snap loss)
make glyph_habitat --demo 6A   # 6A Concept ports + auto-abstraction (hierarchical symbols cut explosion/narrow ports)
make glyph_habitat --demo MCP  # MCP external tools: Wikipedia + Web Search + File Read + Calculator + Summarizer (with memory caching)
make glyph_habitat --demo AGENT  # Agentic memory: own chat history, internal thinking (thoughts), and automatic recall of past sessions
make glyph_habitat --demo BUILD  # Build artifacts using full stack + Grok-style console + MCP writes
make glyph_habitat --demo SPEECH # Train contracts from Hugging Face datasets (speech commands example)
make glyph_habitat --demo LLM   # Train our own CNET-native LLM-type generative model inside (next-token step BTN + contract)
./glyph_habitat --interactive  # Real REPL: ask, ingest books, autolearn, live drafting stream (skeleton->refine->reflect visible)
./glyph_habitat --demo EVAL   # Evaluation harness
# All 5 priorities + build + own internal models + HF contract training done.
# Working Grok-like agent console (with own LLM training): ./build/build_tool.exe (supports build/train lm/train speech + natural language + internal thinking)
make planner_scale_study   # the scaling stress test (breadth / collision / depth)
make compounding_bench       # 3C dual-track demo: LOW vs DEFAULT cost/accuracy trade-off on the same library

# Universal model layer: detect + run any supported model file
make detect FILE=Models/gemma-4-12B-it-MTP-Q8_0.gguf   # probe structure, no weights loaded
make test             # native C verification chain and positive-marker log gate
make unified          # CPU-only native + cnet.so + .NET host + stdio MCP acceptance
make specialist_unit  # Specialist lifecycle/refusal edge cases
make unified_specialist  # the unification acid test: BTN + CCE + Oracle in ONE certified plan
make claims           # execute unified and emit fresh run-scoped claims evidence
make claims_all       # inventory every known log without claiming current-run freshness

# The autonomy loop: extract certified units from a real model.
# MODEL=gemma4-v2-Q4_K_M.gguf (12B); dual-GPU int8 oracle pool + margin-aware certification.
make flagship_run_build
# 100%-yield config (2026-07-05): int8 pool + margin-aware + capacity-tuned student
CNET_ORACLE_INT8=1 CNET_GPU=1 CNET_CERT_MARGIN=1.0 \
  CNET_ACQ_HIDDEN=128 CNET_ACQ_MAXHIDDEN=512 CNET_ACQ_EPOCHS=20000 \
  ./bin/flagship_run Models/gemma4-v2-Q4_K_M.gguf 256 256 80 1.0 0 soul.cnb topk   # resumable; echo stop > soul.cnb.stop
make cnb_audit && ./bin/cnb_audit soul.cnb              # counts + certify-on-load + tag audit
make gpu_equiv_build && CNET_ORACLE_INT8=1 ./bin/gpu_equiv Models/gemma4-v2-Q4_K_M.gguf   # CPU-vs-GPU decision equivalence (NaN-hard-failed) + speedup
make soul_query_build && CNET_ORACLE_INT8=1 ./bin/soul_query Models/gemma4-v2-Q4_K_M.gguf soul.cnb   # ask the soul questions (raw token ids; Python decoder removed for pure-C goal)

# New pure-C Contract Cascade Engine (CCE)
make cce_smoke               # build & run the CCE smoke test (tensor → block → cascade → archive → forest/branches → router(SSMax) → learn (deeper credit) → patch → gpu → ABI + router-learn loop)
make unified_async           # portable 2+ lane async contract/evidence gate
make unified_gpu             # independent GPU0/GPU1 queues + exact-output gate and throughput report
# DS4 dual-R9700 staging keeps llama.cpp :8081 untouched and uses user-systemd supervision.
source config/cnet-ds4-dual.env.example
scripts/run_cnet_ds4_dual.sh print-plan
scripts/run_cnet_ds4_dual.sh identity   # resumable 1 GiB chunk-tree identity
scripts/run_cnet_ds4_dual.sh start      # stages the API on :8082
# cce_train_bench now exercises deeper cascades (4 blocks), real harness (nonlinear/spatial/accuracy), persist, micro-split, etc.

./test_tinystories         # real narrative data (TinyStories) — CCE with context windows, guided coherent generation + raw logits sampling (A/B demo)
```

On Windows the Makefile works under MinGW (`mingw32-make` or `make` from
MSYS2); binaries get an `.exe` suffix automatically. The build uses
`-O3 -march=native -mno-avx` — `-mno-avx` is **required** with `-march=native`
on MinGW GCC 15.2 (aligned 256-bit moves on by-value structs segfault on a
16-byte-aligned stack). Drop `-march=native` for portable binaries.

## Contract Cascade Engine (CCE) — Pure C Runtime

The modern realization of these ideas is the **Contract Cascade Engine (CCE)**, a pure C11 implementation.

**Boundary of "pure C":** the core runtime is C11 + `libm` only — no framework,
no Python, no mandatory GPU. Everything else is an *optional integration outside
the core*: the OpenCL GPU forward (`OpenCL.dll` loaded dynamically at runtime,
CPU path unchanged when absent), the .NET test lane, the safetensors/GGUF
loaders, and downloaded HF datasets/models used purely as input data. None of
these are required to build or run the core.

### High-Level Philosophy

Instead of one large differentiable model trained end-to-end with backprop, CCE treats **frozen structure as a feature**, not a bug:

- Small, specialized "contracts" (cascades of blocks) are trained, then frozen.
- Only unfrozen parts continue to adapt.
- Scale comes from **more specialists + intelligent recall**, not bigger matrices.
- Partial loading via memory mapping allows thousands of frozen contracts with minimal RAM.

### Layered Architecture

| Layer            | Responsibility                                      | Key Ideas |
|------------------|-----------------------------------------------------|-----------|
| **cce_tensor**   | Memory layout & operations                          | Row-major, aligned, zero-copy `views` (for mmap) |
| **cce_block**    | Atomic specialist unit                              | Linear, patch, depthwise etc. Carry `goodness` + freeze flags |
| **cce_cascade**  | Ordered composition of blocks                       | Micro-split (ACE), full freeze, forward chaining |
| **cce_learn**    | Credit assignment & adaptation                      | Layer-wise local targets + decoder proxies, per-block goodness/freeze, DFA + cheap FF negative, NoProp style. Supports deeper cascades + micro-split. |
| **cce_router**   | Dispatch to the right specialist(s)                 | SSMax (sparse softmax over similarity + goodness). Top-k routing. |
| **cce_forest**   | Collection of branches + tiering                    | Branches = specialist cascades (with leaf blocks: linear/patch). Hot (RAM, trainable), Warm (mmap zero-copy views), Cold (disk). Header-based dir + persist. Centroid + SSMax recall. Online growth via micro-split in router-learn loop. |
| **cce_archive**  | Single-file persistent storage                      | Append-only sections + directory. True partial loading via mmap (no full deserialize). |
| **cce_clgemm**   | Multi-GPU forward for the transformer runner (optional) | Self-contained OpenCL: `OpenCL.dll` loaded dynamically (no SDK/CUDA toolkit), runtime-compiled float + `q8` (int8) GEMM, device-resident weights, **column-split across discrete GPUs** (iGPU excluded by property). Oracle *pool* = one instance per GPU. Bit-identity proven by `clgemm_unit` (360 calls, single/dual/split), `FP_CONTRACT OFF`. CPU path unchanged when absent. |
| **C ABI**        | Stable embedding interface (`include/cce/cce.h`)    | `cce_open`, `cce_infer`, `cce_adapt`, `cce_tick`. Easy to call from Unity/C#/Python. |

### Model-Universal Runtime (Dense + MoE + SSM)

`include/model_runtime.h` and `src/model_runtime.c` are CNET's single model
catalog and lifecycle authority. Dense llama-family models, distributed MoE
models, SSMs, embeddings, and CCE-native artifacts coexist as descriptors;
DS4, CCE, and llama.cpp remain replaceable execution backends rather than
independent policy engines.

The runtime provides:

- header-first GGUF inspection through `cce_model_descriptor_probe` (tensor
  metadata only; no weight materialization);
- explicit CPU/GPU resource masks and independent byte budgets;
- lazy cold load, persistent hot residency, concurrent load deduplication, and
  atomic generation-stamped leases;
- one-resource dense placement that first uses a clean preference, then
  minimizes evicted bytes and prefers more free capacity;
- atomic multi-resource MoE admission and true LRU eviction, while leased or
  pinned models are never evicted;
- unlocked backend loading with per-resource reservations, sticky failure and
  explicit retry, plus rejection/unload when a backend exceeds its declared
  resident upper bound.

This adopts the useful llama.cpp runtime behavior under CNET governance:
quantized artifact-size accounting, mmap-friendly identity, explicit placement,
persistent models, model/context separation, and pressure-driven unload. The
architecture-specific tokenizer, templates, KV layout, partial offload, kernels,
and numerical parity remain backend responsibilities; they do not bypass CNET's
catalog, evidence, cancellation, or branch/soul policy.

Resource bits are `CPU=1<<0`, `GPU0=1<<1`, `GPU1=1<<2`, and so on. A dense
model normally allows both R9700 bits but requires one resource; a fixed
dual-GPU MoE allows both bits and requires two resources atomically.
Run the focused acceptance gate with `make unified_models`. A guarded real-file
probe is available by setting `CNET_TEST_DENSE_GGUF` and/or
`CNET_TEST_MOE_GGUF` before running `bin/test_model_catalog`.

### Storage & Partial Loading (cce_archive + cce_forest)

All contracts live in one (or a few) `.cce` files:

- A small in-memory (or on-disk) **directory** maps names → (offset, size).
- Frozen contracts are **memory-mapped views** — the OS only brings in the pages that are actually touched during inference.
- Optimizer state is stripped from cold sections.
- New contracts are appended; old ones are never rewritten.

This is how CCE scales to "thousands of specialists" without loading everything.

#### Importing from SafeTensors (optional seeding)

`cce_safetensors_load` + helpers (`populate_block`, `build_linear_cascade`, `add_linear_branch`, `btn_populate_from_safetensors`) let you **safely** extract tensors from `.safetensors` files (header length first, strict offset/dtype validation, no execution) and map them into CCE blocks/cascades/forests or legacy BTN primitives.

- Always header-first; all offsets validated against actual file size before any data read.
- F32 primary; F16/BF16 conversion supported on load.
- Transpose flag for common PyTorch `[out,in]` vs CCE `[in,out]` layouts.
- Goal: seed a few small frozen specialists or init a cascade from an exported tiny MLP.
- A **full** pretrained model (HF `SupraLabs/Supra-A2A-Nano-Exp`, 119 MB) is now loaded,
  **decomposed** into CNET specialists, run with a **bit-exact** pure-C forward, **quantized**
  and **packed to a ~7 MB 1.6-bit artifact** through this loader — see *Real Model Compression* below.

See `include/cce/cce_safetensors.h` and `tests/test_cce_safetensors.c`. Full CNet structures (cascades + BTN + contracts) are in scope; the engine remains compositional/local.

### Learning Model

- A cascade (specialist "branch") is only adapted while its measured **goodness** is below threshold.
- Inside active cascades we use a hybrid with deeper support:
  - Stored activations per block for real forward.
  - Layer-wise local targets + "small decoder" proxies for hidden blocks (better deep credit on top of DFA/NoProp).
  - Cheap Forward-Forward negative contrast (anti-updates on random bad directions).
  - Per-block aggressive freezing (individual countdowns + EMA goodness) — now across deeper cascades.
- Online micro-split during router-learn adaptation for growth/exploration.
- Once frozen, the block (leaf) is removed from the learning graph entirely and can be contracted to WARM/COLD.

### Research Directions Integrated for Quality (no global backprop)

To improve learning quality while preserving the local/contract/non-monolithic design:

- **NoProp** (Li, Teh, Pascanu arXiv:2503.24322): each block independently learns to "denoise" toward local targets using only information local to the block. CCE adapt stores real activations and applies local error signals sized to each block's output dim (local credit, parallelizable in spirit).
- **Forward-Forward** (Hinton arXiv:2212.13345): every layer/block has a local goodness objective. Positive (real data) increases goodness; negative data decreases it. CCE implements cheap negative contrast (occasional anti-updates on random bad directions using stored activations) + per-block goodness.
- **DFA improvements + local targets**: fixed-size error vectors per block + scaled direct at output + projected random feedback for hiddens. Avoids the prior shared buffer bugs and weak credit.
- These keep the "local targets, contracts, goodness gating" paradigm and high throughput (~2M steps/s on synthetic).

**Scope note — where exact gradients are still used:** "no global backprop"
describes the contract-specialist path: primitives learn from local targets,
are certified, and are then frozen — nothing propagates gradients end-to-end
across composed parts. A few self-contained modules *inside* that system use
ordinary exact gradients where that is the cleanest engineering choice —
notably the factored word-LM head (`cce_wordlm`, below), whose tied
embedding/bottleneck would not learn under approximate DFA. These are
individual modules trained in isolation and then treated as frozen parts, not
a return to end-to-end monolith training.

See `src/cce/cce_learn.c` for implementation notes and `tests/cce_train_bench.c` for stats.

### Routing

`cce_router` implements **SSMax** (a sparse, temperature-controlled variant of softmax):

1. Compute score per branch = (centroid similarity) + (goodness).
2. Run SSMax → sparse distribution.
3. Route to top-k (usually top-1 or top-3) branches.

This gives cheap, sparse, and interpretable dispatch.

### Current Status & How Well It Works

**Verified working (latest, post NoProp+FF+fixes):**

- `cce_tensor` (tiled GEMM) + `cce_block` (now uses tiled in forward) + `cce_cascade` → fully functional
- `cce_archive` (mmap + in-memory directory + zero-copy views) → works
- `cce_forest` + `cce_router` (real SSMax) → works end-to-end
- `cce_learn`: stored activations, per-block local error + aggressive freeze, dim-correct DFA, cheap FF negative, NoProp-inspired local targets. Multi-run bench + held-out eval present.

**Where CCE is meant to shine (and feel lighter than PyTorch):**

- Small/medium local learning loops
- Low-latency embedded inference
- Many small specialists
- Online adaptation
- Environments where bundling Python/PyTorch is unacceptable
- CPU-only deployments where startup time and memory footprint matter

CCE is **not** beating PyTorch globally. It provides PyTorch-*like* usability for the compositional/local-learning niche where a full monolithic framework is structurally heavy.

**Where TensorFlow is still orders of magnitude ahead:**

- Breadth and maturity of autograd
- GPU/TPU + distributed + XLA and "Just works" for research-style large differentiable programs
- Production infrastructure at scale

(We acknowledge these gaps; CCE focuses on the complementary niche rather than copying the full stack.)

---

**Real generative task (TinyStories, 2026-06):**
- CCE cascades with explicit context windows (CTX=8, HID=128, 6-block) trained on real narrative data (~60+ short stories with heavy augmentation + fetch path).
- Three generation modes exercised:
  - **Word-level free-running** (real-word sentences): a CCE softmax head over the previous `WCTX` words predicts the next word, so every emitted token is a real word and the model only has to learn word transitions. Free-runs into complete sentences, e.g. `Once upon a time there was a curious fox who found a glowing mushroom.` This is the mode to use when you want an actual start-to-end sentence.
  - **CCE-guided char continuation**: the char cascade scores candidate next characters drawn only from actual continuations present in the training set after the current suffix. Coherent, but note it only *ranks* corpus continuations — so it is not a clean test of the model itself.
  - **Raw unconstrained char sampling**: pure logits from the char CCE head (no candidate restriction), low-temperature nucleus sampling with a score-based repetition penalty. Now produces genuinely coherent, English-shaped prose with real function words (e.g. `Once upon a time lory bag lingond wat sillden oright and.`) rather than the random characters of earlier versions (`Once puibrjdFapAh.Igrjzbt ...`). Pseudo-words remain — a fundamental capacity limit of a tiny char-LM, which is exactly why the word-level mode exists.
- Demonstrates that local (non-backprop) CCE learning can be driven on real sequence data and free-run coherently once trained with the right objective.
- **Forest + router over the LMs (branches & leaves):** both trained cascades are registered as `cce_forest` branch specialists (each a leaf-block cascade — `char_lm` and `word_lm`), given distinct centroids, and a `cce_router` selects which branch to free-run per query (a "char-ish" query routes to `char_lm`, a "word-ish" query routes to `word_lm`, which then emits the real-word sentence). Shows the branch/leaf/router machinery driving the actual LM specialists, not just the synthetic bench tasks.

**Breaking the O(V²) word-vocab wall (`make wordlm`):** a naive one-hot word head is `[(ctx·V) × V]` = O(ctx·V²) parameters — ~7.5 B (30 GB) at a 50k vocab, infeasible. `src/cce/cce_wordlm.c` factors it so every part is at most O(V·d):
  - **tied embedding** `E:[V×d]` shared across all context slots,
  - **bottleneck hidden** `W1:[hid × ctx·d]` (the low-rank factor that removes the input-side V),
  - **class-factored softmax** over ~√V classes + per-class word heads (sub-linear output),
  - trained with an **exact gradient (full backprop) + Adam**, not the approximate DFA — so the embedding/bottleneck actually learns. `cce_wordlm_gradcheck` verifies analytic vs numeric gradients (≈1.2e-2).

  Measured: at V=50000 the factored model is **9.7 M params vs 7.5 B for the dense head (773× smaller, linear in V)**; the demo trains on a 60-sentence corpus and free-runs `Once upon a time there was a curious fox who found a glowing mushroom in the forest.` What this does *not* fix is data sparsity — a large vocab still needs a real corpus; the tied embedding just uses the available data efficiently. Run with `make wordlm` (`WLM_EPOCHS=N` to override).

**Reusing a generation skill as a frozen contract primitive (`make endgate`):** rather than baking "when to end a sentence" into a monolith, the *sentence terminator* is split out as a standalone BTN primitive with a typed, tagged contract — `last_word : ONEHOT[V] -> sentence_end : ONEHOT[2]`. The split is by **decidability**, which maps onto the two contract families:
  - the **decidable per-word end-rule** is certified *exactly* — `btn_certify` replays every exemplar and reproduces it (demo: 297/297 → CERTIFIED), then the primitive can be frozen and reused;
  - the **residual uncertainty** gets a distribution-free guarantee from split conformal (`src/contract/conformal.c`): accept iff the prediction set is a singleton, else **abstain** and defer to the LM (demo: empirical coverage 0.952 ≥ 0.90 target).

  The same frozen primitive then drives termination for two different generators (a word-LM-style and a char-LM-style caller both name their last word). This is the composition thesis applied to generation: small *decidable* skills become frozen, certified, reusable primitives; the big O(V·d) tables stay as modules. Run with `make endgate`.

**What made generation work (2026-06):** fixes isolated to the CCE LM path:
  1. **Train on every sample.** `cce_train_dynamic` accumulated a per-batch error that was never used — it called `cce_learner_adapt` once per batch on only the *last* sample's input, so 7 of every 8 pairs never drove a weight update. The loop now adapts on every sample.
  2. **Softmax cross-entropy output** (new `classify` flag on `cce_train_dynamic` / `cce_learner`). One-hot targets trained under MSE barely converge (a linear head plateaus near the trivial all-zeros score); cross-entropy is well conditioned and lets the model actually predict the next token. Regression callers (bench, build demo) pass `classify=0` and are unchanged. Per-output stack buffers were widened (128 → `CCE_MAX_OUT`=512) so word-level heads (vocab in the hundreds) fit.
  3. **Word-level mode** for guaranteed real words (above) — a char-LM at this scale cannot free-run into clean real words, so whole-word modeling is the right representation for sentence output.
  4. **Sane decoder + LR.** Adam LR 0.05 (oscillates) → 0.01; the raw sampler treats head outputs as real logits with a score-based repetition penalty, replacing the old "anti-repeat" that substituted an arbitrary token nearly every step.
  5. **~13× faster training.** Adam's bias-correction `powf(beta, t)` was recomputed *per weight* in the inner loop though it depends only on `t`; hoisting it out (numerically identical) cut a 30-epoch char run from ~100s to ~8s, which is what made longer/word-level runs practical.

Env knobs: `CNET_TS_HIDDEN` (char cascade depth), `CNET_TS_EPOCHS`, `CNET_TS_LR`, `CNET_TS_WEPOCHS` (word model), `CNET_TS_RAW_TEMP`/`CNET_TS_RAW_PENALTY`, `CNET_CCE_VERBOSE`.

**Benchmark (deeper 4-block synthetic + real harness extensions, 5 seeds, 5000 steps/run):**
- ~1.156M ± 33k steps/sec on 4-block deeper cascades (high throughput retained).
- Held-out RMSE 0.519 ± 0.061; best RMSE seen across runs ~0.077-0.166.
- Consistent freezing across all layers (`frozen=4.0/4`).
- Real harness adds accuracy metric, spatial/patch tasks (nonlinear targets), persist timing. Multi-specialist via forest branches + router-learn loop.
- Per-block (layer) goodness/freeze + online micro-split support.
- See `make cce_train_bench` for live output including harness.

`make cce_smoke` and `make cce_train_bench` (runs on build). Dedicated forest/archive tests pass. cce_minimal.c isolates core.

Some advanced areas still need work:
- ~~Full OpenCL kernel dispatch~~ **done** for the transformer forward (`cce_clgemm`, 2026-07): dynamic loading, runtime kernel build, now **multi-GPU + int8** (see *GPU Forward*). Bit-identity proven by `clgemm_unit`, not asserted. The old `cce_gpu.c` CUDA path never actually compiled (a Makefile typo hid it) — recorded honestly, superseded.
- Zero-copy WARM **whole-cascade** load: `cce_cascade_view_from_archive` builds a read-only cascade whose block weights/bias are views into the archive mmap (`owns_memory=0`), reusing the mapped pointer instead of reload+copy (`make cce_view`: bit-identical forward + clean free). **Now wired into the forest** (`make forest_view`, 19/19): `cce_forest_seal` + `cce_forest_forward` materialize a branch as a zero-copy view when the forest is sealed (recall reuses the same pointer, no reload), with **copy-on-write to an owned HOT cascade** when training (`cce_forest_promote_to_hot`). Sealing forbids further `add_branch` (an append remaps the mmap and would invalidate views). Latent bugs fixed along the way: the copy loader read a fixed 1 MB prefix (failed on any archive < 1 MB), and `archive_offset==0` was misused as a "not persisted" sentinel (the first branch legitimately lands at offset 0) — now an explicit `persisted` flag.
- Larger-scale real tasks (beyond current harness) + full growth / per-layer curve logging

Core (partial loading, specialist forests/branches + leaf blocks, sparse routing, per-block contract freezing + improved layer-wise local learning) is solid and actively deepened.

See `tests/cce_smoke.c`, `tests/cce_train_bench.c` (4-block **deeper cascades** + real task harness: nonlinear + spatial/patch + accuracy + multi-specialist + persist timing), `tests/cce_minimal.c`, `tests/cce_forest_test.c`, `tests/cce_archive_test.c` and `include/cce/*.h`.

**Recent depth work (2026-06) — branches and leaves + perceptual sub-forests:**
- Perceptual leaves (esp. weak 7-seg) can now be CCE forests with sub-branches (general + ambiguity specialists) and internal cascades. Richer effective margin (router+goodness) + PORT_EVIDENCE output for better abstention at the representation wall.
- Generalized helper (cce_perceptual_leaf) for glyph/grid/block too.
- CCE leaf path is now default in habitat.
- See src/cce/cce_perceptual_leaf.* and glyph_habitat updates.
- **Branches (cce_forest specialists / cascades) + leaves (terminal blocks):** Forest now manages branches (cce_branch entries with cce_cascade specialists). Cascades are chains of "leaf" blocks (linear + patch). Full support for deeper specialist trees.
- **Deeper cascades:** Default 4-block (easily 6-8+) in `cce_train_bench`. Per-block freeze across all layers; forward/learn handle variable depth.
- **Real task harness:** Nonlinear targets, classification proxy (accuracy metric added to BenchStats), multi-specialist routing demo (via ABI `cce_adapt` + forest router-learn loop). Spatial/patch tasks supported.
- **Full header-based dir + zero-copy WARM cascades:** Archive now uses fixed header at offset 0 (`CCE1` + dir_offset/size) for reliable reload. Improved `load_dir_if_present` + `read_raw`. WARM tier uses archive offsets (zero-copy views for tensors; whole-cascade zero-copy via `cce_cascade_view_from_archive`, or owned copy via `cce_cascade_load_from_archive`).
- **Better deep credit:** Layer-wise local targets + "small decoder" proxy in hidden blocks (NoProp/TP flavor on top of DFA). Per-layer error magnitude drives local goodness.
- **Online forest growth + micro-split:** In router-learn loop (`cce_abi.c:cce_adapt`) and harness: occasional `cce_cascade_micro_split` on low-goodness blocks during adaptation. Supports dynamic deepening of specialists.
- **Patch blocks wired:** `cce_block_forward` supports `CCE_BLOCK_PATCH`. New spatial "real" task in bench (synthetic images → patch_extract → patch block cascade + nonlinear target + accuracy).
- **More stats:** Accuracy metric, persist save/load timing, per-block (layer) goodness/ freeze tracking, deeper 4/4 reporting. `cce_train_bench` now surfaces harness results.

**Real sequence modeling milestone (TinyStories / CCE as LM engine):**
- Dedicated `test_tinystories` exercises CCE on real narrative next-token prediction with context windows.
- Guided mode delivers coherent output; raw logits mode now free-runs into coherent prose after the softmax-CE + all-samples + decoder fixes (see "What made raw generation work" above).
- The same CCE technique (contextual cascades + local credit) has been wired into the reusable `cnet_lm` path so that `train lm` / `--demo LLM` can use CCE cascades as the core step model.
- (Historical note: the advertised `CCE_USE_CUDA=1` path never actually compiled — a Makefile typo kept it off. GPU acceleration is now real via the OpenCL `cce_clgemm` forward path instead; training remains CPU.)

See `test_tinystories.c`, `src/cnet_lm.c`, and the build console `train lm`.

See updated `tests/cce_train_bench.c` (real harness + spatial + accuracy) and `src/cce/` for the implementations.

---

# How It Works

## The Primitives

`./nn_demo` trains every primitive from synthetic data and freezes it to a
weight file. Training uses dynamic hidden-neuron growth: each network starts
small and adds neurons when loss stalls, so capacity is learned, not chosen
up front.

| Primitive | Contract | Function |
|-----------|----------|----------|
| nibble recognizer | 4 bits → hex digit | the original demo net |
| `increment` | 4-bit value → 5-bit value | binary increment |
| `hex_value` | one-hot hex digit → 4-bit value | symbol to value |
| `combine` | two 4-bit nibbles → byte | nibble-pair join |
| `split` | byte → two 4-bit nibbles | multi-output decomposition |
| `conditional_increment` | 1-bit flag + 4-bit value → 5-bit value | heterogeneous inputs |
| hex chars / words | hex pairs → ASCII → word tokens | a small decode pipeline |

The pipeline demo decodes binary text through frozen layers. The response (word token) can be produced by a RoutePlan over the contract primitives (see src/main.c "Contract-based response path").

**Contract-based perceptual + symbolic response (working solution milestone):** The system now produces coherent responses from given text (glyphs or symbolic queries) entirely through the contract-based machinery. Glyph "text" is perceived via contract/port-verified leaves (margin snap + route_execute). Symbols feed the perceptual_query orchestrator, which intelligently selects and composes sub-contracts (math, simple add, compound, branching narrative, or abstain) using CNET-D steering. Responses can iterate in diffusion-style passes (skeleton → refinement → reflection), branch on choices, recall persisted memory, mint reusable chunks with recipes, and evolve (CNET-D + beneficial flags). 5B/6A add EVIDENCE (top-k distribs for late binding, less info loss on ambiguous leaves) and CONCEPT (auto-abstracted hierarchical discrete symbols that shorten plans and tame port narrowness/combinatorial growth). The full episode ends with self-reflection and a status table. See `tests/glyph_habitat.c --demo 5A` (and --demo 5B / --demo 6A) for the capstone: multiple queries produce certified coherent narratives with traces, using all prior increments (3C–6A). Run `make decimal && ./glyph_habitat --demo 5A` to see the complete arc from 7+5=12 to a generative agent. 5B/6A directly attack the "Knowing the Edge" trade-offs (info loss, explosion, narrow ports) without breaking prior slices.

```text
0100000101000010 → 0100 0001 0100 0010 → 41 42 → AB
```

A flat raw-binary word model trains alongside the decomposed path so you can
compare direct memorization against composition.

## Interface Contracts (Ports)

Every primitive carries typed input and output ports (up to 8 of each). A port
specifies:

- **family** — `onehot`, `binary_msb`, `binary_lsb`, `raw` (legacy), `evidence` (5B: top-k + weights distrib for late binding / nuance without early snap), or `concept` (6A: auto-abstracted higher discrete symbols for hierarchical compression).
- **field_width × field_count** — representation shape, e.g. two one-hot-16 fields.
- **semantic tag** (optional) — names the *meaning* (`hex_digit`, `nibble_value`,
  `dec_symbol`). When both sides of a handoff carry tags, they must match; an
  untagged port is a wildcard. Tags stop representation-identical but
  semantically different primitives from interchanging.

Executors enforce contracts at run time: every handoff is **validated** against
the consuming port's domain and then **canonicalized** (one-hot → argmax, binary
→ threshold) before the next forward pass. An ambiguous value is an error, not
something to round away. This snap-to-domain at each boundary is what keeps
errors from compounding through deep plans — the same way digital logic restores
signal levels between gates.

### 5B + 6A: Knowing the Edge mitigations
- **Evidence ports (5B)**: instead of forcing a hard one-hot snap on low-margin leaf outputs, carry a small finite support distribution (top-k normalized). Late binding lets downstream contracts (math, narrative) consume the nuance; recovers information that would have been lost. Used in interactive agent for ambiguous perceptual math queries.
- **Concept ports (6A)**: recurring motifs auto-abstract to a higher discrete CONCEPT symbol (hierarchical). A "forest_trick" concept stands in for a sub-DAG; planner uses 1-hop instead of re-expanding. Cuts both combinatorial explosion (fewer candidates) and port narrowness (richer abstractions over raw symbols).
- Either path (or combined) improves the system measurably while preserving exact prior behavior on onehot/binary paths. Demos: `--demo 5B`, `--demo 6A`. Full table in `--demo 5A` output.

### MCP external knowledge + separate memory layer
The interactive agent uses multiple MCP-style tool contracts:
- **Wiki** (`port_contract_mcp_wiki_lookup`): specific Wikipedia summaries.
- **Web Search** (`port_contract_mcp_web_search`): general search (abstract + topics), cached.
- **File Read** (`port_contract_mcp_file_read`): load local files (e.g. books for ingest), cached.
- **File Write** (`port_contract_mcp_file_write`): persist build artifacts / reports (used heavily by build console).
- **Calculator** & **Summarizer**: arithmetic + text summarization tools.
All check (and populate) the agent memory layer first. Tools compose with book distillation to `PORT_CONCEPT`, narrative contracts, streaming, and the new internal LM.
Demo: `make glyph_habitat && ./glyph_habitat --demo MCP` or use in `--interactive`.

### Agentic Memory (Chat History + Internal Thinking + Past Sessions)
At the agent level we now have a dedicated memory system that the interactive agent owns:

- **Chat history**: every user query and assistant response is recorded.
- **Internal thinking**: the agent produces private `[THOUGHT]` entries (chain-of-thought / reflection) that are stored separately but can be included in context.
- **Past sessions**: `agent_memory_init()` loads from the single compact `cnet_knowledge_base.bin` (legacy agent_*.md / .txt are migrated once if found on first init). No new raw .md memory files are created. History grows across invocations.
- **Live streaming (8A+)**: When `reg->streamer` is set (default in --interactive), narrative passes emit "skeleton", "refine", "reflect" tags in real time via the DAG executor handoffs. You see the agent *drafting live* instead of only the final text. Standard token stream is also possible by post-processing the final committed output.
- The agent uses recent context when deciding how to answer and records new thoughts before responding.
- Demo: `make glyph_habitat && ./glyph_habitat --demo AGENT`
- This gives the system real "remember past sessions" behavior without touching the neural primitive or MCP layers.

### Own Internal Generative Models (LLM-type Logic We Can Train Inside)
CNET now includes first-class support for training our *own* generative models entirely inside the system. The primary engine is now the pure-C **Contract Cascade Engine (CCE)**:

- Trainable next-token (or next-concept) predictors using CCE cascades with explicit context windows (CTX) instead of single-token BTNs.
- Two practical generation modes:
  - **CCE-guided**: the trained cascade selects among real continuations present in the training data → coherent narrative output.
  - **Raw unconstrained**: temperature + nucleus sampling directly from the CCE head logits (increasingly non-degenerate with scale).
- Same contract / persist / certification story; CCE cascades are first-class citizens in `cnet_lm`.
- Commands: `train lm` (or "train llm", "own model") in the interactive consoles; `--demo LLM`.
- Explicitly "no external LLM" — this is CNET-native next-symbol logic we fully own, now powered by CCE.

See `test_tinystories` (the real-data A/B demo), `./glyph_habitat --demo LLM`, `src/cnet_lm.c`, and the build console `train lm`.

### Training Contracts from External Data (Hugging Face)
The contract training pipeline can consume data from https://huggingface.co/datasets (and local files):

- Same pattern used for speech contracts (`--demo SPEECH`): fetch labels via curl-style source, build feature/target tables, `btn_train_dynamic`, emit contract + weights.
- Extensible to text (fable traces already used), speech commands, or any tabular next-token data.
- Artifacts (contracts, weights, reports) written to `build/` via MCP or direct.
- Demos: `--demo SPEECH`, `--demo LLM`, `train speech`, `train lm`.
- Combines with agent memory, distillation to `PORT_CONCEPT`, and the planner.

This turns HF into a source for growing CNET's own verifiable primitives.

## The Planner

The planner reads only contracts and reliability statistics — no training data,
no execution.

- **Routes** (`route_plan` / `route_execute`): linear chains over single-input
  primitives, found by exact hop-capped dynamic programming over port types.
- **DAGs** (`dag_plan` / `dag_execute`): multi-input compositions over typed
  sources, found by complete AND-OR branch-and-bound search. The planner assigns
  sources to slots, recurses into sub-goals, and can select any segment of a
  multi-output primitive (projection).
- **Circuits** (`dag_plan_circuit`): several goals satisfied by one coordinated
  structure with port-disjoint node sharing (see *Circuit Plans*).

Both planners maximize the **product of step reliabilities** over the whole
plan. With no recorded evidence every primitive scores the same, so fewer hops
win and planning degenerates to shortest-path — but real evidence can justify a
reliable detour over a flaky shortcut. Primitive expansion per slot is
**beam-limited** to the top-N reliability-ranked candidates (`dag_beam_limit`,
default 8); a reachability memo prunes provably-dead branches. Both knobs trade
nothing on small registries and matter at scale (see *Knowing the Edge →
Planner scaling*).

## Learned Reliability

Executors validate each primitive's raw output (every segment) before snapping
it, and record the outcome in success/failure counters. The score is
Laplace-smoothed: `(s + 1) / (s + f + 2)` — a fresh primitive scores 0.5,
evidence moves it toward its observed validity rate.

- Evidence persists in per-primitive sidecar files (`<name>_stats.txt`,
  `CNET_STATS 1`), never in the weight file: the weight file stays a pure
  function definition, the sidecar is deployment experience. Retraining a
  primitive invalidates its sidecar.
- **Strict mode** (`plan.strict = 1`, opt-in after planning) aborts a run on any
  out-of-domain raw output instead of snapping it. Evidence is recorded either way.

## Machine-Checkable Contracts

A contract makes a tag's meaning checkable instead of trusted: a contract file
(`CNET_CONTRACT 2`) holds a named transform's full port signature — tags
included — plus an exemplar table over canonical values. `btn_certify` replays
the exemplars through a frozen net and grants or denies the claim:

- **Signature gate** — ports must match the contract exactly, tags included.
  Certification is what entitles a primitive to wear the contract's tags.
- **Behavior gate** — every exemplar must replay exactly: raw output in-domain on
  every port, canonical output identical to the spec.

Certification runs on demand and is never persisted (a replay is cheap; a
certificate file would reintroduce staleness). `registry_add_certified` refuses
imposters outright, and `reg.require_certified = 1` makes both planners consider
only certified entries — a returned plan is then certified end-to-end.

### Content identity, cache, seals, audit (`make contract_secure`)

The trust layer is content-addressed (same move as the weight store):

- **Behavior-only digests** — `contract_btn_digest` covers exactly what
  determines behavior (counts, port signatures, ternary settings, live
  weights); evidence counters and learning rate are excluded, so accruing
  evidence never invalidates a certification.
- **Certification cache** — `btn_certify` memoizes in-process keyed by
  (btn digest, contract digest). Both digests cover every input the verdict
  depends on and replay is deterministic, so a hit *is* the replay verdict.
  Measured: 2000 repeat certifications in ~1 ms with 2 replays. Any weight or
  exemplar change alters the key — invalidation is content-driven.
- **Sealed files** — contract files carry a trailing `SEAL` (FNV over the
  semantic content); `contract_load` refuses a mismatch, so a tampered
  exemplar that stays per-value canonical is still caught. Legacy v1 files
  load flagged `seal_verified = 0`. (Integrity, not authentication: no secret.)
- **Certificate ↔ weights binding** — every grant site records the certified
  btn's digest on the registry entry; `registry_audit_certified` demotes any
  entry whose weights changed since (certified cleared, state RESET — heal is
  the only way back). A stale certificate is never trusted.

### Unit files: weights + contract as one artifact (`make contract_unit`)

`registry_save` persists each primitive as ONE sealed binary unit
(`<name>.cnu`, format `CNU1`) instead of the old `.btn` + `.contract` text
pair: binary f64 weights (live slots only, bit-exact) plus **bit-packed
exemplars** (canonical 0/1 values → 1 bit each — format-aware packing beats a
generic codec), sealed with FNV verified **before parsing**. Measured 3.0×
smaller than the text pair. `unit_save` refuses incoherent units (btn/contract
signature mismatch, non-canonical values); `unit_load` round-trips
bit-identically (gated by digest equality) and the loaded unit certifies
against its own embedded contract. `.stats` / `.expansion` sidecars stay
separate on purpose — evidence and recipes have their own lifecycle.

Two primitives can replay every exemplar exactly yet sit at different distances
from the canonicalization boundary — equally *correct*, unequally *robust*.
`btn_certify` reports that worst-case headroom (`CertifyReport.min_margin`), and
`btn_certify_robust(btn, c, floor, …)` raises the bar from "not ambiguous" to
"has margin ≥ floor" over the same exact-replay mechanism. Promotion reuses
those reports: `contract_better_if` prefers the larger minimum margin and only
uses deployment reliability as a tie-break. It does not replay either model a
second time. `contract_init_borrowed` and `contract_init_frozen` validate port
shape and every exemplar before atomically publishing a contract; malformed or
ambiguous tables leave the destination untouched. Text contract persistence writes
17 significant digits and round-trips finite soft `PORT_RAW` vectors and
normalized finite `PORT_EVIDENCE` distributions bit-identically under the
semantic seal; non-finite values and soft values on discrete port families are
refused. (`.cnu` unit exemplars remain bit-packed canonical 0/1 by design.)
Run `make contract_optimized` for the focused structural benchmark plus security,
unit-file, and historical native regressions.

## Property Contracts (Laws)

Exemplar contracts certify one transform; property contracts state relations
*between* transforms. A `<name>_property.txt` file (`CNET_PROPERTY 1`) holds an
equation: a typed source signature and two chains of primitive names.
`property_check` type-checks every seam statically, enumerates the canonical
source domain, and strictly executes both chains on every input:

```text
split_inverts_combine : HOLDS (256/256)
perturbed combine     : VIOLATED (held 240/256)  — a broken retrain
                        cannot hide from the algebra
```

Laws are the regression net for retraining: a wrong `combine` violates
`split(combine(hi, lo)) = identity` even if its own exemplar contract was never
written.

## Generative Narrative & Interactive Agent (3G–5A)

The contract system now supports end-to-end generation of coherent responses
from text. The `perceptual_query` orchestrator scores and selects among
sub-contracts (math, simple/compound add, narrative diffusion, branching, abstain)
while applying CNET-D influence. Diffusion-style passes (skeleton → refinement →
reflection) can branch on choices; successful episodes auto-mint reusable
narrative chunks with recipes. The interactive agent (5A) accepts natural
queries, recalls persisted memory (e.g. prior stories), produces certified
multi-sentence output with trace, and emits a night summary table.

All of the above reuses the same planner, dual-track, persistence, margin
abstention, and evolution machinery. Run `make glyph_habitat --demo 5A` (after
`make decimal`) for the full episode.

## Chunk Consolidation

A proven plan distills into a single new primitive (`consolidate_route` /
`consolidate_dag` / `consolidate_circuit`):

1. Enumerate the plan's canonical input domain (refused if `raw` or over 4096 samples).
2. Label each input by executing the plan as a *strict* teacher; member counters
   are snapshotted and restored — distillation is not deployment experience.
3. Train a student network on the labeled set.
4. Verify the student against the teacher over the whole domain. Below threshold
   (default 100%), consolidation refuses and registers nothing.
5. Seed the chunk's reliability counters with the verification outcome.

The chunk is an ordinary primitive, so the planner needs no changes to prefer it:

```text
teacher chain (2 hops, score 0.2500): hex_value increment
  distilled: 16 samples, verified 16/16
replanned (1 hop, score 0.9444): hex_increment
```

Chunks compose like any primitive, so consolidation recurses: a proven plan
*through* a chunk distills again into a chunk of a chunk. `contract_from_*`
emitted from the same teacher plan certifies the distilled chunk by construction.

## Library Evolution

`library_evolve` (`src/library.c`, `make library`) runs consolidation in a loop:
given a list of tasks (typed source signature + goal), it plans each, distills
every plan that compresses two or more primitives, emits the chunk's contract
from the proven plan, and registers it certified. It ends at a **fixed point**
(a pass that invents nothing), and **compounds** — a chunk invented for one task
shortens another's plan.

With dual-track (3C), a compute-heavy chunk also captures its teacher recipe
(names + costs) at mint time. Under LOW power the planner can selectively
expand the chunk back into its lean teachers. `finalize_chunk` (in library.c)
now records the recipe for heavy chunks; the registry persists it as a
`.expansion` sidecar.

The library + orchestrator have been extended (3D–5A) to support minted
narrative chunks and interactive agents that generate coherent multi-step
stories from text queries, with CNET-D steering, reflection, branching,
auto-minting on patterns, and persistence. See glyph_habitat --demo 5A.

## Neural Lifecycle (states, self-healing, shadow, cost + dual-track)

Reliability gives a primitive a *score* and a contract gives it a *proof*; the
lifecycle gives it an explicit **state** and four runtime behaviors that act on
it. Each `RegistryEntry` carries a `PrimitiveState`:

```text
FUZZY ──evidence──▶ PROVISIONAL ──btn_certify──▶ FROZEN ⇄ RESET
                                                  (RESET on a runtime fault;
                                                   back to FROZEN only via
                                                   registry_heal — retrain + re-certify)
```

- **FUZZY** — registered, uncertified. **PROVISIONAL** — accruing positive
  evidence (`lifecycle_promote_provisional`: reliability ≥ 0.9 over ≥ 16
  outcomes). **FROZEN** — certified. **RESET** — failed a runtime invariant,
  excluded from planning. The *only* path into FROZEN is a passing `btn_certify`;
  evidence alone never freezes.

Everything is opt-in: `reg.lifecycle_enabled = 0` and `reg.power_mode = 0` (the
zero-init defaults) leave the planner and executors byte-identical to before.
RESET/shadow exclusion lives in one chokepoint — `entry_usable` skips those
entries only when `lifecycle_enabled` — so all three planners inherit it at once.

**Self-healing re-route.** `route_execute_healing` runs a plan strict; on an
out-of-domain fault it learns *which* primitive failed (`ExecFault`), marks it
RESET, re-plans around it (the RESET-skip excludes it), and retries — up to
`max_reroutes`. A localized failure becomes a transparent reroute, not a crash.

**Meltdown → retrain (continual learning, the safe way).** A frozen net is
deterministic, so the failures worth catching are out-of-contract edge cases —
where you have the input but no target. `registry_record_fault` parks the failing
input (RESET + an unlabeled retrain queue); a verified target is then obtained
three ways — the **contract** itself, a **teacher** primitive that handles the
input cleanly (`registry_label_via_teacher`), or an **external-oracle** hook
(`registry_supply_label`, where a perception model or a human plugs in). The wake
pass `registry_heal` retrains on the contract ∪ the *labeled* cases, re-certifies,
and restores FROZEN **only on a passing `btn_certify`** — never on a guess. No
verified target, no retrain: the primitive stays RESET and self-healing routes
around it. Continual learning that cannot silently corrupt the trusted core.

**Shadow execution.** A candidate registered against an active primitive
(`registry_set_shadow`) is excluded from planning but run beside the incumbent on
live inputs (`registry_run_shadows`), accruing reliability evidence without
touching any production output. `shadow_promote_if_ready` hot-swaps it in once it
out-scores the incumbent **and** certifies — promoting the candidate to FROZEN and
demoting the old active to RESET.

**Cost-aware polymorphism.** Among equal-reliability candidates the planner
prefers the cheaper one (`btn_cost`, a MAC estimate) when `reg.power_mode =
CNET_POWER_LOW` — a strict tiebreak (reliability is never traded for cost;
`power_mode = 0` is the byte-identical baseline).

**Dual-track expansion (3C A1).** A minted chunk can carry a lean *expansion recipe*
(the ordered names of the teacher primitives it replaced, plus 3A cost labels).
Under `CNET_POWER_LOW` + `expand_in_low_enabled`, a compute-heavy chunk whose
recipe is fully available is hidden from the planner; the existing search
re-composes the obligation from the lean teachers. 

- DEFAULT: the chunk wins (planning-cheap student).
- LOW (opt-in): the teacher sub-plan wins (more accurate + compute-cheap).

Recipes + costs are persisted in `<name>.expansion` sidecars. Over-cap teachers
get no recipe (graceful). The opt-in is zero-init off. See
[`docs/superpowers/specs/2026-06-19-dual-track-expansion-3c-design.md`](docs/superpowers/specs/2026-06-19-dual-track-expansion-3c-design.md).

Demonstrated result (compounding benchmark): LOW expansion recovers the lean
MAC count of an unchunked plan; DEFAULT keeps the ~4× planning reduction of the
chunk. The trade-off is selectable by power mode.

Deferred items (named, not hidden) live in the same lifecycle + cost design doc.

---

# Domains

## Decimal Arithmetic (zero core changes)

The domain-generality claim got its first real test: decimal digit arithmetic
was added with **zero core changes** — only new primitives, tags, contracts and
laws (`make decimal`). The planner discovers
`dec_full_add(dec_value(a), dec_value(b), cin)` unaided; the chunk distills
200/200, certifies against its teacher's contract, and wins the certified
replan. Two laws hold by strict replay: `dec_to_symbol(dec_value(s)) = identity`
and `add(a, b, c) = add(b, a, c)`. A decimal digit and a hex nibble share a
representation (`binary_msb 4`), so the `dec_digit` tag is the only thing keeping
hex values out of decimal adders — exactly the cross-domain safety tags were
built for.

Three findings worth recording: enumerable boundaries must be one-hot (binary
ports would enumerate invalid bit-patterns); multi-output circuits are programs,
not plans (ripple-carry is hand-wired and verified, not discovered as a tree);
and nonlinear primitives must *start wide enough* (a thin adder saturates short
of exact).

## Circuit Plans

Plans are true DAGs (`make circuit`): one primitive execution can feed several
consumers, and several goals can be satisfied by one structure.

- **Port-disjoint sharing.** A node may have multiple consumers only if each
  reads a *distinct output port*. Single-output primitives are never shared, so
  every pre-sharing plan is provably unchanged; what it permits is multi-output
  fan-out (`split`'s two nibbles, an adder's sum and carry).
- **Multi-root circuits.** For goals {sum0, sum1, carry} over two-digit sources,
  types plus the sharing rule *force* the ripple-carry topology — the planner
  discovers it, and the exhaustive strict sweep (all 20,000 two-digit additions)
  certifies the wiring.
- **Reachability pruning.** Both DAG planners precompute a type → minimum-depth
  table and kill branches that provably cannot complete. Plans are identical with
  the knob (`disable_plan_memo`) on or off. Measured on a 158-primitive trap
  registry: **1348 ms unpruned vs 0.02 ms pruned (~67,000×)**, same plan returned.

## The Learned-Leaf Habitat (current frontier, v3.0 → v4.4)

Every domain above is closed-form or hand-shaped — they validate the *harness*,
not the habitat for real *learned* perception. The habitat probe asks the hard
question:

> Can a genuinely **learned, uncertain** leaf be snapped through a finite typed
> port and stay usable downstream — or does its uncertainty poison the chain?

`make glyph_habitat` answers it. A learned BTN reads a noisy rendered digit and
emits a one-hot `dec_symbol`; a **margin gate** rejects near-boundary outputs
(converting uncertainty into *abstention*, not a wrong answer); accepted symbols
feed the existing formula primitives. The arc (v3.0→v3.9) established that
formula error stays ~0 when the snapped symbol is correct and the margin floor
gives a sane coverage/accuracy tradeoff. (The early "canonical beats raw/soft"
claim is *not* established here — the downstream is an integer evaluator with no
coherent soft mode, so the comparison is ill-posed; treat it as untested where it
would matter.)

**v4.0 → v4.4 — multi-domain transfer.** The same `dec_symbol` boundary now
carries **four** distinct raw perceptual families through one port:

| domain | features | role |
|--------|----------|------|
| glyph (5×7 font) | 35 | original |
| 7-segment (LED) | 7 | v4.0 second domain |
| 3×5 dot grid | 15 | v4.3 third domain |
| 4×4 block/silhouette | 16 | v4.4 fourth domain |

An **accepted-leak frontier** audits the boundary with raw counters, per-domain
leak rates, and a mix-pair matrix (all 10 composition pairs sampled). The
headline is not "no domain dominates" — it is **where the boundary stops being
fail-safe**:

- **The gate is fail-safe only to the degree a leaf's errors are low-margin.**
  The 7-segment leaf is the counterexample: at the noisy tail >50% of the symbols
  the gate *accepts as confident* are wrong (max accepted error ≈0.55). On 7-seg
  it isn't abstaining — it's passing confident-wrong through at near coin-flip
  rates. Every dominant confusion is a Hamming-1 font pair (8→6, 9→5, 6→5).
- **That confident-wrong is largely uncatchable** (`make sevenseg`). The
  discriminator settles the fork: the trained net's accepted-error sits *at or
  below* the model-free nearest-template floor (no overconfidence headroom), and
  5 independently trained leaves agree on the *same wrong digit* ~85–95% of the
  time. An ensemble-disagreement gate removes only ~12–19% of it. It is the
  information-loss wall at the leaf input — 7 features alias digit pairs under
  noise — not a model defect a gate can fix.
- The other three leaves are well-behaved (4×4 block is the cleanest, max ≈0.12);
  the mix-pair matrix localizes the damage to 7seg-involving pairs rather than
  hiding it in an aggregate.

**Frozen claim (fail-safe form):** four learned perceptual domains compose
through one finite `dec_symbol` boundary, and the boundary converts uncertainty
into abstention **exactly to the degree a leaf's errors are low-margin**. The
7-segment leaf is the named blind spot: its 7-feature code aliases under noise,
so it fails *confidently*, and the margin gate — and an ensemble — are blind to
it. Visibility is not mitigation; below the representational floor the confident-
wrong is the world, not the model.

---

# Knowledge Pipeline (PDF → corpus → memory → graduation)

A real-data arc on top of the frozen-primitive thesis: ingest documents, accumulate a
**dynamic memory that compounds across books**, and **graduate** stable regularities out of
the fuzzy memory into **certified, router-composable primitives** — closing the
non-monolithic loop (the "model" = a registry of proven primitives + a router + a memory,
never one net). Pure C, stdlib + libm, **zero core/router/contract edits** — every stage
consumes the existing public APIs. Each stage is its own self-checking target.

| Stage | Target | What it does |
|-------|--------|--------------|
| JSON as contract | `make jsonstory` | understand+generate JSON as a **PROVEN** contract over TinyStories: `json_escape_class`/`json_unescape_short` certified exhaustively, round-trip 256/256, `characters` via the `endgate` name-gate, story from `cce_wordlm` |
| PDF extraction | `make pdftest` | text-born extractor in pure C: embedded `inflate` (DEFLATE), `Tj/TJ` + kerning-space heuristic, **`/Differences` font recovery** (best-match decode — no `/ToUnicode` needed), `/Encrypt` detect-and-skip; 19 unit checks |
| Dynamic corpus | `make pdflearn [file.pdf …]` | extract → autosplit → **persistent append-only corpus** (exact-line dedup, uncapped) → replay-train `cce_wordlm`; re-ingest is idempotent, new books grow the store |
| Compounding retrieval | `make compound` | frozen LM + growing **kNN-LM** store: identical re-ingest = **100% reuse / 0 new keys**, related = partial, flat **O(book)** ingest vs growing **O(Σ)** retrain |
| Tiered tile memory | `make tiermem_test` | production fuzzy tile memory (AICIMO-adapted): HOT cap + **WARM disk spill** (bounded RAM / unbounded disk), **TF-IDF** term-dictionary recall, decay, persistent store |
| Graduation | `make graduate` | mine a near-deterministic regularity → two `btn_certify`'d **positionally-tagged** units → `registry_add_certified` → **evict** tiles from the fuzzy memory → the **router auto-composes** them (`route_plan(w_a→w_c)` discovers `[step_ab, step_bc]`). The loop, closed. |
| Extraction quality | `make fontdecode` | `/Differences` glyph→text recovery (ligatures `f_i`→`fi`) + English-likeness quality filter; on the sample book: 1410 junk sentences dropped, 10/11 ligature words recovered |
| TF-IDF retrieval | `make tfidf` | term-dictionary `term→df` + idf-weighted overlap search — fixes the rare-term recall miss (a "last mile" query now returns last-mile sentences) |
| Synonym retrieval | `make synonyms` | corpus **PPMI** same-tile co-occurrence map (`src/corpus/synonyms.c`) + opt-in query expansion in tile_memory; **discounted PPMI** (Pantel–Lin support factor) kills the rare-term/OCR neighbor noise (`customer→churn/profitability`, not `→satisffed`); `alpha=0` default = byte-identical TF-IDF. Closes the synonym gap — a zero-lexical-overlap query now hits (25/25) |
| Inverted index | `make tileindex` | unified **`term→tid` inverted index** over HOT+WARM tiles — search scores only candidate tiles (HOT in array order, WARM by byte offset), **byte-identical** to the kept linear oracle `tilemem_search_linear`; **18.8–162× faster** on selective queries (24/24) |
| Semantic consolidation | `make consolidate` | opt-in `tilemem_consolidate`: merge **paraphrase** tiles via synonym-aware soft-Jaccard (bidirectional `min`-coverage guard, subset-safe), candidates from the inverted index; rarest-first **candidate cap** bounds the pass to `O(n·k)` → ~3 s on the book (~41× over the uncapped scan) (17/17) |

**Pipeline lineage:** PDF → dynamic corpus → compounding retrieval → tiered tile memory →
graduation back into the contract/router core. The fuzzy memory is the *soft* store;
graduation moves proven knowledge *soft → hard* into the certified library. Five
retrieval-quality levers stack on the tile memory — **font-encoding recovery** (`fontdecode`)
and **idf-weighted search** (`tfidf`) for lexical quality, a **PPMI synonym layer**
(`synonyms`) that closes the zero-overlap gap, a **unified inverted index** (`tileindex`) that
makes search candidate-only *without changing results*, and opt-in **semantic consolidation**
(`consolidate`) that merges paraphrases so the store *compounds* rather than accumulates.
Every lever is opt-in and byte-identical-or-additive to prior behavior; **zero
core/router/contract edits** throughout. Honest edge: the synonym signal is same-tile
*topical* relatedness (not strict synonymy), and consolidation is conservative + HOT-only —
both bounded by corpus size and extraction quality, not perfect. Design rationale for each
stage is in `docs/superpowers/specs/2026-06-26-*.md` and `…/2026-06-27-*.md`.

---

# Real Model Compression (HF Supra → decomposed CNET → packed 1.6-bit)

A full real-world arc on top of the decomposition thesis: take a pretrained 119 MB Hugging
Face transformer, **decompose it into independent CNET specialists**, run it with a **bit-exact**
pure-C forward, then walk a **quantization ladder** down to an actual **~7 MB reloadable 1.6-bit
artifact** — and pin down exactly where the line between *storage* and *quality* sits.

**The model.** `SupraLabs/Supra-A2A-Nano-Exp` (4 layers, 256-dim, 4-head, vocab 50520) loads via
the header-first safetensors loader, **downloaded once into `./supra_cache/` and reused every
session**. Every linear projection (qkv / attn-proj / mlp / head) becomes its own frozen
`cce_block` specialist in a `cce_forest`; embeddings, LayerNorms and the VQ codebook load as
first-class tensors — *no monolithic model object*. `make supra_console`, `make cce_safetensors_test`.

**Verified, not hand-waved.** The pure-C forward (`cce_supra_gpt_forward`) matches a numpy/float64
reference **exactly** (identical top-8 logits + per-layer checksums, `max|Δlogit| ≈ 2e-5`); the
byte-level BPE tokenizer matches Hugging Face `tokenizers` **6/6** and round-trips **6/6**. Greedy
decode is coherent: *"Once upon a time there was a little girl named Lily. She loved to play
outside and explore the world around her."*

**Speed (CPU, single binary): 4.7 → 256 tok/s (~54×), and flat O(T).**
- vectorizable mat-vec in `cce_block` (i-outer/o-inner, **bit-identical**) → SIMD; AVX re-enabled
  for the CCE/Supra targets (the `-mno-avx` workaround is router-only),
- a **KV cache** (incremental decode, O(T²)→O(T); greedy output 64/64 identical to the batched forward),
- OpenMP over the 50520-wide head.

**The quantization ladder (measured):**

| representation | size | quality |
|---|---|---|
| Original checkpoint | 118.9 MB | — |
| CCE FP specialists | 64.4 MB | exact |
| **int8 weight-only PTQ** (`--int8`) | **16.3 MB** | **near-lossless** (cosine 0.99999, greedy 64/64 identical) |
| post-hoc ternary | ~6 MB | **collapses** (cosine 0.83, argmax flips) |
| **packed 1.6-bit (trits)** | **6.95 MB, actual on disk** | **bit-exact vs ternary** — storage proven; quality = QAT |

int8 PTQ is the shippable compression result today. Ternary is **BitNet b1.58** (per-row absmean,
`{-1,0,+1}`, 5 trits/byte = 1.6 bit/weight): the **packing** is bit-exact and the **6.95 MB artifact
reloads end-to-end** (`cce_supra_export_packed` / `cce_supra_load_packed`, **no FP big tensors**) —
but **post-hoc ternary on a full-precision-trained model collapses quality**. That is the known
BitNet result, and recovering it needs *quantization-aware training*.

**QAT works — proven at LM scale (`make wordlm_bitnet`).** The `cce_wordlm` word-LM trained three
ways on one corpus: **FP perplexity 1.41, QAT-ternary 1.95 (coherent, sample identical to FP),
post-hoc ternary 61.5 (broken).** Ternary BitLinear (FP shadow weights + straight-through estimator)
recovers what post-hoc throws away. A four-way (linear / embedding / both) localizes the cost to the
*linear* matrices — ternary embedding is essentially free. The trained model **trit-packs to a real
1.6-bit artifact and reloads bit-exact** (`max|ΔNLL| = 0`). `make bitnet_qat` is the self-contained
STE demonstration (ring task: FP 99.8% vs QAT 82.2% vs post-hoc 64.2% = chance).

**Honest line.** Storage/format down to ~7 MB is *proven and reloadable*; deployable Supra quality
is gated on a **Supra QAT trainer** (the Supra forward is inference-only, so joint QAT needs a
transformer backward). The build plan — starting with a *head-only* smoke test that needs **no
transformer backward** (freeze the transformer, cache hidden states, train a standalone ternary
head) — is scoped in
[`docs/superpowers/specs/2026-06-28-supra-qat-scope.md`](docs/superpowers/specs/2026-06-28-supra-qat-scope.md).
The trit runtime kernel is LUT-decoded, tiled and OpenMP-threaded (27.9× over the original
serial kernel on the 256×50520 head: 6.14 → 0.22 ms/forward — now 2× *faster* than FP32 at 20×
smaller), and stays **bit-identical** to the int8 ternary path (gated in `make trit_bench`,
which memcmps the full output). int8 remains the outright speed path (1.57× faster than trit,
at 5× the bytes).

---

# Universal Model Layer (detect → decompose → identify → dedup → run bounded → merge)

The Supra arc proved one model decomposes; this layer makes **any** supported model
file a first-class citizen, then applies the codebase-memory move to weights:
index the monolith into named, content-addressed units and reuse instead of
re-deriving. Each stage is its own module with an exact test gate, all in `make test`.

**1. Autodetect before running (`cce_detect`, `make detect FILE=path`).**
Magic-sniffs the container (GGUF / safetensors / CCE archive / packed formats),
then fingerprints the architecture from the tensors *actually present*
(separate q/k/v/o → llama-family, fused qkv → gpt2-family, `ssm_*`/`A_log` →
mamba) — declared arch strings are labels, never trusted alone. Reports layers /
dims / heads / vocab / dtype / tied-embeddings **without loading any weights**
(probing a 12B GGUF reads a few KB), plus an honest *runnable* verdict from a
single **runner registry** that also drives `cce_anymodel_open` dispatch.
Unsupported structures (hybrid attention+SSM, mamba-2, headless HF checkpoints)
refuse with a reason instead of mis-running.

**2. Universal runners.** One shared transformer forward runs both containers:
the GGUF loader and the **HF-llama safetensors loader** (`cce_st_llama`) build
the same decomposed forest, gated by **bit-identical logits** on shared weights.
A **mamba-1 SSM runner** (`cce_ssm`) does the same decomposition for state-space
models (recurrent O(1)-state inference), verified against an independent
double-precision reference (max |Δ| 7.7e-7). Empirical find, fixed everywhere:
**GGUF records dims in ggml ne-order — the reverse of torch** — proven against
the real file in `Models/`; before the fix the real-file forward silently no-opped.
`rope_theta` / `rms_norm_eps` plumb from config/metadata into the shared forward.

**3. Specialist identity (`cce_specgraph`).** Every specialist gets a content
digest (FNV over structure + weight bytes) and a behavioral fingerprint (seeded
probes; digest-equal ⇒ fingerprint-equal is a checked invariant), plus
materialized DATA_FLOWS wiring. Same weights via GGUF and safetensors produce
**identical digests** — identity is content, not container. A one-matrix
fine-tune changes exactly one node.

**4. Content-addressed weight store (`cce_wstore`).** Specialists stored once
by digest (git-objects style; one file per digest), models become flat-text
**manifests** of references. Measured: same model twice ⇒ 0 new payloads; same
weights via the other container ⇒ 0 new; a one-matrix fine-tune costs **exactly
one payload**; a model restored purely from manifest + store is **bit-identical**
in forward. "Reused" verdicts are **byte-verified**, never hash-trusted —
a forged payload under a digest is refused.

**5. Bounded-RAM streaming (`cce_tiers`).** The tile-memory pattern applied to
weights: attach a store-backed transformer to a HOT cap and the forward
rehydrates each layer's specialists on demand, evicting LRU. Capped streaming
logits are **bit-identical** to all-resident; residency never exceeds the cap;
one forward = one fetch per specialist (no thrash).

**6. Epsilon-merge behind evidence (`cce_similar`).** SIMILAR_TO candidates from
signature distance, then an adversarial probe battery through both cascades;
`cce_similar_merge` rewrites the manifest to the canonical digest and **refuses
unverified pairs** (including forged flags with no battery behind them).
Measured: an epsilon fine-tune (dev 1.4e-3) merges — restoring **bit-identical**
to the canonical model; a material change (dev 2.0) is refused twice over.

Net effect: N fine-tunes cost one base plus their material diffs, and run in
capped RAM. Sharded HF checkpoints (`model.safetensors.index.json`) load
through the same handle — gated by bit-identical logits vs the single-file/GGUF
paths, strict index↔shard cross-validation (refuse, don't guess), and a
byte-flip fuzz sweep over the index surface (`make mutate`). Honest limits:
GGUF-mamba mapping verified self-consistent but not against a real llama.cpp
export; SSM tier-streaming deferred (its runner caches cascade pointers).

---

# The Autonomy Loop (2026-07): acquire → certify → seal → route

The thesis' closing arc: the router *detects* what it cannot do, and the system
*acquires* the missing capability on its own — mining exemplars from an oracle
(a reference implementation, or a CCE-loaded model), training a candidate,
certifying it, sealing it, and registering it so the router simply finds the
plan. Everything composes the machinery above; nothing new is trusted.

**Gap-triggered acquisition (`make acquire`).** Three trigger kinds land in a
persistent ledger (`CNET_GAPS 1`, sidecar rules): NO_PLAN, LOW_RELIABILITY,
HEALTH. While a gap is open, an in-process oracle fallback keeps tasks answered
and harvests each answer as a free training exemplar. The drain then mines the
rest (exhaustive within budget, else deterministic stride sampling with a
**pilot phase** — DSpark-inspired confidence scheduling: a 16-point
domain-spanning pilot refuses degenerate constant slices at ~16× less cost),
trains, gates on oracle evidence, certifies (**PROOF** on enumerated domains,
else **SAMPLED** behind a Wilson floor), seals, registers, and replans.
Candidates are structurally invisible to the planner until certified; a
deferred acquisition leaves registry, counters, and disk **byte-identical**
(DEFER is total). Rebuilds never same-name-replace: a suspect unit is
re-certified against freshly mined truth first (a healthy incumbent is never
churned), and only a behaviorally broken one is demoted to RESET beside a
fresh-named replacement.

**The unified base (`make base`, CNB version 2 semantics under stable CNB1
magic, with v1 read compatibility).** One sealed container replaces
per-unit file sprawl: content-addressed blobs of exact `.cnu` images (each
keeping its own seal), name→blob references, digest-bound reliability stats
(stale evidence refused mechanically), oracle descriptors, and the **tag
registry** — mint-once governance with provenance and *refusal teeth*: a
near-miss tag (case-fold, underscore-strip, or Damerau-Levenshtein ≤ 1 — plain
Levenshtein misses transpositions like `nibbel`) refuses the whole unit,
all-or-nothing. Trust is replayed, never stored: loading a base re-certifies
every unit against its embedded contract (cheap via the certification cache).
`save → load → save` is byte-identical. Systematic tag families must be
spaced ≥ 2 apart (`wa<t>q<t>` doubled-id scheme) — the typo guard is doing its
job. `cnb_audit` inspects any base and digest-compares two.

**The flagship campaigns (`make flagship`, `flagship_run`).** A
thermal-governed, duty-cycled, crash-resumable harness sweeps conditioning
tokens and extracts one certified unit per token from a CCE-loaded model. The
base **is** the checkpoint (resume = skip existing units; deferred gaps reopen
and retry); a stop file interrupts cleanly; nvidia-smi gates GPU temperature;
the process runs below-normal priority.

**Campaign result (gemma4-v2, 12B, real forward — 2026-07-05).** The earlier
"draft model, 256/256 at 100%" number was an *artifact* and is retracted:
instrumenting the oracle per-layer exposed that the gemma4 forward was never
real — a GGUF data-section alignment bug read tensors shifted (NaN weights),
NaN then satisfied every float gate *vacuously* (determinism, `gpu_equiv`'s
"max |Δlogit| = 0.0", PROOF certification), and gemma4 attention was never
implemented for its true geometry (q/k/v were uninitialized heap). Every
"256/256" unit was a constant function a one-neuron net memorizes. Fixed:
align-up honoring `general.alignment`, NaN = hard-fail on every gate,
geometry-driven attention (per-layer head dims / MQA / sliding window, one
runner for gemma4 + llama + qwen), and a bit-identical dual-GPU int8 forward.
The honest run on the real oracle: **253/256** ordered-top-3 slices certified
(**SAMPLED** tier, Wilson floor ≥ 0.984), 4.76 h on two R9700s; the 3
uncertified are the model's own near-ties, declined by *margin-aware*
certification. Verified: 253/253 re-certify on load, and querying the units
against the live model (`soul_query`) reproduces it on **45/48 (93%)** sampled
questions — misses only on the abstained near-ties. Honest caveat kept in
view: the mined window is gemma4's top continuations of a *bare, template-less*
`<bos>` — high-frequency **multilingual** tokens (正如, もう少し, であれば, …),
faithfully reproduced but linguistically artificial. A faithful filing
cabinet, not yet a mind; meaningful extraction needs real-context
conditioning, not more machinery. **That conditioning now exists**: the gap
lane's corpus-drawn teaching (window files + a pinned real-prose prefix via
the probe path) produced its first unit the same day it landed —
`corpus_next_v01`, whose probed transitions are real English bigrams
(of→the, to→the, is→the). See *The gap lane* in the ledger above.

**The fuzzy tier (PAIR / TOPK task shapes).** Where exactness isn't available,
the machinery is *calibrated abstention*, not fuzzy logic (evaluated and
rejected — no calibration guarantee): sampled mining + Wilson floor globally +
**split-conformal reject-option** per query (report-only probe today). The gate
shows the guarantee working: units that memorized without generalizing get
**100% abstention** on unseen strides; diversified training samples reached
68.8% answered at 0.0227 empirical risk (target ≤ 0.05). The **TOPK "soul"
track** extracts the model's ordered top-k preference (k one-hot fields —
exactly certifiable ranked taste, margins 0.99 on the real model). Honest
findings, recorded: exactness-on-the-sample is the admission bar (a ~94%-exact
student has no path in — by design); the gemma draft model is globally
constant over arbitrary contexts, so its pair-conditional slices are
information-free and correctly refused — the sampled tier's real fight needs
corpus-drawn contexts or a non-draft model. This gate also caught a latent
drain bug (rc-vs-verdict: every successful SAMPLED certification had been
deferred) — the first genuinely sampled domain exposed it in minutes.

**GPU forward (`gpu_equiv`, `CNET_GPU=1`).** The oracle forward runs on GPU
via a self-contained OpenCL module (`cce_clgemm`): `OpenCL.dll` loaded
dynamically (no SDK, no CUDA toolkit, no nvcc/MSVC), GEMM kernel compiled at
runtime, weights device-resident. Now **multi-device**: an oracle *pool* runs
one model instance per discrete GPU (integrated GPUs excluded by the
host-unified-memory property), mining the independent per-token calls in
parallel; float weights column-split, int8 layers resident via a `q8` kernel
(**Rung 5**: ~640 GB/s GDDR6 vs ~80 GB/s DDR5). Measured on two AMD R9700s
(gemma4-v2 12B): 4.28× GPU forward, 14 GB resident/lane, plus per-unit KV
prefix reuse and a window-restricted head. Bit-identity is not assumed but
*proven*: `clgemm_unit` checks 360 GEMM calls against an exact CPU reference
across single/dual/split placements (the kernel needed `FP_CONTRACT OFF` — FMA
fusion drifted ulps and flipped near-tie argmax). **Honesty note:** the old
"11.9× on a 4070 Ti, max |Δlogit| = 0.0" number was measured on the NaN-era
forward, where the gate passed *vacuously* (NaN defeats every comparison); it
is retracted. `gpu_equiv` now hard-fails on any NaN logit, and the equivalence
sweep re-runs per lane at campaign start.

---

# Knowing the Edge

The architecture's guarantees all come from one choice — a finite alphabet at
every port. CNET's reach is therefore co-extensive with tasks that decompose into
steps whose handoffs can be a *digest* rather than a dump. Two families of
standalone harness map that edge empirically instead of arguing about it.

## The representation walls (`make margin` / `fuzzy` / `stochastic`)

Two instruments are read off every handoff: **margin** (`port_margin`, how far a
raw output sits from the canonicalization boundary — *is the handoff
unambiguous*) and **divergence** (snapped symbol vs ground truth — *is it
correct*). They are independent (a net can be confidently, maximal-margin
*wrong*), so together they separate three walls by sweeping network width and
interface resolution:

| Wall | recovers with… | signature in the data |
|------|----------------|------------------------|
| **net too small** (architectural) | width | divergence recovers as capacity rises |
| **interface too coarse** (information loss) | resolution | divergence floors across width; ambiguous *fraction* = count/2ᵏ shrinks |
| **no sharp boundary** (stochastic) | nothing | divergence floors across width *and* resolution; ambiguous *count* grows ~2ᵏ, fraction constant |

The count-vs-fraction inversion is the discriminator: a finer ADC dissolves
information-loss ambiguity but cannot touch a graded world, because that residue
is in the world, not the encoding. (Full method:
[`docs/superpowers/specs/2026-06-13-margin-study.md`](docs/superpowers/specs/2026-06-13-margin-study.md).)

## Planner scaling (`make planner_scale_study`)

Does "scale by adding primitives, not changing the core" survive a large library?
This harness drives the real `dag_plan_circuit` and reads the real search
counter (`nodes_expanded`). It separates three knobs — registry *breadth*,
per-type *collision* (how many primitives share one contract), and target
*depth*:

- **Breadth is free.** Adding distinct-typed irrelevant primitives leaves
  expansions flat (3 from registry size 2 → 514): reachability pruning skips them.
- **The default beam (8) caps collision.** With k interchangeable producers for a
  needed type, expansions saturate at a constant once k > 8, and plans stay valid.
  Collision only blows up with the beam *disabled* (then ≈ k² at depth 1).
- **Depth is the exponent (beam off).** A left-deep combiner chain of depth d
  (= d+1 leaf slots) costs ≈ k^(d+1) — measured cleanly as k², k³, k⁴ for
  d = 1, 2, 3 (1.7M expansions at d=3, k=16). Under the default beam this instead
  saturates in k.

The honest residual risk: the beam keeps the top-8 by *reliability*, so a correct
producer ranked below the cutoff (stale or adversarial stats) could be silently
missed — a failure mode the interchangeable-producer harness cannot yet exhibit.
That is the next experiment worth running.

---

# CNET-D — The Advisory Imagination Lane

A second lane runs beside the deterministic core: a **proposer** generates candidate
structures, the existing strict machinery validates them, and the proposer carries **zero
authority** — *diffusion proposes, CNET disposes*, except diffusion is only a possible future
backend; the durable win is the no-authority sidecar itself. It slots into the same
SHADOW_ONLY / `influence_on_planner = 0` discipline as the v2.x engram/rank-artifact lineage.
All of it is working-tree only (uncommitted).

- **Proposal Sidecar (v5.0, SHADOW_ONLY).** The no-authority contract: a scripted proposer
  emits candidate plans, `dag_execute` (strict) is the *only* validator, and the sidecar
  **snapshots + restores the borrowed primitives' reliability counters** so that even
  *validating* an imagined candidate leaves no footprint on the evidence the planner ranks by.
  Hermetic gate in `make test`.
- **Below-Beam Recovery (v5.1, SHADOW_ONLY · detect + report).** The planner-scale study named
  a residual risk: a correct producer ranked below the beam-8 cutoff (stale or adversarial
  stats) is silently missed. The sidecar exhibits it — a beam-lifted re-plan finds a valid plan
  the official beam missed, reports it, and changes nothing. Hermetic gate.
- **Failure-mode characterization (`make belowbeam_chars`).** Detection is cause-agnostic, so
  the data that matters is the *shape*: a missed plan reappears exactly at the **reliability-
  margin zero-crossing** (mechanism-intrinsic), under two causes — *cold-start* (correct
  producer under-ranked for low evidence) and *rank-poisoning* (wrong producer over-ranked).
  The rank-gap distribution is population-dependent (a sweep artifact); only the margin boundary
  transfers, and real-deployment frequency is the deployer's own convolution — not measured here.
- **Probe overhead (`make probe_overhead`).** ~1.90× one route+execute cycle (a deployment-
  shaped denominator, not the bare planner). Shadow-only and opt-in: paid only when the probe is
  actually called.
- **Structural Preference — graded coherence over already-valid structures (built).** A
  *recommendation* layer that sits **above** the pure binary validator, never inside it.
  **A (SHADOW_PLUS_RECOMMEND, `planner_influence = 0`)** ranks already-valid structures by a
  *derivation-lock residual*: re-derive the same task under bounded perturbation (the discriminating
  one is candidate order) and count how often the same canonical plan digest reappears — an attractor
  (residual 0) is recovered by many search paths, a fragile structure (residual > 0) only by one
  ordering. It then *recommends* the most order-robust structure, planner untouched. **B0 (opt-in
  ORDER_ONLY gate probe)** freezes that ranking into a `CircuitRankArtifact` and feeds it through the
  *existing* v2.2 ORDER_ONLY channel: the additive bias flips a single-root pick toward the attractor,
  full set preserved, no prune/mutate — but **default runtime stays `attention_mode = OFF`** and it does
  **not** claim the gated attention planner is reopened (keying is still producer-name-global; per-task
  keying is the deferred B1). Honest scope: this is **structural** preference (stable, reproducible,
  attractor-like), and the residual's *sign* is the signal, not its magnitude (which is
  perturbation-scheme-dependent). It is *not* human/semantic taste — that lives on the wrong side of the
  representation wall and needs a learned distribution CNET deliberately doesn't carry.

The honest frame: CNET-D never decides, mutates, or certifies — it proposes and ranks; the
strict core stays the sole authority. Diffusion is one possible proposer backend, not the
engine — and at toy scale, deterministic proposers (beam-lifted re-plan, the margin signal,
perturbation robustness) have so far dominated it.

---

# Status & Caveats (current)

The **dated arc log** — the chronology of what landed when, including the
mechanism thread (v0.5.3–v2.9) and the 2026-06→07 CCE/autonomy arc — lives in
[`docs/cnet-history.md`](docs/cnet-history.md). This section is the undated
current state.

- **Mechanism thread frozen.** Attention-planner work is gated: resume only
  if, on top of reachability pruning, non-tiny fixtures show ≥3× fewer nodes
  or less wall time with no fallback-rate increase, plan correctness
  preserved, and no synthetic dashboards.
- **Working solution achieved:** Coherent responses from given text (perceptual glyphs or symbolic queries) via the contract-based system (perceptual_query orchestrator + sub-contracts + CNET-D steering + dual-track + persistence + evolution). See glyph_habitat --demo 5A for the interactive memory agent.
- **Own internal models:** First-class trainable generative logic (next-token / next-concept predictors) implemented as ordinary CNET BTNs + contracts. Trainable from HF data or internal traces. See `--demo LLM` + `train lm`.
- **HF contract training:** Use https://huggingface.co/datasets to source data for speech contracts and own LLM-type models (`--demo SPEECH`, `--demo LLM`).
- **Build system:** Full `build/` directory (parallel to tests/) with Grok-style console (`build_tool`) that produces artifacts via MCP writes, supports natural language, internal thinking, and training speech/LLM contracts.
- **Active frontier** is the learned-leaf habitat (v4.4) + generative narrative layer + own trainable LLM cores + external data ingestion for contracts. CNET-D advisory lane continues to gain steering depth.
- **Real model compression:** a full 119 MB HF transformer is decomposed,
  run bit-exact in pure C, and compressed down a verified ladder — int8 PTQ
  near-lossless, 1.6-bit packing storage-exact but quality-gated on QAT.
  Details + caveats in *Real Model Compression*.
- **Universal model layer:** supported model files autodetect structurally,
  decompose through universal runners, dedup into a content-addressed weight
  store, stream in bounded RAM bit-identically, and merge only behind an
  adversarial probe battery. Details in *Universal Model Layer*.
- **Contract hardening:** behavior-only digests, certification cache, sealed
  contract files (tamper refused), certificate-to-weights binding with audit
  demotion, and one-file sealed `.cnu` units in `registry_save`. Authored,
  borrowed, and frozen contracts now validate bounded shape and exemplars before
  atomic publication; signatures are checked before table hashing; text
  persistence round-trips finite RAW/EVIDENCE values at `%.17g`; robust
  promotion reuses one certification report per candidate.
- **Unified runtime + Oracle v2:** BTN, CCE, and evidence-carrying Oracle
  specialists share one certification/admission lifecycle through native C,
  `SoulHost`, .NET, and MCP. Async lanes preserve cancellation, deadline, and
  failure evidence instead of coercing non-answers into labels.
- **Model residency:** one backend-neutral catalog governs Dense, MoE, SSM,
  embedding, and CCE-native descriptors with explicit resource budgets, lazy
  loading, generation-stamped leases, concurrent-load deduplication, pinning,
  true LRU eviction, and atomic multi-resource admission.
- **Qwythos/Qwen3.5 architecture/QGKP:** the QGKP-v3 envelope preserves a byte-identical mixed
  TQ1_0+Q4_K payload and the 1,048,576-token context metadata, then executes it
  through the CNET-governed hybrid backend. It is deliberately not routed into
  the legacy QGKP-v2 native runner, which lacks hybrid SSM/attention dispatch.
- **Autonomy loop:** gap-triggered acquisition (DEFER-total), the unified CNB
  version 2 base under stable CNB1 magic (v1-readable) with mint-once tag governance, and the
  thermal-governed flagship harness. Honest campaign (gemma4-v2 12B, real forward, 2026-07-05):
  **253/256** ordered-top-3 slices certified (SAMPLED, Wilson ≥ 0.984),
  93% live-model fidelity when queried; the fuzzy tier adds sampled extraction
  (Wilson floors + conformal probe), *margin-aware* certification (decline the
  teacher's own near-ties), and ranked-preference ("soul") units. The prior
  "256/256 at 100%" was a NaN-oracle artifact — retracted, story in *The
  Autonomy Loop*.
- **GPU forward:** self-contained OpenCL (`cce_clgemm`), multi-GPU oracle pool
  + int8 `q8` layers, 4.28× on 12B, bit-identity **proven** (`clgemm_unit`),
  NaN-hard-failed (`gpu_equiv`, `CNET_GPU=1`).
- **Legacy DAG machinery restored and gated:** the full planner/executor/
  blackboard/engram/rank-artifact code lives in `src/router/dag_full.c`;
  the complete legacy `test_all` is green and runs as a verify gate
  (`legacy`) with demo-driven fixture regeneration. (The restoration story —
  a refactor had silently stubbed ~3,500 lines — is in the history doc.)
- **Build state:** `make test` runs the native verification chain: CCE runtime,
  universal-model suites, contract security + unit files, the acquisition loop
  (`acquire`), the unified base (`base`), the flagship harness (`flagship`),
  the restored legacy aggregate (`legacy`), and an allocation-balance leak gate
  (`leakcheck`, `--wrap`-based, CRT-baseline-aware — the no-ASan toolchain's
  behavioral substitute). `make unified` adds the model-file-free `cnet.so`,
  Oracle v2, async lanes, model residency/catalog, DS4 launcher checks, .NET
  host, and stdio MCP vertical slice. `make test_full` adds GPU equivalence.
- Cost-aware dual-track expansion (3C A1 — durable teacher recipes + LOW-power
  planner expansion) is committed. The architecture has been extended through multiple increments (3D–5A) into a self-improving perceptual reasoning engine capable of certified multi-step narrative generation.
- A fixed-buffer stack overflow in `attention_retrieve_top_k` (registries > 64
  primitives under any attention mode) was found and fixed; `make
  test_attention_overflow` guards it.
- **Repository hygiene (2026-07-11):** `*.cnb` base containers are large mined
  artifacts and now gitignored (kept on disk, beside the existing `*.cce` /
  `*.gguf` / `*.safetensors` rules); the 262 MB campaign base was stripped from
  git history to fit hosting limits (pre-rewrite history preserved locally on
  `backup-pre-rewrite`). The campaign checkpoint IS the on-disk base — version
  control carries the machinery and its gates, never the mined weights.

# Make Targets

| Target | What it does |
|--------|--------------|
| `make` / `make run` | build / build and run `nn_demo` (trains and freezes all primitives) |
| `make test` / `make verify` | full offline native verification: claims-generator regression, CCE DLL, safetensors loader, autograd, model save/load, WARM archive/forest views, universal-model suites (detect, ssm, st_llama, specgraph, wstore, tiers, similar), contract security + unit files, loader-robustness sweep (`mutate`), acquisition loop, unified base, flagship harness, restored legacy aggregate, and leak gate |
| `make unified` | CPU/model-file-free vertical gate across native adapters, Specialist edge units, Oracle v2, async runtime, model catalog/residency, DS4 launcher, `cnet.so` symbols, `SoulHost`, .NET host/tests, and stdio MCP; requires a fresh strict 13/13 run-scoped claims ledger and labels the GPU-only lane out of scope; prints `CNET_UNIFIED_PASS` |
| `make specialist_unit` | Specialist NULL/unknown-kind/adapter refusal, lifecycle trust-axis characterization, conservative residency mapping, registry initialization, and axis-name edge cases; prints `SPECIALIST_UNIT_PASS` |
| `make unified_specialist` | the heterogeneous-plan acid test: ONE certified route/DAG plan mixing a native BTN, a real CCE model, and an Oracle unit through the single `Specialist` door; prints `HET_PLAN_PASS`; in `make unified` |
| `make specialist_health` | runtime health optimizer gate: fix (audit, contract/teacher labeling, heal) + improve (promote, shadow-swap) through certified paths only; healthy registry is a proven no-op; prints `SPECIALIST_HEALTH_PASS`; in `make unified` |
| `make gap_lane` | the 24/7 learning-loop gate: inbox → ledger → oracle-taught dynamic-growth student → certified/sealed → replan → rebuild → atomic checkpoints → resume; prints `GAP_LANE_PASS`; in `make unified` |
| `make gap_lane_run_build` | build `bin/gap_lane_run`, the gap-lane daemon (local-model teacher via the CCE GGUF runner; stop file `<base>.stop`; env knobs in `tests/gap_lane_run.c`) |
| `make claims` | execute `make unified` and emit a fresh, strict run-scoped ledger (`logs/claims.jsonl`, `docs/verified-today.generated.md`) |
| `make claims_all` | inventory every known log with exact-marker verdicts; explicitly labels filesystem mtimes/scanner identity and does not claim current-run freshness |
| `make unified_async` | work-conserving 2+ lane runtime: copied inputs, ordered collection, bounded capacity, cancellation, deadlines, timeout recovery, and telemetry |
| `make unified_gpu` | two physical R9700 lane fixture with iGPU exclusion, exact CPU parity, per-lane work evidence, and serial-vs-async timing |
| `make unified_models` | synthetic QGKP-v3 envelope round-trip/refusal tests plus 90-check model lifecycle/resource manager and 12-check header-only model catalog; real Qwythos execution is a separate acceptance target |
| `make unified_ds4_launcher` | hermetic validation of the resumable dual-R9700 DS4 launcher, chunk-tree identity, endpoint verification, and refusal paths |
| `make oracle_v2_bench` | seven-round median benchmark of direct v1, governed v1, governed v2 evidence, and v2 semantic validation |
| `make contract_optimized` | optimized contract semantics, RAW/EVIDENCE round-trip, robust-margin promotion, exact forward-count benchmark, ASan+UBSan, security/unit, and historical regressions |
| `make qgkp_envelope_test` | QGKP-v3 metadata/payload envelope round-trip and malformed-envelope refusal checks |
| `make qwythos_qgkp_acceptance` | inspect/materialize/hash the Qwythos QGKP-v3 envelope and run the bounded GPU1 causal/constraint/narrative coherence gate |
| `make verify-long` | fast verification plus longer benches/studies: `cce_train_bench`, Supra head QAT, corpus QAT, and `wordlm_bitnet` |
| `make legacy` | the restored full historical aggregate (`test_all`, ALL TESTS PASSED) + demo-driven fixture regeneration; in `make test` |
| `make leakcheck` | allocation-balance gate over the base+acquire paths (`-Wl,--wrap`, CRT-baseline-aware); in `make test` |
| `make acquire` | gap-triggered acquisition loop gate: ledger, oracle fallback, drain, rebuild, DEFER totality (118 checks) |
| `make base` | unified CNB version 2 base gate under stable CNB1 magic (v1-readable): sealed container, tag governance, certify-on-load bridge, Oracle identity persistence, and migration (81 checks) |
| `make flagship` | flagship harness gate: task shapes, sampled tier, conformal probe, pilot scheduling, resume, stop file (72 checks) |
| `make flagship_run_build` | build the REAL extraction CLI (CCE model as oracle); `CNET_GPU=1` enables the OpenCL forward (equivalence-gated) |
| `make cnb_audit` | base inspector: counts, certify-on-load verification, tag audit, cross-base digest fidelity |
| `make gpu_equiv_build` | CPU-vs-GPU equivalence gate (argmax + top-3 must agree 100%; prints speedup) |
| `make test_full` | everything `make test` covers plus the GPU equivalence sweep |
| `make decimal` | train + freeze the decimal domain, run all acts |
| `make circuit` | discover + verify circuits, distill the circuit chunk, measure pruning |
| `make chunk` | distill proven plans into chunk primitives |
| `make library` | grow the library by itself to a fixed point (dedup + law guard) |
| `make certify` / `make property` | certify primitives end-to-end / check the round-trip laws |
| `make glyph_habitat` | the learned-leaf habitat: 4 perceptual domains through one `dec_symbol` port (v4.4) + interactive agent + build/ console + own LLM + HF contract training |
| `make glyph_habitat --demo 5A` | full 5A episode (queries, memory, branching, summary table) |
| `make glyph_habitat --demo BUILD` | build artifacts + Grok-style REPL integration |
| `make glyph_habitat --demo SPEECH` | train speech contracts from https://huggingface.co/datasets using btn_train_dynamic |
| `make glyph_habitat --demo LLM` | train our own internal CNET generative model (next-token predictor BTN + contract) |
| `cd build && make` (or top-level `make build_tool`) | dedicated build/ agent console (Grok-like REPL, artifact generation, train speech/lm) |
| `make planner_scale_study` | breadth / collision / depth scaling sweep (not in `make test`) |
| `make lbench` | lifecycle-spine planning-overhead benchmark (not in `make test`) |
| `make belowbeam_chars` | CNET-D below-beam failure-mode sweep: margin-boundary + rank-gap (not in `make test`) |
| `make probe_overhead` | CNET-D v5.1 probe cost vs one route+execute cycle, ~1.90× (not in `make test`) |
| `make struct_pref` | CNET-D structural-preference derivation-lock sweep: attractor vs fragile (not in `make test`) |
| `make test_attention_overflow` | regression for the >64-primitive attention overflow |
| `make compounding_bench` | demonstrates 3C dual-track + generative notes (4A/4B orchestration) |
| `make build_tool` | build the Grok-like build/ console (artifact generation + train speech/lm) |
| `make margin` / `fuzzy` / `stochastic` | the three representation walls (not in `make test`) |
| `make sevenseg` | is the 7-seg confident-wrong catchable? input floor + K-leaf ensemble (not in `make test`) |
| `make throughput` / `fastpath` | batched fast-execution lanes (benchmark / unit tests) |
| `make jsonstory` | JSON understand+generate as a PROVEN contract over TinyStories |
| `make pdftest` / `make pdflearn [file.pdf …]` | PDF extractor unit tests / ingest a PDF into the dynamic corpus + replay-train `cce_wordlm` |
| `make compound` | compounding kNN-LM retrieval memory (reuse vs retrain cost curve) |
| `make tiermem_test` / `make graduate` | tiered tile memory (HOT/WARM, TF-IDF) / graduate memory → certified router-composed units |
| `make fontdecode` / `make tfidf` | extraction quality (`/Differences` recovery + filter) / TF-IDF retrieval |
| `make synonyms` / `make tileindex` | PPMI synonym map + opt-in query expansion / unified inverted index (candidate-only search, byte-identical to the linear oracle) |
| `make consolidate` | opt-in PPMI semantic consolidation: merge paraphrase tiles (rarest-first candidate cap, conservative `min`-coverage) |
| `make supra_console` | load + run the decomposed HF Supra model (download-once `./supra_cache/`; `--mode text/chat`, `--int8` for int8 PTQ) |
| `make cce_safetensors_test` | safetensors loader unit tests + real Supra decomposed load (header-first, validated) |
| `make detect FILE=path` | CLI probe: container format, structural arch family, hparams, honest runnable verdict — no weights loaded |
| `make cce_detect` | autodetect + runner-registry suite (synthetic fixtures per family + guarded real-file checks) |
| `make cce_ssm` | mamba-1 SSM runner: born-exact vs independent double reference + gguf/safetensors mapping equivalence |
| `make cce_st_llama` | HF-llama safetensors loader: bit-identical logits vs the GGUF path on shared weights |
| `make cce_specgraph` | specialist identity graph: content digests + behavioral fingerprints + DATA_FLOWS wiring |
| `make cce_wstore` | content-addressed weight store: models as manifests, byte-verified reuse, bit-identical restore |
| `make cce_tiers` | bounded-RAM streaming inference from the store (HOT cap + LRU; logits bit-identical) |
| `make cce_similar` | SIMILAR_TO + adversarial verify + evidence-gated epsilon-merge (manifest remap) |
| `make contract_secure` | contract digests, certification cache, sealed files, certificate-to-weights audit |
| `make contract_unit` | one-file sealed units (.cnu): weights + contract, bit-packed exemplars, tamper refused |
| `make mutate` | loader-robustness sweep: every single-byte flip + truncation over `.cnu`/`.cnb` (all refused) and gguf/safetensors/`.cce` probes (no crash); in `make test` |
| `make bitnet_qat` | self-contained BitNet b1.58 QAT demo (FP shadow + STE): QAT ternary clearly beats post-hoc ternary |
| `make wordlm_bitnet` | long BitNet QAT run in the real word-LM: FP / linear / embedding / both ternary + trit-packed export → reload parity (`ΔNLL=0`); use `WLM_EPOCHS=N` to shorten |
| `make clean` | remove all binaries |

Demo targets rerun `./nn_demo` first, regenerating weight files and deleting
stale stats sidecars. Deterministic seeds make retraining byte-identical — that
reproducibility is the project's regression gate.

# Building Manually

```sh
gcc -std=c11 -Wall -Wextra -pedantic -O2 -o nn_demo src/nn.c src/main.c -lm
./nn_demo 0.7 20000 12 0001000010101111   # decodes to 10AF
```

Planner / consolidation binaries also link files under `src/router/`, `src/plan_table.c`,
`src/contract.c`, and `src/consolidate.c`; see the Makefile for each target's
exact inputs.

## Build Tool (Grok-like Console + Artifact Generation + Own Models)

```sh
make build_tool
./build_tool          # interactive Grok-style REPL
./build_tool --build "a short story about rain"
```

The `build/` directory (parallel to `tests/`) contains a dedicated agent console:

- Natural language + explicit commands: `build <spec>`, `train speech`, `train lm`.
- Uses full CNET stack (memory, MCP file write, contracts, internal thinking).
- Can train speech contracts from HF and our own LLM-type generative models (next-token BTNs + contracts).
- All artifacts written under `build/`.
- See `build/README.md` and `./glyph_habitat --demo BUILD` / `--demo LLM` / `--demo SPEECH`.

Run from project root or inside `build/`. Sanitization protects filenames; thoughts are recorded.

# File Formats

- **Weight files** (`CNET_BTN 5`): plain text — dimensions, typed input/output
  ports with optional tags, then weights. Versions 1–4 still load.
- **Stats sidecars** (`CNET_STATS 1`): two counters (successes, failures). A
  missing sidecar means "no recorded history."
- **Contract files** (`CNET_CONTRACT 2`): a named transform spec — port signature
  with tags plus a canonical exemplar table and a trailing `SEAL` (FNV over the
  semantic content, refused on mismatch); replayed by `btn_certify`. v1 files
  (unsealed) still load, flagged `seal_verified = 0`.
- **Unit files** (`<name>.cnu`, `CNU1`): weights + contract as ONE sealed binary
  artifact — binary f64 live weights, bit-packed canonical exemplars, FNV seal
  verified before parsing. Written by `registry_save` (replaces the
  `.btn` + `.contract` pair); `unit_load` round-trips bit-identically.
- **Weight-store payloads + manifests** (`<digest>.spec`, `CNET_MANIFEST 1`):
  content-addressed cascade/tensor payloads (one file per digest) and flat-text
  model manifests referencing them; specialist graphs persist as
  `CNET_SPECGRAPH 1` sidecars.
- **Base containers** (`<name>.cnb`, CNB version 2 semantics under stable CNB1
  magic, with v1 read compatibility): ONE sealed container for many
  units — content-addressed blobs of exact CNU1 images, name→blob references,
  the mint-once tag registry (with provenance), digest-bound stats, and oracle
  descriptors; whole-file seal verified before parsing; `save → load → save`
  byte-identical. Gitignored: bases are large mined artifacts that live on
  disk beside the repo, not in version control.
- **Gap ledgers** (`CNET_GAPS 1`): the acquisition loop's sidecar — per-gap
  trigger kind, task signature, status (OPEN/DEFERRED/CLOSED), counters, and
  defer-reason atoms.
- **Property files** (`CNET_PROPERTY 1`): an equational law — typed sources plus
  two chains of primitive names, checked by strict replay over the enumerated domain.
- **Expansion sidecars** (`<name>.expansion`, 3C): for chunks that carry a recipe —
  teacher_mac, student_mac, compute_beneficial, expand_in_low, and the ordered
  teacher primitive names. Written by `registry_save`; restored by
  `registry_load_expansion`. Absent for compute-beneficial or over-cap chunks.

# Repository Layout

```text
include/
├── cce/               CCE headers (cce.h + cce_*.h)
├── contract/          Contract headers (no contract_ prefix)
└── router.h           Planner API

src/
├── cce/               Contract Cascade Engine (new core)
│   ├── cce_tensor.c
│   ├── cce_block*.c
│   ├── cce_cascade.c
│   ├── cce_archive.c     # mmap + directory for partial load
│   ├── cce_forest.c      # hot/warm/cold + recall
│   ├── cce_router.c      # SSMax
│   ├── cce_learn.c       # goodness + DFA
│   ├── cce_safetensors.c # HF safetensors load + Supra decompose / int8+ternary quant / trit-pack + on-disk packed artifact
│   ├── cce_gguf.c        # GGUF loader + decomposed transformer runner (ne-order dims)
│   ├── cce_st_llama.c    # HF-llama safetensors -> same decomposed transformer
│   ├── cce_ssm.c         # mamba-1 SSM runner (recurrent, forest-decomposed)
│   ├── cce_detect.c      # container sniff + structural arch fingerprint + runner registry
│   ├── cce_specgraph.c   # specialist digests + fingerprints + DATA_FLOWS graph
│   ├── cce_weight_store.c# content-addressed store; models as manifests
│   ├── cce_tier_runtime.c# bounded-RAM streaming from the store (HOT cap + LRU)
│   ├── cce_similar.c     # SIMILAR_TO + adversarial verify + evidence-gated merge
│   ├── cce_wordlm.c      # O(V·d) word-LM + BitNet b1.58 QAT (shadow+STE) + trit-pack export/reload
│   ├── cce_clgemm.c      # self-contained OpenCL GEMM (dynamic OpenCL.dll, resident weights) — the GPU forward
│   ├── cce_gpu.c         # legacy GPU scaffolding (the CUDA path never compiled; superseded by cce_clgemm)
│   └── cce_abi.c         # public C ABI
├── contract/          Contract modules (contract.c: certify+cache+seal+audit; unit.c: .cnu units; coverage.c, conformal.c, ...)
├── router/            dag_full.c (restored monolith DAG machinery) + registry.c + route.c
├── pdf/               PDF ingestion: inflate.c, pdf_extract.c, font_decode.c
├── corpus/            corpus_split.c, corpus_store.c, retrieval.c, tile_memory.c, synonyms.c, graduate.c
├── acquire.c          Gap-triggered acquisition loop (ledger, oracle fallback, drain)
├── base.c             Unified CNB v2 semantics (stable CNB1 magic, v1-readable; governance, registry bridge)
├── flagship.c         Thermal-governed extraction harness (task shapes, conformal probe)
├── nn.c               Legacy primitives
└── main.c             nn_demo (historical)

tests/cce_smoke.c      Full CCE layer test

build/                 Grok-style console (maintained)
```

The CCE in `src/cce/` is now the primary focus.

**Real-data milestone:** `test_tinystories` shows CCE cascades (with context windows) producing coherent narrative on TinyStories via guided selection while also exercising raw unconstrained logits sampling. The same engine powers the `cnet_lm` / `train lm` path, making CCE the default for own generative models. See the dedicated test, `src/cnet_lm.c`, and the build console.

The design documents in `docs/superpowers/specs/` record rationale for the overall approach.
