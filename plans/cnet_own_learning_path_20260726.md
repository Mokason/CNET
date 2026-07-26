# CNET Own-Learning Path (2026-07-26)

**Repo:** `/home/marble/AI/CNET` @ `7d19354`
**Baseline verified this session:** `HYBRID_AI_PASS checks=21`, `CAPABILITY_CERT_PASS certified=5/5`.
**Do not push.** Local commits OK when green.

---

## 1. Executive judgment

CNET's *learning substrate is already real*. This is the surprise of the audit, and it
changes the diagnosis. Two independent mechanisms genuinely change CNET's own weights
and are already wired to serve:

- **Certified LoRA on the serve critical path.** `registry_lora_install_orchestrator`
  (`src/router/registry_lora.c:418`) arms `registry_lora_enable_serving`, and the deployed
  lane sets `CNET_LORA_AUTO_ORCH=1` (`config/personal-ai.env:135`). The serve hook applies
  a low-rank delta to a BTN's raw output (`registry_lora.c:458`) and refuses to serve an
  uncertified adapter (`registry_lora.c:448`, `certify-before-serve`). A real adapter with a
  measured delta exists on disk: `logs/lora_store/json_toolcall_v2.lora`,
  `acc_off=0.2975 → acc_on=0.7375 (Δ+0.44)`.
- **BTN structure mining from residual.** `hybrid_structure_mine` (`src/hybrid_ai.c:360`)
  distils the Tier-C residual into an admitted student unit.

So the blocker is **not** "CNET cannot learn its own weights." It can, and it does, under a
certification gate that is better than most production ML stacks.

**The blocker is that the learning machinery is starved of real data and blind to its own
progress.** CNET built an engine and connected it to a test tank instead of the fuel line.

The single most damning piece of evidence: the fault bus — the intake for the general PEFT
learner — contains **4,788 rows, of which 4,788 are synthetic**.

```
total 4788
sources {'jtc': 4788}
units   {'json_toolcall_v2': 4788}
notes   {'mirror_labeled': 4788}
```

One synthetic seeder, one unit, one note. **Zero rows originate from a live serve miss.**
Every adapter CNET has ever certified was trained on data a test tool manufactured.

The bus was *designed* for the missing edge and the edge was never built:
`include/cnet_fault.h` documents `label_kind` as `"argmax" | "residual" | "text"` and
declares `CNET_FAULT_SRC_SURPRISE`. Both are dead identifiers — `SURPRISE` appears **only**
in its own name/parse functions (`src/cnet_fault.c:36,48`) and nothing anywhere emits
`label_kind="residual"`. The Tier-C residual serve path (`src/personal_ai.c:377-389`) records
a hit, accounts a tier, optionally mines — and **throws the (input, teacher answer) pair away**.

Second: CNET **discards the real input distribution**. `hybrid_trace_residual`
(`src/hybrid_ai.c:281-282`) *overwrites* `tr->in`/`tr->out` on every matching request. A
"trace" is one slot per port *shape* (64 slots, `HYBRID_TRACE_MAX`) holding a single
last-seen exemplar. Consequently the miner cannot train on what users actually asked; it
synthesises a one-hot basis and re-labels it through the residual
(`label_expand_rows`, `hybrid_ai.c:316`). CNET is learning a **lookup table over a canonical
basis**, not the user's distribution — and only for `PORT_ONEHOT` single-field ports
(`hybrid_ai.c:396-405`); every other port family degenerates to `n_rows = 1`.

Third: **CNET cannot see whether it is learning.** The defining KPI of own-learning —
residual dependence falling — is not persisted anywhere. `residual_hits` exists
(`include/personal_ai.h:50`, incremented `src/personal_ai.c:141`) but lives in
`ai->totals` and dies with the process. `scripts/personal_ai_loop_report.sh`, named
"Measure" in the hybrid plan, reports **inventory** (base bytes, unit count, inbox lines,
service liveness) and greps the journal. It never reports the local:residual ratio.

Fourth: **four of five certified capabilities are pass/fail smoke, not capability
measurement.** `calibrated_abstention`, `honest_memory_retrieval`, `hybrid_skill_serve`,
`sleep_consolidation` all carry `absolute_floor: 1.0, regression_budget: 0.0` and score
`metric=1.000`. Only `cce_classification` is graded (`0.861`, floor `0.45`). A binary floor
detects *breakage*; it can never show *improvement*. The cert culture is right; the
instrumentation is one bit deep.

Fifth: **sleep consolidation changes no weights.** `cnet_sleep_consolidate` dedups and
merges memory tiles, then sets `report->graduated_units = tilemem_certifiable(...)`
(`src/cnet_sleep_consolidate.c:124`) — a **count of tiles meeting a recurrence threshold**.
The code comment concedes it: *"Full BTN graduate_deterministic remains available
separately."* Sleep is a memory compactor wearing the word "graduation".

