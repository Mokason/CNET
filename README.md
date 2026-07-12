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

Rechecked end-to-end on this host (2026-07-12, post legacy-quarantine): `make
test` — all 24 core suites green in **2:05.67** wall; `make compat` — the
quarantined legacy aggregate green in its own tier; `make unified` —
`CNET_UNIFIED_PASS` with a fresh, strict **16/16** in-scope claims ledger; the
physical-GPU lane is reported explicitly as out of scope rather than accepted
from an older log.

| Area | Verified by | Status |
|---|---|---|
| CCE runtime (tensor → block → cascade → archive → forest → router → learn) | `make cce_smoke`, `make cce_train_bench` | passing (standalone gates) |
| CCE storage + loaders (C ABI/DLL, safetensors, autograd, model save/load, zero-copy WARM views) | `cce_dll`, `cce_safetensors_test`, `cce_autograd_test`, `cce_model_test`, `cce_view`, `forest_view` | passing, in `make test` |
| Contract security + one-file sealed units | `make contract_secure`, `make contract_unit` | passing, in `make test` |
| Contract correctness + robust promotion quality/speed | `make contract_optimized` | malformed authored/frozen contracts refused atomically; stronger certified margin wins with one replay per model |
| Unified contract runtime (BTN + CCE + Oracle v2 evidence-carrying specialists + CNB version 2 semantics under stable CNB1 magic + planner/executor + `SoulHost` + .NET/MCP projection) | `make unified` | CPU-only, model-free vertical acceptance gate; Oracle descriptors remain provenance until independently certified/admitted |
| **One `Specialist` type across kinds** — a native BTN, a real CCE model, and an Oracle unit as ordinary nodes of ONE certified plan (route + DAG), admitted through the single door (`specialist_admit`), trust/residency/role axis views | `make specialist_unit`, `make unified_specialist` | `SPECIALIST_UNIT_PASS`, `HET_PLAN_PASS`, in `make unified`; edge refusals, lifecycle axes, and the heterogeneous strict-exact plan are gated |
| **Runtime health optimizer** — one opt-in maintenance pass that fixes (audit → contract/teacher fault labeling → heal via retrain + passing re-certify) and improves (evidence promotion, shadow hot-swap) through existing certified paths only | `make specialist_health` | `SPECIALIST_HEALTH_PASS` (17 checks), in `make unified`; healthy registry = proven no-op, zero-init config = total no-op; demoted adapters stay RESET — their repair path is re-acquisition  Wired into SoulHost/MCP: `soul_health_tick` C ABI, `cnet_health_tick` tool, opt-in `CNET_HEALTH_TICK_SECONDS` timer |
| **One dispatch story** ([`docs/dispatch.md`](docs/dispatch.md)) — "which specialist runs" has one answer in three layers with machine-checked boundaries: recall (`cce_router` SSMax) dispatches INSIDE a Specialist under its contract; the certified planner is the sole cross-specialist composition authority; orchestrators are policy above the planner | `make dispatch_story` | `DISPATCH_STORY_PASS` (15 checks), in `make unified`: a two-branch model routes per input yet certifies and replays as ONE node; evidence never outranks certification; among the certified, learned reliability ranks |
| **The gap lane** — the 24/7 learning runtime: serving misses land in a gap inbox, the lane ingests them into the persistent ledger, bridges unhealed demotions to HEALTH gaps, and drains by teaching from a bound local model (dynamic-growth students, PROOF/SAMPLED certification, sealed into the base); checkpoints are atomic for base AND ledger; a fresh lane resumes from disk | `make gap_lane` | `GAP_LANE_PASS` (39 checks), in `make unified`; daemon `bin/gap_lane_run` (`make gap_lane_run_build`) deployed as the `cnet-gap-lane` user service teaching from gemma4-v2 12B (CPU, int8-on-load) Novel goals flow in by explicit typed signature: the `cnet_request_capability` MCP tool (`soul_request` ABI) serves the request now when a certified plan exists, answers capability probes without executing, and otherwise queues the full signature to the inbox — a goal no longer needs an existing unit to become the lane’s work. The wildcard-identity plan (length 0) counts as no plan, same rule as `soul_route` Teaching is corpus-drawn: `CNET_WINDOW_FILE` maps the one-hot alphabet to real corpus token ids (the english window) and `CNET_LANE_CONTEXT_FILE` pins a real tokenized prose prefix in the KV once — every teaching call is an independent probe of prefix+token (`cce_gguf_qwen2_forward_probes`, KV never modified), with the head restricted to the window ids (bit-identical, fraction of the cost). Bare single-token contexts remain the fallback Multiple named contexts per lane: every `<name>.ids` in `CNET_LANE_CONTEXT_DIR` is a pinned-able context selected by goal-tag prefix (`<name>_…`); the teacher re-pins the KV on context switch (once per gap, not per point), and each context carries its own provenance fingerprint into the ledger — proven live with lamp/storm units closed in one tick whose transitions differ by context on identical inputs Unit provenance is a direct relation: a closed gap’s teacher persists as a CNB oracle descriptor (kind `gap_lane_teacher`; identity = the model artifact’s BYTES via streamed FNV — the file’s content, not its path — window config, context retrieval snapshot, taught-signature contract digests, and the teaching stack’s toolchain digest), idempotent by name, and the minted unit points at its descriptor directly (`CnbUnitRef.provenance`, CNB v3 with v1/v2 read compat; the gaps ledger v2 carries the minted unit name; projected by `soul_unit_provenance`), surviving resume, served by `cnet_list_oracles` under the existing `descriptor_only_not_runtime_trust` admission label. Reconciliation does its expensive work per closed gap once (runtime done-marks — every dirty pass still traverses the row list; a persistent cursor/index is open work) and stays retryable on write failure. Audited caveats (2026-07-12, in the changelog): descriptor reuse is by name without identity comparison, and the toolchain digest attests compiler+ABI only — identity-aware rebinding and hermetic build attestation remain open |
| Generated claims ledger (unified gate logs → machine-readable run-scoped verdicts) | `make claims`, `make unified`; inventory only: `make claims_all` | exact terminal markers required; current verification refuses missing, failed, or pre-run logs for its 16 CPU/model-file-free claims and labels the GPU-only lane `OUT_OF_SCOPE`; the all-log inventory does not claim freshness |
| Work-conserving Oracle v2 async lanes (backpressure, cancellation, deadlines, telemetry) | `make unified_async` | `ASYNC_RUNTIME_PASS` |
| Oracle v2 governed invocation | `make oracle_v2_bench` | semantic validation at 12.446 ns/call; ~80.3M calls/s, validator delta 0.676 ns (re-measured 2026-07-11; supersedes the July 10 preliminary 12.432) |
| Two physical R9700 lanes, exact CPU parity, iGPU excluded | `make unified_gpu` | 1.984× over the serial lane fixture; `ASYNC_GPU_LANES_PASS` |
| Model-universal residency (Dense + MoE + SSM descriptors) | `make unified_models` | 90 lifecycle/resource checks + 12 catalog checks; lazy load, leases, deduplicated cold loads, LRU, atomic multi-resource admission |
| Qwythos (Qwen3.5 architecture) QGKP v3 lossless envelope | `make qgkp_envelope_test` (synthetic format), `make qwythos_qgkp_acceptance` (real artifact) | `QGKP_ENVELOPE_PASS`; 1,048,576-token context metadata and payload SHA-256 preserved; mixed TQ1_0+Q4_K passes 3/3 coherence at 70.97 tok/s on GPU1 |
| Universal model layer (detect, SSM + llama runners, specialist graph, weight store, bounded-RAM tiers, evidence-gated merge) | `make cce_detect` / `cce_ssm` / `cce_st_llama` / `cce_specgraph` / `cce_wstore` / `cce_tiers` / `cce_similar` | passing, in `make test` |
| Autonomy loop (gap-triggered acquisition → unified CNB version 2 base under stable CNB1 magic, with v1 read compatibility → flagship harness) | `make acquire`, `make base`, `make flagship` | passing, in `make test`; real gemma4-v2 12B campaign **253/256** certified (SAMPLED, Wilson ≥ 0.984), 93% live-model fidelity when queried (`soul_query`) — the earlier "256/256 at 100%" was a NaN-oracle artifact, retracted |
| Restored legacy aggregate (COMPAT tier) + allocation-balance leak gate | `make compat`, `make leakcheck` | legacy aggregate quarantined out of `make test` into its own tier (`CNET_COMPAT_PASS`; also in `verify-long`); leak gate stays in `make test` |
| Loader robustness (byte-flip + truncation sweeps over every artifact loader) | `make mutate` | passing, in `make test`; sealed formats refuse every mutation, unsealed probes never crash |
| GPU forward (self-contained OpenCL, **multi-GPU + int8**) | `make gpu_equiv_build`; `make` `clgemm_unit` | optional; dual-R9700 oracle pool + `q8` layers, 4.28× on 12B, **bit-identical proven** (`clgemm_unit`, 360 calls), NaN-hard-failed |
| Supra int8 PTQ | `make supra_console` (`--int8`) | near-lossless |
| Packed 1.6-bit ternary | `make wordlm_bitnet` + export/reload tests | storage bit-exact (`ΔNLL=0` reload); *deployable quality still needs QAT at model scale* |
| Representation walls / planner scaling | `make margin` / `fuzzy` / `stochastic`, `make planner_scale_study` | documented **limits**, not claims |

Not claimed: beating PyTorch/TensorFlow globally, free-running clean prose from
the tiny char-LM, or steering the 3M-param Supra model into long-form output —
see *Knowing the Edge* and the caveats inside each section.

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

## Essential targets

`make test` (core chain, 24 suites), `make unified` (the vertical
acceptance gate + fresh claims ledger), `make compat` (the quarantined
legacy aggregate), `make gap_lane` / `make dispatch_story` /
`make specialist_health` / `make unified_specialist` (the unification
gates), `make claims`. The complete annotated table is in
[`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md#make-targets).

The design documents in `docs/superpowers/specs/` record rationale for
the overall approach.
