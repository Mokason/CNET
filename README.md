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

Rechecked end-to-end on this host (2026-07-14, priority acceptance): `make
priority_acceptance` — `PRIORITY_ACCEPTANCE_PASS`; its unified phase emitted
`CNET_UNIFIED_PASS` with a fresh, strict **21/21** in-scope claims ledger. The
physical-GPU and private-model lanes are reported explicitly as out of scope
rather than accepted from older logs.

| Area | Verified by | Status |
|---|---|---|
| CCE runtime (tensor → block → cascade → archive → forest → router → learn) | `make cce_smoke`, `make cce_train_bench` | passing (standalone gates) |
| CCE storage + loaders (C ABI/DLL, safetensors, autograd, model save/load, zero-copy WARM views) | `cce_dll`, `cce_safetensors_test`, `cce_autograd_test`, `cce_model_test`, `cce_view`, `forest_view` | passing, in `make test` |
| Contract security + one-file sealed units | `make contract_secure`, `make contract_unit` | passing, in `make test` |
| Contract correctness + robust promotion quality/speed | `make contract_optimized` | malformed authored/frozen contracts refused atomically; stronger certified margin wins with one replay per model |
| Unified contract runtime (BTN + CCE + Oracle v2 evidence-carrying specialists + CNB version 5 semantics under stable CNB1 magic with v1–v4 read compatibility + planner/executor + `SoulHost` + .NET/MCP projection) | `make unified` | CPU-only, model-free vertical acceptance gate; Oracle descriptors remain provenance until a resolver asserts matching sealed identity and the callback passes provenance-linked contract replay |
| **One `Specialist` type across kinds** — a native BTN, a real CCE model, and an Oracle unit as ordinary nodes of ONE certified plan (route + DAG), admitted through the single door (`specialist_admit`), trust/residency/role axis views | `make specialist_unit`, `make unified_specialist`, `make soul_reopen_test`, `make admission_bypass_audit` | Edge refusals and heterogeneous strict planning are gated; close/reopen remount proves live ORACLE identity and native+Oracle execution twice; the static audit rejects production admission bypasses |
| **Runtime health optimizer** — one opt-in maintenance pass that fixes (audit → contract/teacher fault labeling → heal via retrain + passing re-certify) and improves (evidence promotion, shadow hot-swap) through existing certified paths only | `make specialist_health` | `SPECIALIST_HEALTH_PASS` (17 checks), in `make unified`; healthy registry = proven no-op, zero-init config = total no-op; demoted adapters stay RESET — their repair path is re-acquisition  Wired into SoulHost/MCP: `soul_health_tick` C ABI, `cnet_health_tick` tool, opt-in `CNET_HEALTH_TICK_SECONDS` timer |
| **One dispatch story** ([`docs/dispatch.md`](docs/dispatch.md)) — "which specialist runs" has one answer in three layers with machine-checked boundaries: recall (`cce_router` SSMax) dispatches INSIDE a Specialist under its contract; the certified planner is the sole cross-specialist composition authority; orchestrators are policy above the planner | `make dispatch_story` | `DISPATCH_STORY_PASS` (15 checks), in `make unified`: a two-branch model routes per input yet certifies and replays as ONE node; evidence never outranks certification; among the certified, learned reliability ranks |
| **The gap lane** — the 24/7 learning runtime: serving misses land in a gap inbox, the lane ingests them into the persistent ledger, bridges unhealed demotions to HEALTH gaps, and drains by teaching from a bound local model (dynamic-growth students, PROOF/SAMPLED certification, sealed into the base); checkpoints are atomic for base AND ledger; a fresh lane resumes from disk | `make gap_lane` | `GAP_LANE_PASS` (51 checks), in `make unified`; daemon `bin/gap_lane_run` (`make gap_lane_run_build`) has a fail-safe `cnet-gap-lane` user unit, currently intentionally **disabled/offline** with Gemma offline; `make gap_lane_service_prepare` only builds and validates it and never starts/enables the service; activation remains an explicit operator action. Novel goals flow in by explicit typed signature: the `cnet_request_capability` MCP tool (`soul_request` ABI) serves the request now when a certified plan exists, answers capability probes without executing, and otherwise queues the full signature to the inbox — a goal no longer needs an existing unit to become the lane’s work. The wildcard-identity plan (length 0) counts as no plan, same rule as `soul_route` Teaching is corpus-drawn: `CNET_WINDOW_FILE` maps the one-hot alphabet to real corpus token ids (the english window) and `CNET_LANE_CONTEXT_FILE` pins a real tokenized prose prefix in the KV once — every teaching call is an independent probe of prefix+token (`cce_gguf_qwen2_forward_probes`, KV never modified), with the head restricted to the window ids (bit-identical, fraction of the cost). Bare single-token contexts remain the fallback Multiple named contexts per lane: every `<name>.ids` in `CNET_LANE_CONTEXT_DIR` is a pinned-able context selected by goal-tag prefix (`<name>_…`); the teacher re-pins the KV on context switch (once per gap, not per point), and each context carries its own provenance fingerprint into the ledger — proven live with lamp/storm units closed in one tick whose transitions differ by context on identical inputs Unit provenance is a direct relation: a closed gap’s teacher persists as a CNB oracle descriptor (kind `gap_lane_teacher`; identity = the model artifact’s BYTES via streamed SHA-256 — the full 256-bit hash recorded (`artifact_sha256`, collision-resistant) with its 64-bit truncation kept as the fast behavior-digest index; the file’s content, not its path; empty artifacts refused — window config, context retrieval snapshot, taught-signature contract digests, and the teaching stack’s toolchain digest: compiler + build flags + source revision, Makefile-injected, `unattested` visible when built outside it), and the minted unit points at its descriptor directly (`CnbUnitRef.provenance`, CNB v5 with v1/v2/v3/v4 read compat; the gaps ledger v4 carries the minted unit name, the persisted reconcile mark, AND the recipe fingerprint; projected by `soul_unit_provenance`), surviving resume, served by `cnet_list_oracles` under the existing `descriptor_only_not_runtime_trust` admission label. Descriptor resolution is identity-aware: a recurring teacher name with a DIFFERENT identity mints a versioned descriptor (`<name>_i2`…) — the stored identity is never silently reused; a zero-toolchain identity is not provenance; without a live teacher, links happen only when unambiguous. Recipe-change retry: a deferral for a recipe-dependent reason (certify/accuracy/exemplar/oracle-fit/imbalance) is stamped with the acquisition recipe's fingerprint, and a later drain reopens it when the current recipe differs — so a bigger student budget or looser certification bar automatically retries old failures instead of the no-churn re-note policy stranding them; a re-defer re-stamps, so the retry fires once per recipe change, never every drain. Reconciliation is queue-fed O(1) per closure (drain close-hook + persisted done-marks; the one full scan is the open() migration/repair pass) and stays retryable on write failure. Full-width artifact identity landed 2026-07-13; CPU linked-runtime attestation and exact SoulHost/.NET/MCP projection landed 2026-07-17. Still open: GPU driver/kernel folding when a GPU lane teaches |
| **Phase 1/2 runtime integration** — counterfactual route evidence executes on the live `soul_route` serving path (report-only, opt-in `CNET_COUNTERFACTUAL`; served answers byte-identical knob on/off, roster slot 0 reserved for the served unit) and sparse per-specialist KV selection executes on the `cce_gguf_qwen2` KV attention path (opt-in `CNET_SPARSE_KV`, audited-inert OFF, bit-identical at budget 1.0, fail-loud on unwired paths) | `make counterfactual_serving`, `make sparse_kv_exec` | `COUNTERFACTUAL_SERVING_PASS` (28), `SPARSE_KV_EXEC_PASS` (43); external benchmarks (FACTOR/TruthfulQA, LongBench-class) remain **withheld** — the TruthfulQA dataset is in-repo (`references/truthfulqa/`) but unmeasured |
| **Teaching fast path** (`CNET_TRAIN_FAST=1`, opt-in, plain-SGD only) — register-blocked reductions + exact-zero skip + deterministic OMP worksharing, BYTE-IDENTICAL weights to the default path | `make teach_fast`, `make acquire` (both knob states) | `TEACH_FAST_PASS`; measured 1.61× serial / 4.3× @4 threads at the deployed H=512 shape; **deploy profile defaults ON** via `config/cnet-deploy.env` / gap-lane service (hermetic gates still exercise both states) |
| **Unified self-improve + resource control plane** — deploy free wins, resource governor (RAM/VRAM/teach-rate/teacher-idle), budgeted gap drain + sleeping teacher, TOPK_SET teacher claims, multi-step distill through specialist door, harness-as-oracle admit, CF ORDER_ONLY ranker, recipe proposals that never lower cert bars | `make unified_self_improve` | `UNIFIED_SELF_IMPROVE_PASS` (component gates: `RESOURCE_GOVERNOR_PASS`, `CF_ORDER_PASS`, `SELF_IMPROVE_PASS`, `DEPLOY_PROFILE_PASS`, `RECIPE_PROPOSALS_PASS`); plan: `plans/unified_self_improve_resource.md`; v2 campaign recipe: `config/qwythos_v2_campaign.env` (new goldens required) |
| **Campaign v2-fast** — sweep-guided allowlist (`CNET_UNIT_ALLOWLIST`) + index shards (`CNET_UNIT_START`/`END`) + deploy free wins; no cert-bar changes | `make campaign_v2_fast`; `tools/campaign_v2_fast.sh prepare\|estimate\|env` | `CAMPAIGN_V2_FAST_PASS`; real sweep exports **255** minable ids → ~**4–6 h** 1-worker / ~**2 h** 2-worker wall (50–70 s/unit) |
| **Multimodal v0 (voice/vision external teachers)** — foreign frameworks as oracles only; closed-set speech commands + fixed-label vision mined through `external_teacher` → certify → `specialist_admit` | `make multimodal_v0` | `MULTIMODAL_V0_PASS` (39); plan: `plans/multimodal_external_teachers.md`; helper: `tools/multimodal_campaign.sh` |
| **Personal AI (local first, big AI on call)** — certified library serves first; miss → teacher help + gap note; tick seals a local skill; abstain if no teacher | `make personal_ai` | `PERSONAL_AI_PASS` (13); plan: `plans/personal_ai_local_first.md`; env: `config/personal-ai.env` |
| **Personal AI automation** — user systemd learner + optional Hermes serve; one script prepare/install/start/status/doctor | `make personal_ai_auto`; `scripts/personal_ai_auto.sh start` | `PERSONAL_AI_AUTO_PASS`; full loop: `SERVE=1 scripts/personal_ai_auto.sh start` |
| **Hybrid A/B/C (universally stronger serve)** — Tier A certified → B soft/medium → C residual; structure mine C→A; adapter; distill hook | `make hybrid_ai`, `make hybrid_bench` | `HYBRID_AI_PASS` (21); bench skill/open **100%**, ops ratio **0.625** vs dense (`logs/hybrid_bench.json`); plan: `plans/hybrid_universal_architecture.md` |
| **Real Tier C residual (GGUF) + P5 structure mine** — local transformer as open-ended residual; auto-bind on `personal_ai_open`; residual traces mine into certified local units | `make residual_gguf`; real: `make residual_gguf_real` / `residual_structure_mine_real` | Hermetic `RESIDUAL_GGUF_PASS`; real gemma4-v2: serve Tier C then **post-mine source=local Tier A** (checks=15); env: `config/personal-ai.env` |
| **Colibrì placement/LFRU/PILOT (P0–P5)** — plan/doctor dual-load safety, forest LFRU, heat-ranked mine, session KV, pilot hints | `make colibri_integrate`; `bin/cnet_plan doctor` | `COLIBRI_INTEGRATE_PASS` (21); plan: `plans/colibri_integration.md` |
| **Live residual on SoulHost/Hermes** — certified miss → residual answer + gap note; health tick structure-mines; serve stats; grow/report scripts | `make soul_residual_serve`; deploy: `scripts/deploy_hermes_mcp.sh` | `SOUL_RESIDUAL_SERVE_PASS` (19); MCP `source`/`residual` on request; residual env on live CnetMcpServer |
| **Qwythos v2 lever measurement** — full margin-distribution sweep (65,536 pinned-prefix int8 probes, both abstention margins per probe, resumable) + offline acquire-gate reproduction | `CNET_MARGIN_SWEEP` via `bin/flagship_run`; `tools/margin_sweep_analyze.py` | verdict: **`CNET_TOPK_SET`** — set semantics predicts 255/256 minable vs ordered 101/256 at margin 0.02 (+154, 0 lost); v2 = new campaign + new goldens; evidence `qwythos_english_v1.margin_sweep.tsv`, `logs/margin_sweep_verdict.txt` |
| **Linked-runtime attestation** (CNB v5) — `runtime_libs_digest` records the loaded DSOs' build-ids + glibc version beside the toolchain digest; `artifactSha256` and `runtimeLibsDigest` are projected exactly through native `SoulHost`, append-only/defaulted .NET descriptors, and MCP JSON (record, not index; zero = unattested label, never a refusal; v1–v4 bases read compatibly) | `make base`, `make gap_lane`, `make soul_host_test`, `make unified` | `base` 94 checks, `GAP_LANE_PASS checks=51`; unified asserts both exported accessor symbols and exact real-fixture MCP values; only GPU-driver/kernel folding remains deferred (`plans/toolchain_digest_linked_runtime.md`) |
| Generated claims ledgers (hermetic, GPU, and private-model evidence are separate scopes) | `make claims`, `make unified`; private evidence: `make claims_model`; inventory only: `make claims_all` | exact terminal markers required; unified verification refuses missing, failed, or pre-run logs for its 21 CPU/model-file-free claims and labels GPU/model evidence `OUT_OF_SCOPE`; strict model evidence also refuses `SKIPPED`; the all-log inventory does not claim freshness |
| Work-conserving Oracle v2 async lanes (backpressure, cancellation, deadlines, telemetry) | `make unified_async` | `ASYNC_RUNTIME_PASS` |
| Oracle v2 governed invocation | `make oracle_v2_bench` | semantic validation at 12.446 ns/call; ~80.3M calls/s, validator delta 0.676 ns (re-measured 2026-07-11; supersedes the July 10 preliminary 12.432) |
| Two physical R9700 lanes, exact CPU parity, iGPU excluded | `make unified_gpu` | 1.984× over the serial lane fixture; `ASYNC_GPU_LANES_PASS` |
| Model-universal residency (Dense + MoE + SSM descriptors) | `make unified_models` | 90 lifecycle/resource checks + 12 catalog checks; lazy load, leases, deduplicated cold loads, LRU, atomic multi-resource admission |
| Qwythos (Qwen3.5 architecture) QGKP v3 lossless envelope | `make qgkp_envelope_test` (synthetic format), `make qwythos_qgkp_acceptance` (real artifact) | `QGKP_ENVELOPE_PASS`; 1,048,576-token context metadata and payload SHA-256 preserved; mixed TQ1_0+Q4_K passes 3/3 coherence at 70.97 tok/s on GPU1 |
| **Qwythos-9B end-to-end hybrid forward** (qwen35: gated attention + Gated-DeltaNet on one residual stream, behind the `cce_gguf_qwen2` seam every oracle consumer binds to; YaRN IMROPE text reduction, tiled K→V broadcast, oracle prefix-rewind state protocol; nextn/MTP block skipped) | `make cce_qwen35_e2e` (hermetic double-precision fixture, 36 checks), `make qwythos_e2e` (real-model parity + greedy vs llama.cpp CPU), `bin/depth_probe` + flagship startup gates | Token-identical to llama.cpp: **greedy 32/32**, per-layer relL2 within envelope, ordered top-3 identical; depth_probe preflight validity 0/128, distinct 123/128; prefix ON/OFF mining **byte-identical bases** (sha-equal); depth-cap verdict DEAD (top-3 saturates only at 32/32) — spec: `docs/superpowers/specs/2026-07-14-qwythos-hybrid-forward-design.md` |
| **Qwythos english-window soul v1** (first certified base from the end-goal model; margin 0.02 abstention + SAMPLED Wilson ≥ 0.95, student 128→512/20k adaptive; recipe-recorded but exact historical build not repo-replayable: `source_dirty=1`, executable not retained) | `tools/mining_campaign.sh` + `qwythos_english_v1.cnb.manifest.json`; goldens `goldens_qwythos.txt`/`.int8.txt` (FP ≡ int8, 32/32) | **89/256 units (34.8%)**, 167 `oracle_unfit` proven seed-independent — the teacher's honestly-confident subset; an earlier 207/256 was a chimera-oracle artifact (stale recurrent state), retracted and re-mined after two oracle-integrity fixes caught by the prefix-ON/OFF unit-level A/B |
| Universal model layer (detect, SSM + llama runners, specialist graph, weight store, bounded-RAM tiers, evidence-gated merge) | `make cce_detect` / `cce_ssm` / `cce_st_llama` / `cce_specgraph` / `cce_wstore` / `cce_tiers` / `cce_similar` | passing, in `make test` |
| Autonomy loop (gap-triggered acquisition → unified CNB version 5 base under stable CNB1 magic, with v1–v4 read compatibility → flagship harness) | `make acquire`, `make base`, `make flagship` | passing, in `make test`; real gemma4-v2 12B campaign **253/256** certified (SAMPLED, Wilson ≥ 0.984), 93% live-model fidelity when queried (`soul_query`) — the earlier "256/256 at 100%" was a NaN-oracle artifact, retracted. Caveat (2026-07-15): each goldens-enabled run's FIRST unit (vocab[0]) was mined against golden-battery state (flagship lane-prefix-cache bug, fixed in `cbe130e`) — gemma bases' vocab[0] units are suspect. Update (2026-07-17): the recert attempt exposed that `goldens_gemma4v2.txt` itself was never provably recorded by a known binary (a hermetic 3-era logit checksum proves the classic forward UNCHANGED since July 14 — the file, not the oracle, is the stale artifact); fresh int8 goldens (`goldens_gemma4v2.int8.txt`) are recorded and independently reproduced 32/32, and a full differential drift audit of both gemma bases against them is the vocab[0] verdict's evidence — see the July 17 CHANGELOG rows and `*.recert.txt` |
| Restored legacy aggregate (COMPAT tier) + allocation-balance leak gate | `make compat`, `make leakcheck` | legacy aggregate quarantined out of `make test` into its own tier (`CNET_COMPAT_PASS`; also in `verify-long`); leak gate stays in `make test` |
| Loader robustness (byte-flip + truncation sweeps over every artifact loader) | `make mutate` | passing, in `make test`; sealed formats refuse every mutation, unsealed probes never crash |
| GPU forward (self-contained OpenCL, **multi-GPU + int8**) | `make gpu_equiv_build`; `make` `clgemm_unit` | optional; dual-R9700 oracle pool + `q8` layers, 4.28× on 12B, **bit-identical proven** (`clgemm_unit`, 360 calls), NaN-hard-failed |
| Supra int8 PTQ | `make supra_console` (`--int8`) | near-lossless |
| Packed 1.6-bit ternary | `make wordlm_bitnet` + export/reload tests | storage bit-exact (`ΔNLL=0` reload); *deployable quality still needs QAT at model scale* |
| Representation walls / planner scaling | `make margin` / `fuzzy` / `stochastic`, `make planner_scale_study` | documented **limits**, not claims |