**Judgment:** do not build new learning machinery. **Connect the machinery that exists to
real traffic, retain the real distribution, and make the substitution rate a first-class,
persisted, gated metric.** The gap between CNET today and a genuine own-learner is roughly
one data edge, one reservoir, and one honest KPI — not a new architecture.

Hybrid residual stays. It is the fuel, not the ceiling: **every residual call is a labelled
training example CNET currently throws in the bin.**

---

## 2. Evidence-backed blockers

Ranked by leverage. Every row cites file:line or a command output from this session.

| # | Blocker | Evidence | Consequence |
|---|---|---|---|
| **B1** | **No organic intake.** Tier-C residual serve never emits a fault row. | `src/personal_ai.c:377-389` records hit + accounts tier, no fault emit. Fault bus 4788/4788 synthetic (`source=jtc`, `note=mirror_labeled`). `CNET_FAULT_SRC_SURPRISE` referenced only at `src/cnet_fault.c:36,48`. | The PEFT learner has **zero** real-world training signal. The flywheel has no fuel line. |
| **B2** | **Real input distribution discarded.** Traces overwrite; one exemplar per port shape. | `src/hybrid_ai.c:281-282` `memcpy` overwrite; `HYBRID_TRACE_MAX 64` (`include/hybrid_ai.h:29`). | Miner trains on a synthetic one-hot basis, not user traffic. Learns a table, not a skill. |
| **B3** | **Substitution rate unmeasured.** No persisted local:residual KPI. | `residual_hits` in-process only (`include/personal_ai.h:50`, `src/personal_ai.c:141`); `scripts/personal_ai_loop_report.sh` reports inventory + journal grep, no ratio. | "Own learning" is **unfalsifiable**. No way to tell progress from drift. |
| **B4** | **Binary capability floors.** 4/5 caps are `floor 1.0 / budget 0.0`, `metric=1.000`. | `config/capability_manifests/{calibrated_abstention,honest_memory_retrieval,hybrid_skill_serve,sleep_consolidation}.json`. Verified `CAPABILITY_CERT_PASS certified=5/5`. | Certs detect breakage only. Improvement is invisible to the gate. |
| **B5** | **Sleep graduates a number, not a unit.** | `src/cnet_sleep_consolidate.c:124` `graduated_units = tilemem_certifiable(...)`; comment at `:121-123` concedes BTN graduation is "separately". | Sleep cycle cannot change future serve behaviour. Consolidation ≠ learning. |
| **B6** | **Mining is one-hot-only.** Expansion gated on `PORT_ONEHOT && field_count==1`. | `src/hybrid_ai.c:396-405`; else branch → `n_rows = 1` (`:423-433`). | Any richer port family mines a single exemplar — effectively no learning. |
| **B7** | **Multimodal is a fixture.** Vision = hermetic 8×8, 4 shapes, not on serve path. | `src/modality_vision.c` (144 lines, `cnet_vision_hermetic_render`); no reference from `personal_ai.c`/`soul_host.c`/`hybrid_ai.c`. | The MiniCPM-V bar is not merely unmet — it is unattempted. Correctly deprioritised (see §5). |

### Candidates explicitly **refuted** by evidence

Honesty requires naming what the brief suspected that turned out false:

- **"PEFT/LoRA not on the serve critical path."** *Refuted.* It is armed in the deployed
  lane (`CNET_LORA_AUTO_ORCH=1` → `registry_lora.c:418,422`) and enforces certify-before-serve
  (`registry_lora.c:448`).
- **"Structure-mine blocked on W=256 windows."** *Stale.* `plans/cnet_cert_learn_20260725.md`
  lists this as the known gap; it has since been fixed — `expand_cap = 64` with rows spread
  across the window (`src/hybrid_ai.c:388-403`, `label_expand_rows` comment `:313-315`).
  That plan's "Known gap (next)" section should be marked resolved.
- **"Classification HEALTHY is a toy proxy."** *Partly true but the least of the problems.*
  It is the **only** graded capability CNET owns (`0.861` vs floor `0.45`, majority-baseline
  lift). The issue is not that it is graded badly — it is that nothing else is graded at all.

---

## 3. Chosen path — the **Organic Distillation Flywheel**

One path. Not four.

> **Every Tier-C residual answer becomes a labelled training example; adapters certify against
> held-out replay; the substitution rate is the score.**

