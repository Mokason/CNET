# Portable isolated knowledge + accumulation benchmark (2026-07-27)

**Repo:** `/home/marble/AI/CNET` — baseline `fc4c512`
**Decision record.** This repo has no ADR tree; `plans/*.md` is the established
decision-doc convention, so this file is both the report and the record of why.

---

## 0. Canonical framing (now enforced by a gate)

> **CNET builds ASI — Artificial Specialized Intelligence. In CNET, ASI never
> means Artificial Superintelligence. CNET is not claiming AGI. Broad semantic
> grounding exists to understand intent; deep competence comes from isolated,
> certified, portable Micro Tensor Kernels. Wikipedia-like accumulation may
> broaden coverage and composition over time, but unit count alone is not
> intelligence and all broader claims remain benchmark-gated.**

```
base semantic grounding → typed intent → CNET isolated knowledge registry
      → certified kernels → verifier / abstention → residual teacher (uncovered)
```

Placed in `README.md`, `AGENTS.md`, `docs/INDEX.md`, `docs/ARCHITECTURE.md`.
`make asi_framing` greps all four for the canonical sentence, the
"never … Artificial Superintelligence" disclaimer and "not claiming AGI", and is
wired into the existing `claims` gate. It is a grep, not a snapshot, so it
survives reflow but still fails on drift — verified by temporarily corrupting
README:

```
ASI_FRAMING_FAIL missing AGI disclaimer in README.md   (restored, then PASS)
ASI_FRAMING_PASS files=4
```

**Why ASI is spelled out everywhere:** the acronym is overwhelmingly read as
"superintelligence". Every use in code comments, docs and this report expands it
on first mention and disclaims the other reading, because an unexpanded "ASI"
in a repo is a future misquote waiting to happen.

---

## 1. Architecture actually found: two different things

The machine has both. They are **not** interchangeable and the task's warning
was justified.

| | **MTK cartridge** (`.tskill` / CMSK / MTSK) | **Certified unit / capsule** |
|---|---|---|
| Source of truth | `include/cce/cce_mtk.h`, `src/cce/cce_mtk.c` | `include/base.h`, `include/cnet_capsule.h` |
| What it is | f32-sparse / ternary **weight deltas** applied to a host model's named tensor sites (`cce_mtk_site`), revertible via `base_snap` | a **CNU1-sealed BTN** plus its `Contract` exemplars, typed Ports, provenance |
| Typed I/O contract | none | yes (`Port` in/goal, exemplar table) |
| Certification | none | behaviour digest + contract verify on materialise |
| Coverage / abstention | none | yes — and it now travels with the artifact |
| Portability | only onto a matching host model's tensor names | any compatible CNB runtime, with explicit refusal otherwise |

MTK is a **knowledge-swap mechanism for a transformer's weights**. The certified
unit is **CNET's isolated knowledge object**. The owner's "Micro Tensor Kernel"
thesis — isolate, store, recall, transfer — maps onto the *certified unit*,
because only that side has identity, contract, certification and coverage. No
duplicate package system was created.

### What already existed

`cnb_export_subset(src, dst, keep, ctx)` (`src/base.c:391`) already extracts one
unit with its sealed contract, oracle descriptors, provenance and stats.
`cnb_save`/`cnb_load` persist it. So ~80% of transfer was present.

### The real gap (load-bearing)

**Coverage lives in a `<base>.coverage` sidecar, not in the CNB.** So
`cnb_export_subset` moved a mined unit's *weights and contract* but silently
dropped its **certified domain**. On the target the unit had no coverage record,
`hybrid_coverage_admits` default-allowed, and it would answer outside what it was
certified for — the exact confident-wrong failure the coverage work exists to
prevent. Transfer was un-gating capability.

---

## 2. TDD evidence

**RED** — test written first, `logs/knowledge_capsule_RED.log`:

```
make: *** No rule to make target 'src/cnet_capsule.c', needed by 'knowledge_capsule'.  Stop.
```

**GREEN** — after implementing `include/cnet_capsule.h` + `src/cnet_capsule.c`:

