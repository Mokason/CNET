# CNET Own-Learning Path (2026-07-26)

**Repo:** `/home/marble/AI/CNET` @ `7d19354`
**Baseline verified this session:** `HYBRID_AI_PASS checks=21`, `CAPABILITY_CERT_PASS certified=5/5`.
**Status:** M1–M4 + S6/S7 shipped and pushed to `origin/master` (2026-07-26). Next: S8 (§12).

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
| **M3** | **Input reservoir.** Retain K real (in,out) samples per port shape instead of one overwritten exemplar; miner trains on real traffic. | `make residual_reservoir` → `RESIDUAL_RESERVOIR_PASS` ✅ *shipped (checks=12, K=6)* |
| **M4** | **Replay bench.** Capture N real requests, measure residual_rate before/after one consolidation cycle. Floor: rate must **fall**. | `make residual_substitution_bench` → `SUBSTITUTION_BENCH_PASS` ✅ *shipped: 1.0000 → 0.0000, accuracy flat at 1.0000* |

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
| `residual_rate` | `residual_hits / served` | **↓** | Falls if CNET abstains more, **and** falls when a unit answers out-of-coverage inputs wrongly (measured, §10 E2). Never read without `abstain_rate` **and** held-out accuracy. |
| `heldout_accuracy` | correct / served on inputs absent from capture | **flat or ↑** | The guard that makes `residual_rate` safe to read. A fall here voids any substitution win. |
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
| **S2** | Reservoir of K real inputs per port shape | "real traffic is too sparse/heterogeneous to train on" | ✅ **shipped** — `make residual_reservoir` |
| **S3** | Replay bench: residual_rate before/after a cycle | **"certified units actually displace residual calls"** — the core claim | ✅ **confirmed** (§10 E1); limit measured (§10 E2) |
| **S6** | Coverage-gated abstention: a unit abstains outside certified coverage | "CNET can refuse to answer what it did not learn" — closes §10 E2 | ✅ **shipped** — `make coverage_abstain`, §11 |
| **S7** | Persist coverage so it survives a lane restart | "the S6 protection is durable, not process-local" | ✅ **shipped** — `<base>.coverage`, §12 |
| **S8** | One shared durable seal for mined units | "a mined unit survives restart, and both callers seal identically" | ✅ **shipped** — `hybrid_seal_mined_unit`, §13 |
| **S4** | Sleep graduates one real BTN unit | "consolidation can change serve behaviour" | 90d |
| **S5** | Graded floor for one binary capability | "certs can measure improvement, not just breakage" | 90d |

**S3 is the experiment that decides whether this path is true.** If captured organic traffic
trains adapters that do *not* reduce residual rate on held-out replay, the flywheel is a
distillation toy and the strategy must change. Everything before S3 exists to make S3
runnable.

---

## 10. S3 result (2026-07-26) — the flywheel works, and its limit is now measured

`make residual_substitution_bench` runs two experiments on a **multi-field** one-hot port
(`field_count=2`), a shape the old miner could not expand at all — it would have mined a
single exemplar. Two arms, identical traffic, one difference (consolidation).

### E1 — in-coverage substitution (this gates)

```
arm=control      replay served=16 local=0  residual=16   (no consolidation)
arm=consolidate  replay served=16 local=16 residual=0    (mined from 16 real rows)

residual_rate      1.0000 → 0.0000  (Δ -1.0000)
substitution_rate  0.0000 → 1.0000
accuracy           1.0000 → 1.0000  (no capability traded away)
abstain_rate       0.0000 → 0.0000  (the drop was not bought by abstaining)
```

**The core claim of the path is confirmed:** units mined from captured real traffic displace
100% of the teacher's calls on that port shape, with no loss of accuracy and no extra
abstention. Own weights genuinely substitute for the residual.

### E2 — out-of-coverage generalisation (reported, deliberately tested to failure)

Mined from 12 of 16 pairs, then replayed the 4 pairs never seen during capture.
**These are the pre-S6 numbers; §11 shows what they became.**

```
heldout served from own weights: 4/4 (1.0000)
heldout CORRECT:                 0/4
replay accuracy:                 0.7500   (was 1.0000 under the teacher)
```