```
  request
    │
    ├─► Tier A  certified BTN + certified LoRA ──hit──► serve      certified_serves++
    │                                                              (goal: ↑)
    ├─► Tier B  soft specialist (margin-gated) ──hit──► serve
    │
    └─► Tier C  Bonsai residual (external)     ──hit──► serve UNCERTIFIED
                                                  │                residual_hits++ (goal: ↓)
                                                  └──► EMIT organic fault          ◄── B1 fix
                                                       (in, teacher_out, port,
                                                        label_kind="residual",
                                                        source="surprise")
                                                  └──► RESERVOIR retain real input ◄── B2 fix
                                                       (K samples/port shape,
                                                        not 1 overwritten slot)
  ── sleep / tick ────────────────────────────────────────────────────────────────
    fault bus (organic) ─► registry_lora_tick ─► teach ─► CERTIFY on held-out ─► attach
                        └► hybrid_structure_mine ─► BTN student ─► admit
  ── next request of same shape ──────────────────────────────────────────────────
    Tier A hit  ⇒  residual_rate ↓        ◄── the measurable claim              ◄── B3 fix
```

### Why this path and not the alternatives

**vs. "reimplement a MiniCPM-V harness in C".** That buys fluency and loses the entire moat.
CNET cannot win a parameter-scale race on one ROCm box, and a C rewrite of a Python
fine-tuning harness is the *worst* trade: all of the maintenance, none of the contracts. The
flywheel instead makes fluency a **commodity input** — Bonsai is a labelling service, and any
OpenAI-compatible endpoint can replace it.

**vs. "sleep→specialist" as primary.** Sleep is real but currently changes nothing (B5).
Making sleep graduate units before there is organic data to consolidate would graduate
**synthetic** regularities. Sleep is milestone M5, *after* the fuel line exists — otherwise it
consolidates the test tank.

**vs. "micro-VLM student" as primary.** B7 is real but it is a *breadth* problem, and CNET's
deficit is *depth of loop*. Adding a modality to a loop that does not close multiplies an
unmeasured quantity. Vision is a 90-day option (M8) and enters as **another Tier C**, never as
a thing CNET clones.

**Why it fits CNET's contracts specifically.** The flywheel is the only design where CNET's
existing distinctives are load-bearing rather than decorative:
- *certify-before-serve* (`registry_lora.c:448`) is precisely the anti-collapse guard that a
  distillation loop requires — CNET already has the mechanism a naive distiller lacks;
- *abstention* keeps the loop honest — a miss that abstains is still a captured gap, not a
  fabricated label;
- *provenance* means every certified adapter can name the teacher answer that trained it;
- *ports* give a natural, typed key for the reservoir and for held-out splits.

The fault bus's dead `"residual"` / `SURPRISE` identifiers are the tell: **the original design
intended this edge.** We are finishing it, not inventing it.

### The anti-collapse rule (non-negotiable)

> **CNET may train on an external teacher's answer, a user correction, or a verified tool
> result. CNET may never train on its own Tier-A/Tier-B output.**

Enforced structurally: only the Tier-C branch emits capture rows, and every row is stamped
`source=surprise, label_kind=residual` so any consumer can filter provenance. Certification
runs on a **held-out** split the adapter never trained on.

---

## 4. Milestones and gates

### 30 days — close the loop and make it visible

| M | Deliverable | Gate |
|---|---|---|
| **M1** | **Organic intake.** Tier-C residual serve emits labelled fault (`source=surprise`, `label_kind=residual`), env-gated, dedup-aware. | `make own_learning_loop` → `OWN_LEARNING_LOOP_PASS` ✅ *shipped this session* |
| **M2** | **Substitution KPI persisted.** `personal_ai_kpi_json` + `logs/own_learning_kpi.json`. | folded into `own_learning_loop` ✅ *shipped this session* |
| **M3** | **Input reservoir.** Retain K real (in,out) samples per port shape instead of one overwritten exemplar; miner trains on real traffic. | `make residual_reservoir` → `RESIDUAL_RESERVOIR_PASS` |
| **M4** | **Replay bench.** Capture N real requests, measure residual_rate before/after one consolidation cycle. Floor: rate must **fall**. | `make residual_substitution_bench` → `SUBSTITUTION_BENCH_PASS rate_before>rate_after` |

### 90 days — make it compound

| M | Deliverable | Gate |
|---|---|---|
| **M5** | **Sleep graduates units.** Replace count-only `graduated_units` with real BTN graduation + provenance. | `make sleep_graduate` → `SLEEP_GRADUATE_PASS units>0` |
| **M6** | **Graded capability floors.** Convert the four binary caps to measured metrics with real regression budgets. | `make capability_cert` with ≥4 graded metrics, none `floor==1.0 && budget==0.0` |
| **M7** | **Promotion requires beating the teacher.** A unit may be admitted only if held-out ≥ residual on the same fixtures. | `make promote_gate` → `PROMOTE_GATE_PASS` |
| **M8** | *(optional)* **Vision as second Tier C.** MiniCPM-V-class student behind the residual port; CNET owns certified decoders, not the VLM. | `make vision_residual` → `VISION_RESIDUAL_PASS` |

---

## 5. KPI dashboard