Not claimed: beating PyTorch/TensorFlow globally, free-running clean prose from
the tiny char-LM, steering the 3M-param Supra model into long-form output, or
exact top-5 next-token certification in *every* teaching context — the gap lane
learned top-5 continuation in the default and lamp contexts but not the storm
context, and doubling student capacity (512→1024 hidden) did not recover it, so
it reads as graded structure in that context rather than a capacity wall. See
*Knowing the Edge* and the caveats inside each section.

## The map

The deep documentation is split by altitude (2026-07-12; unification
analysis item 8 — one document per job, all content preserved):

| Where | What |
|---|---|
| [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) | how every layer works: the Specialist type, CCE runtime, planner/contracts/lifecycle, domains, knowledge pipeline, compression, universal model layer, autonomy loop, Knowing the Edge, CNET-D, status & caveats, the full make-target table, file formats, repo layout |
| [`docs/CHANGELOG.md`](docs/CHANGELOG.md) | the dated optimization ledgers — what landed when, what evidence supports it, and which negative or retracted results must not be revived |
| [`docs/dispatch.md`](docs/dispatch.md) | the one dispatch story: recall / synthesis / policy, and the gate that pins its boundaries |
| [`docs/cnet-history.md`](docs/cnet-history.md) | the mechanism-by-mechanism chronology |
| [`docs/verified-today.generated.md`](docs/verified-today.generated.md) | machine-generated claim verdicts (`make claims`; regenerated by every `make unified`) |

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
make claims_model     # require private checkpoints/reference data and exact REAL_*_PASS evidence
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