**The unit answered every unseen input from its own weights and got every one wrong.** It
replaced a correct teacher answer with a confident wrong one, and `residual_rate` reported
that as a total win. This is the single most important result of the session:

> **Substitution without generalisation is a capability regression that the headline KPI
> scores as success.**

Mining memorises captured coverage; it does not extrapolate. Three consequences, now
evidence-backed rather than speculative:

1. **M7 is not optional, it is load-bearing.** Promotion must require beating the teacher on
   *held-out* evidence. Serving more traffic is not evidence of learning.
2. **Coverage-gated abstention is the missing guard.** A certified unit should abstain
   outside its certified coverage and fall through to Tier C, rather than answer confidently.
   This is CNET's own distinctive (contracts + abstention) applied to its own units.
3. **`residual_rate` must never be read alone.** The KPI dashboard already pairs it with
   `abstain_rate`; this result proves it must also be paired with **accuracy on held-out**.
   Added to §5 as a gating KPI, not a nice-to-have.

The bench's PASS marker deliberately carries `heldout_correct=0/4` so a green gate can never
be misread as "CNET generalises".

### Latent bug found and fixed en route

`external_teacher.c` set `tc->onehot_in = (input_port.family == PORT_ONEHOT)`, ignoring
`field_count`. The one-hot fast path uses `argmax(in)` as a **row index** into the teacher
table, which is only valid when row *r* is hot at index *r*. For a multi-field port that
argmax is field 0's symbol, so the teacher returned the **wrong target** and the student
trained on mislabelled data — admission then failed certification (`mine_rc=1`).

Dormant until now because multi-field ports never reached this path before the reservoir. The
fix verifies the identity-basis invariant at bind time instead of inferring it from the port
family, and falls back to exact nearest-neighbour otherwise. It also silently affected the
large-window (Bonsai W=256) path, where `argmax` exceeded `n_rows` and the teacher returned
an error every row, forcing a fallback — correct by luck, now correct by construction.

---

## 9. First concrete next step

**S9 — see §13.** S8 landed: one shared durable seal (`hybrid_seal_mined_unit`) used by both
`personal_ai_structure_mine` and `soul_host`, certifying on the rows the unit was actually
mined on. Next load-bearing items are tracked in §13.

---

## 11. S6 result (2026-07-26) — coverage-gated abstention closes E2

`make coverage_abstain` → `COVERAGE_ABSTAIN_PASS checks=18 heldout_correct=4/4 was=0/4`

### Margin cannot solve this (measured first, before building anything)

The obvious reuse — `CNET_RESIDUAL_MIN_MARGIN`, hybrid soft `min_margin`,
`cnet_governance_decide` — is **blind to this failure**. Probing every replay serve of the
E2 unit:

```
idx  held  tier    margin    correct
  0        local  1.00000    yes
  3  HELD  local  1.00000    NO      [0.000 0.000 1.000 0.000]
  6  HELD  local  1.00000    NO      [0.000 1.000 0.000 0.000]
  9  HELD  local  1.00000    NO      [1.000 0.000 0.000 0.000]
 12  HELD  local  1.00000    NO      [0.000 1.000 0.000 0.000]
```

A mined BTN emits a **saturated one-hot**. Margin is exactly 1.00000 on the inputs it has
never seen and gets wrong — identical to the ones it gets right. Confidence carries zero
information here, so every margin-keyed abstention surface in the tree is structurally
incapable of catching an out-of-coverage answer. **Coverage has to be membership, not
confidence.** This is worth recording because "just add a margin threshold" is the natural
first instinct and it would have shipped a gate that does nothing.

### Mechanism

A contract certifies over a **domain**. `hybrid_structure_mine` now records the exact input
rows a unit was certified on (`hybrid_coverage_record`), and the Tier-A serve branch consults
`hybrid_coverage_admits` before claiming a certified answer. Outside coverage it declines and
falls through to Tier B/C, so the teacher answers instead. Default-allow when no record
exists, so hand-admitted and full-basis units are untouched.

### E2 before → after