```
KNOWLEDGE_CAPSULE_PASS checks=19 coverage_rows=5
  export: coverage travelled with it                             PASS
  import: gate restored, not silently dropped                    PASS
  round-trip: all 8 answers match the source                     PASS
  OOD: only certified symbols admitted                           PASS   (5 of 8)
  OOD: uncovered symbols refused                                 PASS   (3 of 8)
  corruption: tampered payload REJECTED   reject_reason=payload_integrity_mismatch
  incompatible: foreign cnb_version REJECTED
                reject_reason=incompatible_cnb_version=99_expected=5
```

A second RED landed during the benchmark and is kept because it is a real
finding — see §5.

---

## 3. The capsule

`dir/{unit.cnb, manifest.cknow}`. Binds, and refuses to import without:

| Requirement | How |
|---|---|
| stable identity + version | unit name + contract behaviour digest, re-checked after materialise |
| typed I/O contract | Ports + exemplar count, compared against the payload |
| payload | single-unit CNB, per-blob CNU1 seal verified on `cnb_get_unit` |
| runtime compatibility | `cnb_format_version()` (new accessor) pinned in the manifest |
| certification evidence | behaviour digest mismatch ⇒ refuse |
| coverage / abstention | rows **and** targets serialised at `%.17g`, restored via `hybrid_coverage_record` |
| provenance | carried by `cnb_export_subset` |
| integrity | FNV-1a 64 over the payload, checked before parsing it |
| explicit rejection | 16 named `reject_reason` values; **nothing** is written to the target before every check passes |

Integrity here is a **corruption check, not a signature** — no PKI is present and
claiming authenticity would be dishonest.

---

## 4. Benchmark

`make knowledge_accumulation_bench` → `logs/knowledge_accumulation_bench.json`

```
KNOWLEDGE_ACCUMULATION_BENCH_PASS units=32 isolation=32/32 roundtrip=8/8 ood_refused=96
```

| Measure | Floor | Result |
|---|---|---|
| Isolation — each of 32 disjoint units individually correct | == 32 | **32/32** |
| Interference — unit 0 answer vector at N=1 vs after 31 later adds | drift == 0 | **0/8 drift** |
| Portable round-trip — export → fresh base → identical behaviour | 100% | **8/8** |
| Corruption rejected | == 1 | **1** (`payload_integrity_mismatch`) |
| Incompatible version rejected | == 1 | **1** (`incompatible_cnb_version=99_expected=5`) |
| OOD abstention — uncovered inputs refused | == 96 | **96** refused / **160** admitted |
| Baseline (negative control) — no import | == 0.0 | **0.0** |
| Composition | — | **WITHHELD** |
| Semantic intent understanding | — | **WITHHELD** (no semantic path exercised) |
| Broad intelligence | — | **WITHHELD** |

Scaling (CPU only, fresh temp base):

| N | units | build_ms | lookup_ms/unit | serve_ms/query |
|---|---|---|---|---|
| 1 | 1 | 21.5 | 0.0001 | 0.0002 |
| 8 | 8 | 174.2 | 0.0000 | 0.0002 |
| 32 | 32 | 698.1 | 0.0001 | 0.0003 |

Capsule payload: **2583 bytes** per unit. Build cost is linear in N (training
dominates); lookup and serve are flat — accumulation does not slow recall at
this scale.

**What the green means, precisely.** Exact mechanism proof: isolation,
interference, round-trip, refusal, abstention. Held-out specialized recall: each
unit reproduces its own rotation on all 8 symbols after transfer. It does **not**
show reasoning, composition, generalisation beyond the certified domain, or
intent understanding. The baseline is the honest comparison: without the capsule
the target base cannot answer at all (0.0), so 8/8 measures transfer, not a
model that already knew the answer.

**Composition is withheld, not failed.** `route_plan` was asked for a 2-step plan
across two independently stored units with disjoint tags and did not produce
one. That is arguably *correct* — bridging unrelated tags would be a typing
violation — but since nothing verified a genuine composition, the JSON says
`withheld_planner_does_not_chain_disjoint_tags` rather than reporting a score.

---