### Portable build and native package

`VERSION` is the single release version source. `PORTABLE=1` disables
host-specific ISA tuning; the default keeps the measured native optimization.

```sh
make PORTABLE=1 ci                 # CPU-only CI/release gate
make PORTABLE=1 cnet_dll           # portable unified shared library
make PORTABLE=1 DESTDIR=/tmp/stage PREFIX=/usr install
PKG_CONFIG_SYSROOT_DIR=/tmp/stage \
  PKG_CONFIG_PATH=/tmp/stage/usr/lib/pkgconfig pkg-config --cflags --libs cnet
make DIST_DIR="$PWD/dist" dist     # reproducible cnet-<version>.tar.gz
make DESTDIR=/tmp/stage PREFIX=/usr uninstall
```

The install layout is versioned (`libcnet.so.<version>` plus ABI and linker
symlinks), installs all public headers below `include/cnet`, and publishes
`cnet.pc`. The GitHub Actions workflow exposes the same portable `make ci`
path by manual dispatch only; pushes and pull requests do not trigger paid CI.
The local `native_warning_gate` compiles the complete shared-library source set
with `-Werror` under `-Wall -Wextra -Wpedantic`. Inactive OpenMP pragmas are
source-guarded, so the portable serial build is diagnostic-free without warning
suppressions.

On Windows the Makefile works under MinGW (`mingw32-make` or `make` from
MSYS2); binaries get an `.exe` suffix automatically. The build uses
`-O3 -march=native -mno-avx` — `-mno-avx` is **required** with `-march=native`
on MinGW GCC 15.2 (aligned 256-bit moves on by-value structs segfault on a
16-byte-aligned stack). Use `PORTABLE=1` for portable binaries.

## Essential targets

`make test` (core chain, 24 suites), `make unified` (the vertical
acceptance gate + fresh claims ledger), `make compat` (the quarantined
legacy aggregate), `make gap_lane` / `make dispatch_story` /
`make specialist_health` / `make unified_specialist` (the unification
gates), `make claims`. The complete annotated table is in
[`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md#make-targets).

The design documents in `docs/superpowers/specs/` record rationale for
the overall approach.

## License

Unless a file states otherwise, CNET-authored source code and documentation
are licensed under **Apache-2.0**. See [LICENSE](LICENSE). Third-party model,
tokenizer, dataset, and other external artifacts retain their applicable
upstream terms.

The canonical repository currently remains private. Selecting Apache-2.0 does
not authorize a public release or a repository-visibility change.