| E2 (mined from 12 of 16) | before S6 | after S6 |
|---|---|---|
| held-out served from own weights | 4/4 | **0/4** |
| held-out deferred to teacher | 0/4 | **4/4** |
| held-out **correct** | 0/4 | **4/4** |
| replay accuracy | 0.7500 | **1.0000** |
| coverage abstains | — | 4 |

E1 is unchanged — `residual_rate 1.0000 → 0.0000`, `substitution_rate 1.0000`,
`accuracy 1.0000`, `abstain_rate` flat. **In-coverage substitution is fully preserved; only
the unearned claims are refused.**

The bench now fails closed on this: it refuses to pass if any held-out input is answered from
own weights and wrong (`reason=out_of_coverage_wrong`), and equally if abstention merely drops
the request instead of deferring (`reason=heldout_not_answered`). Abstaining into silence is
not a fix.

`make coverage_abstain` also reproduces the regression on demand: with
`CNET_COVERAGE_ABSTAIN=0` the unit answers all 4 held-out inputs itself and gets all 4 wrong
again. The gate is demonstrably the thing doing the work, not an incidental change.

### Scope and limits (honest)

- **`PORT_RAW` is not gated.** Exact membership is meaningless for continuous values; gating
  it would abstain on everything. Asserted in the gate so it cannot drift silently. RAW-port
  units remain exposed to the E2 failure — a real remaining hole.
- **Exact match only.** Discrete families (`ONEHOT`, `BINARY_*`) are gated bitwise. Anything
  needing near-neighbour or interval coverage is future work.
- **The MoE hard-expert branch** (`cnet_moe_try_hard`, ahead of Tier A) is **not** gated.
  Mined units are named `hyb_struct_N` and are not reachable by goal-tag dispatch today, so
  the path is currently unexposed — but it is an ungated door if that ever changes.
- **`HYBRID_COVERAGE_MAX` is 8** port shapes; a ninth mined shape records no coverage and
  therefore default-allows. Bound to raise when more shapes are mined.
- Coverage is **in-memory**, not persisted to the CNB. After a restart a mined unit reloads
  from the base with no coverage record and default-allows — so this protection does not yet
  survive a lane restart. **Highest-priority follow-up.**

### KPI

`coverage_abstains` is now in `personal_ai_kpi_json`. Read it beside `residual_rate`: a
healthy loop shows residual_rate falling *while* coverage_abstains stays proportional to
genuinely novel traffic. Coverage abstains climbing toward the serve count means the mined
library has gone stale relative to what users are asking.

---

## 12. S7 result (2026-07-26) — the gate survives a restart

`make coverage_abstain` → `COVERAGE_ABSTAIN_PASS checks=36 heldout_correct=4/4 was=0/4`

S6 protected a running process. The deployed lane runs for days with
`CNET_PERSONAL_STRUCTURE_MINE_ON_SERVE=1`, so a gate that lapses on restart is a gate that
lapses exactly where it matters.

### Mechanism

Coverage travels beside the base as **`<base>.coverage`**, the same sidecar convention already
used by `<base>.gaps.txt`, `<base>.inbox`, `<base>.curiosity` and `<base>.evidence.jsonl`.
Written on every successful mine (`personal_ai_structure_mine`, which the tick path now also
routes through), reloaded in `personal_ai_open` **before anything can serve**. Text format
with `%.17g` so doubles round-trip bit-exactly — membership is a `memcmp`, and a lossy
round-trip would silently abstain on inputs that *are* covered. The gate asserts that
bit-exactness directly.

CNB-native was considered and rejected: the coverage set belongs to a *mined* unit, the CNB
unit record has no field for a certified input domain, and widening the on-disk format would
version every existing base for a property only mined units carry. The sidecar reloads with
the base and costs nothing when absent.

### Round-trip evidence

The gate seals the mined unit durably the way the live path does (`soul_host` structure seal:
`cnb_add_unit` + checkpoint), closes, and reopens the same base:

```
S7: 12 certified rows restored from disk                       PASS
S7: coverage abstains survive the restart                      PASS   (4)
S7: in-coverage traffic still served locally after restart     PASS   (>=12 local hits)
S7: held-out still CORRECT 4/4 after restart                   PASS
S7: restored rows match bit-exactly (%.17g round-trip)         PASS
```