Emitted to `logs/own_learning_kpi.json`; the first four are the scoreboard.

| KPI | Definition | Direction | Gaming risk / joint guard |
|---|---|---|---|
| `residual_rate` | `residual_hits / served` | **↓** | Falls if CNET abstains more — **must** be read with `abstain_rate`. |
| `substitution_rate` | `local_hits / (local_hits + residual_hits)` | **↑** | The headline own-learning number. |
| `certified_serves` | Tier-A hits | ↑ | — |
| `organic_fault_rows` | fault rows with `source=surprise` | **↑ from 0** | Today: **0**. Any value > 0 is new capability. |
| `synthetic_fault_share` | `jtc+synth / total` | **↓ from 1.00** | Today: **1.00**. |
| `abstain_rate` | `abstains / served` | flat or ↓ | Guards `residual_rate` gaming. |
| `honest_miss_rate` | `(abstains + gap_noted) / served` | flat | Rising = CNET hiding misses. |
| certified adapters | store count + `rejected` from lora tick | ↑ / ↓ | Rejections rising = distilling noise. |
| capability floors | graded metrics from `capability_cert.json` | ↑ | Binary floors excluded from "improvement". |

**Today's honest reading:** `organic_fault_rows = 0`, `synthetic_fault_share = 1.00`,
`residual_rate` unmeasured. Those three numbers *are* the gap.

---

## 6. Risks and anti-patterns

**Risks**

1. **Distilling the teacher's errors.** Bonsai is wrong sometimes; the flywheel would bake it in.
   *Mitigation:* certify-before-serve (`registry_lora.c:448`) + held-out split + M7 (must beat
   teacher, not merely match).
2. **Self-train collapse.** *Mitigation:* the anti-collapse rule (§3) — Tier-C-only capture,
   provenance-stamped rows, never Tier-A output.
3. **Metric gaming via abstention.** *Mitigation:* `residual_rate` is never read without
   `abstain_rate`.
4. **Dedup hides volume.** `cnet_fault_mirror_labeled` dedups by input hash
   (`src/cnet_fault.c:414-422`), so a repeated real request logs once — good for storage,
   misleading for "how much traffic did we see". *Mitigation:* KPI counts serves, not rows.
5. **Capture cost on the serve path.** File append per residual miss. *Mitigation:* env-gated,
   requires `CNET_FAULT_LOG`, off by default in tests; dedup already bounds writes.
6. **One-hot tunnel vision (B6).** The reservoir (M3) must not inherit the `PORT_ONEHOT` gate.

**Anti-patterns to refuse**

- Counting `graduated_units` (a tile count) as evidence of learning.
- Binary certs presented as capability measurement.
- Any KPI computed from synthetic seeder rows.
- Declaring the loop closed before `organic_fault_rows > 0` on a **real** serve.

---

## 7. Non-goals (explicit)

1. **No full native LLM pretraining.** CNET does not have, and does not need, datacenter scale.
2. **No replacing Bonsai now.** Tier C is the fuel line. Success is *lower residual rate on
   covered shapes*, not residual removal.
3. **No Python-first rewrite of CNET core.** Steal patterns (tiny specialist + compression +
   fine-tune on failures), never the harness or the product identity.
4. **No CUDA-only paths.** AMD/ROCm; GPU_0/1 house rules hold.
5. **No new adapter lifecycle.** `src/specialist_adapters.c:1` already warns: PEFT path of
   record is `registry_lora` / `registry_lily` / `cce_adapter_bank`. Do not fork a third.
6. **No claiming multimodality** while vision is an 8×8 fixture (B7).

---

## 8. Spike list (smallest code that could falsify the path)

| S | Spike | Falsifies | Status |
|---|---|---|---|
| **S1** | Residual serve → organic fault row + persisted KPI | "the loop cannot be closed without redesign" | ✅ **shipped** — `make own_learning_loop` |
| **S2** | Reservoir of K real inputs per port shape | "real traffic is too sparse/heterogeneous to train on" | next |
| **S3** | Replay bench: residual_rate before/after a cycle | **"certified units actually displace residual calls"** — the core claim | next, highest value |
| **S4** | Sleep graduates one real BTN unit | "consolidation can change serve behaviour" | 90d |
| **S5** | Graded floor for one binary capability | "certs can measure improvement, not just breakage" | 90d |

**S3 is the experiment that decides whether this path is true.** If captured organic traffic
trains adapters that do *not* reduce residual rate on held-out replay, the flywheel is a
distillation toy and the strategy must change. Everything before S3 exists to make S3
runnable.

---

## 9. First concrete next step

`make residual_substitution_bench` (S3) — build on the S1 capture that now exists. Files to
touch: `src/hybrid_ai.c` (reservoir, B2), `tests/residual_substitution_bench.c` (new), one
manifest in `config/capability_manifests/`.