## 5. Finding: tag governance blocks numbered skill families

The benchmark's first run died at `build FAILED at k=1`. Cause: `cnb_add_unit`
mints port tags, and `cnb_tag_mint` refuses **near-misses** — case-insensitive
equality, underscore-stripped equality, or **Levenshtein distance 1**. So
`kb_in_0` / `kb_in_1` is refused, and a sequentially-numbered skill family
**cannot be minted at all**.

This is the same rule that parked **7 production gaps** as `tag_collision`
(`grow_tok_1..7`) in yesterday's plateau investigation — the mechanism is now
identified. The benchmark works around it with tags that differ in ≥2 positions
and documents the rule at the call site.

Not "fixed": the refusal is a deliberate anti-confusion guard, and relaxing it is
a policy decision with real downside. But **any skill family generated by
incrementing a counter will silently fail to accumulate**, which is a sharp edge
for exactly the Wikipedia-style breadth this thesis depends on.

---

## 6. Files changed

| File | Change |
|---|---|
| `include/cnet_capsule.h`, `src/cnet_capsule.c` | new — capsule export/import, fail-closed |
| `tests/test_knowledge_capsule.c` | new — RED-first capsule gate |
| `tests/knowledge_accumulation_bench.c` | new — accumulation benchmark + JSON |
| `include/base.h`, `src/base.c` | `cnb_format_version()` accessor for compatibility pinning |
| `Makefile` | `knowledge_capsule`, `knowledge_accumulation_bench`, `asi_framing`; `claims` depends on `asi_framing` |
| `README.md`, `AGENTS.md`, `docs/INDEX.md`, `docs/ARCHITECTURE.md` | canonical ASI framing |

Not touched: `config/personal-ai.env`, `scripts/cnet_offhours_watchdog.sh`.
Production `soul_gemma4v2_final.cnb` was never opened by any gate here — its
mtime moves because the running lane service checkpoints it.

---

## 7. Verification run

```
KNOWLEDGE_CAPSULE_PASS checks=19 coverage_rows=5
KNOWLEDGE_ACCUMULATION_BENCH_PASS units=32 isolation=32/32 roundtrip=8/8 ood_refused=96
ASI_FRAMING_PASS files=4
COVERAGE_ABSTAIN_PASS checks=55 heldout_correct=4/4 was=0/4
OWN_LEARNING_LOOP_PASS checks=31
HYBRID_AI_PASS checks=21
POST_SEAL_SERVE_PASS checks=16
COLIBRI_INTEGRATE_PASS checks=26
STRUCTURE_MINE_SERVE_DURABLE_PASS checks=13 rows=2
SUBSTITUTION_BENCH_PASS ... heldout_correct=4/4 coverage_abstains=4
CAPABILITY_CERT_PASS certified=6/6
COGNITIVE_RUNTIME_PASS capabilities=6 classification=measured
```

---

## 8. Does the evidence support the thesis?

**Specialized portable knowledge: yes, mechanism-proven at small scale.** 32
disjoint certified units coexist without interference, any one transfers to a
fresh runtime with byte-identical behaviour and its abstention gate intact, and
corrupt or incompatible artifacts are refused with a named reason.

**Anything broader: no, and the gate says so.** Composition, semantic intent and
broad capability are WITHHELD. The units are 8-symbol rotations — this proves
the *container and the accounting*, not that CNET knows anything hard. Nothing
here supports a general-capability claim, and the framing gate now makes it
awkward for a future agent to imply one.

---

## 9. Next highest-leverage milestone

**Make composition real, or prove it cannot be.** Accumulation without
composition is a filing cabinet: N units answer N questions and never the
N+1'th. The capsule now makes units portable, so the next question is whether
two independently certified kernels can be *chained* on a held-out task under
the existing planner, with coverage enforced at each hop. That is the first
milestone where "accumulation may broaden capability over time" stops being a
hypothesis. It should be attempted with deliberately composable ports (A: X→Y,
B: Y→Z) rather than the disjoint tags used here.

Secondary: decide the `tag_collision` policy (§5), since it currently caps how
fast any numbered skill family can grow.