And the negative control that proves the sidecar is the thing doing the work — delete only
the coverage file, reopen the same base with the unit still sealed in the CNB:

```
S7: no coverage restored => no abstains                        PASS   (0)
S7: unit answers all held-out itself again                     PASS   (4/4)
S7: and gets all 4 WRONG — the sidecar is load-bearing         PASS   (0/4 correct)
```

### Found en route: personal_ai's mine path never sealed durably

`hybrid_structure_mine` admits into the `PrimitiveRegistry` only; nothing calls `cnb_add_unit`.
`gap_lane_checkpoint` saves `L->base`, which the mined unit was never added to — so a unit
mined through `personal_ai` is **process-local and lost on restart**. This is why `soul_host`
carries its own separate structure seal (`src/soul_host.c:621`, student self-labels +
`cnb_add_unit`, commented "durable seal optional").

Not fixed here — it is a behaviour change to the mine path, not a safety hole, and S7's job
was the gate. Recorded as **S8**. Note the interaction: while mined units are process-local,
a restart loses the unit *and* the risk with it; the dangerous configuration is precisely the
one `soul_host` creates, which is what §12's round-trip reproduces.

One detail worth keeping: sealing against raw `btn_forward` output fails
`contract_slice_valid` — the student's raw vector is not canonical for the port. The gate
seals against the teacher labels the unit was certified on, which are canonical by
construction and are the same rows the coverage record holds. `soul_host`'s self-label seal
is likely failing the same way in production and silently returning 0 ("durable seal
optional") — worth checking under S8.

### Remaining limits (unchanged from §11 unless noted)

- **Corrupt/absent sidecar fails open**, logging to stderr. Fail-closed is not possible
  without knowing which shapes were mined — which is what the file itself carries.
- `PORT_RAW` ungated; exact-match only; `HYBRID_COVERAGE_MAX` 8 shapes; MoE hard-expert
  branch ungated. All as §11.
- The sidecar is **not** written atomically (no temp+rename); a crash mid-write can truncate
  it, which degrades to fail-open on next load.

---

## 13. S8 result (2026-07-26) — one durable seal, shared

`make coverage_abstain` → `COVERAGE_ABSTAIN_PASS checks=37 heldout_correct=4/4 was=0/4`

### The bug was worse than "not sealed"

`hybrid_structure_mine` admits into the `PrimitiveRegistry` only, so a unit mined through
`personal_ai` was process-local. `soul_host` had the only durable seal — and it **rebuilt a
synthetic basis** at seal time rather than using the rows the unit was mined on:

```c
n_rows = in_dim <= 16 ? in_dim : 16;
for (r = 0; r < n_rows; r++)
    for (j = 0; j < in_dim; j++)
        inputs[r * in_dim + j] = (j == r) ? 1.0 : 0.0;   /* single hot bit */
```

Two independent defects:

1. **It certified a domain the unit was never trained on.** The sealed contract's exemplars
   and the coverage record would disagree about what the unit is certified for.
2. **For any multi-field port those rows are invalid.** A 2-field one-hot needs one hot bit
   *per field*; a single hot bit over the whole vector leaves field 1 all zeros, so
   `contract_slice_valid` → `port_validate` rejects it, `contract_init_borrowed` fails, and
   the code logged and `return 0`'d as "durable seal optional". **The unit silently never
   became durable** — the exact failure reproduced as `rc=-5` while building §12's gate.

### The fix

`hybrid_seal_mined_unit(h, base, stu, &reused)` is now the single seal, used by both
`personal_ai_structure_mine` and `soul_structure_mine`. It finds the coverage record by the
student's own ports and certifies on **the rows the unit was mined and certified on** —
canonical for the port by construction, and identical to what coverage gates. It never
invents rows; with no labelled record it returns 1 rather than sealing something arbitrary.

`personal_ai_structure_mine` now seals, persists coverage, and checkpoints on every
successful mine. The mine already costs thousands of training epochs, so the I/O is noise
beside it, and the unit plus its guard become durable the moment they exist.

### Evidence

The gate no longer hand-seals anything — the product does it:

```
S8: capture 12/16, mine — product seals+checkpoints            PASS
S8: mined unit itself restored from the CNB (no hand-seal)     PASS   (cnb_has_unit)
S7: 12 certified rows restored from disk                       PASS
S7: coverage abstains survive the restart                      PASS   (4)
S7: in-coverage traffic still served locally after restart     PASS   (>=12 local)
S7: held-out still CORRECT 4/4 after restart                   PASS
```

Both negative controls still hold (`CNET_COVERAGE_ABSTAIN=0`, and deleting only the sidecar).

---

## 14. Fail-open sweep (2026-07-26) — four doors closed

`make coverage_abstain` → `COVERAGE_ABSTAIN_PASS checks=49 heldout_correct=4/4 was=0/4`

After S8 the gate was correct but several paths around it could still silently drop it.
Each of these is a *fail-open*: the protection disappears and nothing reports it.

### 1. Coverage sidecar was truncated in place

`hybrid_coverage_save` opened the live file with `"w"`. A crash or a full disk mid-write
leaves a corrupt sidecar, which loads as "no coverage" and reopens the confident-wrong hole.
Now written to `<path>.tmp` and `rename(2)`d over — the same temp+rename pattern
`ledger_save_atomic` already uses. Gated: no `.tmp` survives a successful save, and a failed
write leaves the previous good file intact.

### 2. A unit could be admitted with no coverage record

`HYBRID_COVERAGE_MAX` was **8** while `HYBRID_TRACE_MAX` is **64**, and the miner discarded
`hybrid_coverage_record`'s return. Mining a 9th distinct port shape therefore admitted a
certified unit with no coverage record — and `hybrid_coverage_admits` default-allows, so the
unit served ungated. Reachable with ordinary multi-shape traffic; this was a broken door, not
a tuning constant.

Two changes: the table is sized to `HYBRID_TRACE_MAX` (a mine can only come from a trace, so
in-process overflow is now unreachable), and the miner **reserves the slot before admitting**
— if the shape cannot be gated, it refuses to mine at all (`rc=2`) rather than create a unit
it cannot constrain. Demotion was considered and rejected: `PRIM_RESET` only excludes from
planning when `reg->lifecycle_enabled`, which `registry_init` leaves zero.

### 3. Mined units had a dangling name pointer (memory-safety)

`registry_add` stores the name **pointer**, not a copy (`src/router/registry.c:501`), and
`hybrid_structure_mine` passed its stack-local `char name[64]`. Every structure-mined
registry entry therefore held a pointer into a dead stack frame — undefined behaviour on any
later read (`find_named`, `gap_lane_persist_stats` → `cnb_has_unit`, diagnostics).

It also had a security-relevant side effect: `find_named` could never match a mined unit, so
the MoE hard-expert path *appeared* unreachable. That apparent safety was an accident of
undefined behaviour, not a property.

Fixed with a deliberately never-freed copy. The registry does not own names and can outlive
the `HybridAi`, so no other lifetime is safe; it is one small allocation per successful mine,
bounded by the coverage table.

### 4. The MoE hard-expert door bypassed the coverage gate

`cnet_moe_try_hard` runs **ahead of** the Tier-A plan and dispatches on `goal_port.tag`
naming a unit, so it reaches certified weights without passing the gate. With defect 3 fixed
this became genuinely reachable — the gate and the memory fix belong together.

Gating it by port shape would not work: the request's goal port carries the unit *name* as
its tag, so a shape lookup misses and default-allows. `hybrid_coverage_admits_unit` keys on
the unit name instead. Gated both ways:

```
moe: in-coverage still served by the expert                     PASS
moe: out-of-coverage refused at the hard-expert door            PASS
```

### Still open, deliberately

- `PORT_RAW` ungated (exact match is meaningless for continuous values) — **backlog**.
- Near-neighbour / interval coverage — **backlog, research**.
- Corrupt sidecar still fails open (logged); fail-closed needs a separate record of which
  shapes were mined, which is what the file itself is — **must-fix later, not now**.
- Other `specialist_wrap_btn` callers pass caller-owned names; only the mine path was proven
  to pass a stack buffer. A sweep of the rest is **must-fix later**.
