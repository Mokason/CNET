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

## 2. Review round 2 — what the first pass got wrong

Independent review of `9adaffe` found merge blockers. All are fixed here, RED first.
Claims disproved by review are **removed**, not softened.

**RED**, `logs/knowledge_capsule_RED2.log` — 9 failures before any fix:

```
neg: tampered coverage row REJECTED                            FAIL
neg: tampered coverage target REJECTED                         FAIL
neg: manifest tag not matching payload REJECTED                FAIL
neg: provenance rewritten and resealed                         FAIL
neg: provenance not matching payload REJECTED                  FAIL
neg: symlinked payload REJECTED                                FAIL
neg: coverage-bearing capsule with cov=NULL REJECTED           FAIL
transactional: NO ungated unit left behind                     FAIL
checks=39 failures=9
```

**GREEN**: `KNOWLEDGE_CAPSULE_PASS checks=39 coverage_rows=5`, warning-clean.

Two of my own tests initially passed for the wrong reason and were tightened before
counting: the symlink case used a *relative* target so it merely dangled
(`payload_missing`), and the hostile-dimension case was caught by the `cov_rows`
bound while `cov_out` — the actually unbounded one — was never probed. Both now
assert the specific rejection.

| # | Blocker | Fix | Proven by |
|---|---|---|---|
| 1 | Import not transactional | Coverage is stored **first** because only it can be rolled back (`cnb_add_unit` has no removal counterpart); a failed admit calls `hybrid_coverage_forget_unit` | `coverage_restore_failed`, no unit left |
| 2 | Coverage silently dropped when `cov==NULL` | Refuse | `coverage_present_but_no_target_registry` |
| 3 | Manifest tags overwrote verified ports | Payload ports are authoritative; manifest must **agree** including tags, then is never substituted | `contract_port_mismatch` (tag rewritten **and resealed**) |
| 4 | Manifest metadata unprotected | Trailing checksum covers every preceding byte — ports, provenance, all coverage values | `manifest_integrity_mismatch` on row and target tamper |
| 5 | Provenance claim exceeded validation | Bound and verified against payload; header now states it is **not mandatory** and proves "unchanged in transit", never "from a trusted party" | `provenance_mismatch` |
| 6 | Unbounded coverage allocation | Every dimension bounded + checked multiplication before `calloc` | `coverage_bounds` on both rows/in and out_dim (**by bound, not by `oom`**) |
| 12 | Wrong FNV-1a basis | `14695981039346656037ULL` (was a digit short) | — |
| 13 | Symlink / TOCTOU / export partials | `O_NOFOLLOW` + `fstat` regular-file check; hash the exact bytes then load them from a private temp; manifest written temp+rename | `payload_missing_or_symlink` |

`src/acquire.c` carries the same FNV typo in two places. **Not changed**: those feed
stored `recipe_fp` values and correcting them would invalidate existing ledgers.

---

## 2b. Review round 3 — memory safety and true atomicity

Re-review of `66134af` found unresolved Critical/High blockers. Fixed RED first;
`logs/knowledge_capsule_RED3.log` holds 7 failures before any implementation.

**GREEN (round 3)**: `KNOWLEDGE_CAPSULE_PASS checks=52`. Round 4 takes this to **80**.

Two of the new tests again passed for the wrong reason and were tightened before
counting: overlong provenance was refused by downstream *parse fallout* (the overflow
still occurred), and `cov_out` mismatch by a COVOUT row-count accident. Both now assert
the specific reason (`provenance_field_too_long`, `coverage_out_dim_mismatch`).

| # | Blocker | Fix | Proven by |
|---|---|---|---|
| 1 | **Stack overflow**: `%127s` into `prov[CNB_NAME_MAX=64]` | read into a 256-byte scratch matching a `%255s` bound, range-check, then copy | `provenance_field_too_long` + ASAN clean |
| 2 | **OOB read**: `cap_slurp` sized exactly `st_size`, `strstr` assumed NUL | allocate checked `size+1`, terminate, keep logical length separate | ASAN/UBSAN clean on the manifest path |
| 3 | **Coverage displacement / non-transactional rollback** | coverage is keyed by PORTS, so import now asks `hybrid_coverage_owner` and **refuses a conflicting shape** rather than freeing the incumbent's rows | `coverage_port_conflict_owned_by=…`; incumbent rows/targets/tags asserted byte-identical after rejection |
| 4 | **CnetBase left partially mutated on failed admit** | fixed in the owning abstraction: blob-collision decided **before** minting, and tag mint wrapped in an exact rollback (`tag_count` + `next_mint_seq`), since mint is append-only | `atomicity: tag/blob/unit_count unchanged after refusal` |
| 5 | `cov_out` not bound to the output port | bound to the manifest's **declared** goal dims at parse time (before COVOUT rows) with checked multiply, and re-verified against the payload port | `coverage_out_dim_mismatch` |
| 6 | Predictable `manifest.cknow.tmp` via `fopen` follows symlinks | exclusive `O_CREAT\|O_EXCL\|O_NOFOLLOW` temp + rename | — |
| 7 | Regression not in a CI path | `knowledge_capsule` added to **`ci_core`**; `-Werror` on both focused targets | `CNET_CI_CORE_PASS` |

### Finding 4 — the trigger worth recording

The tag preflight compares each tag against the **base**, never against the unit's own
other tags. So a unit whose in/out tags near-miss each other (`pair_aa` / `pair_ab`,
Levenshtein 1) passed preflight, minted the first, and failed on the second — leaving a
tag behind. Deterministic, reachable, and now asserted.

### Scope honestly declined

- `src/cnet_auto_learn.c` has pre-existing format-truncation warnings that `-O1` surfaces.
  `knowledge_capsule_san` therefore does **not** use `-Werror`; `-Werror` is enforced on
  the two focused targets at the project's standard flags. Unrelated TUs were not edited
  to manufacture a clean sanitizer build.
- `src/acquire.c` still carries the short FNV basis in two places; correcting it would
  invalidate stored `recipe_fp` values.

---

## 2c. Review round 4 — final Critical/High closure

Re-review of `a496642`. RED first: `logs/knowledge_capsule_RED4.log`, 4 failures /73.
**GREEN: `KNOWLEDGE_CAPSULE_PASS checks=80`**, `-Werror`, and clean under
ASAN + UBSAN + **LeakSanitizer**.

| # | Blocker | Fix | Proven by |
|---|---|---|---|
| 1 | Same-owner rollback destroyed the old gate | target-side preflight before any mutation: same name + same digest + byte-identical coverage = idempotent no-op; anything else refused | `target_unit_conflict_same_name_different_content`; old rows/targets and base counts asserted byte-identical |
| 2 | Failed imports stranded registry slots | `hybrid_coverage_forget_unit` now moves the last record into the hole and decrements `coverage_count` | `reclaim: count decreased / array compacted / all remaining preserved`; `HYBRID_COVERAGE_MAX+8` rejected imports leave the count flat and the registry usable |
| 3 | `cnb_add_unit_bytes` not allocation-failure atomic | reserve tags/blobs/units **before** any mutation via `cnb_reserve`, then a commit that cannot allocate; test-only `cnb_test_alloc_fail_in(n)` injects failure at each reservation | 3/3 injected failures refused with `tag/blob/unit_count` unchanged, then a normal admit still succeeds |
| 4 | Payload temp symlink clobber | fixed in `cnb_save` itself (benefits every caller): `O_CREAT\|O_EXCL\|O_NOFOLLOW` + rename | planted symlink at `unit.cnb.tmp`; victim file byte-identical after export |
| 5 | `cov_out==0` contract undefined | **permitted and documented**: `HybridCoverage` treats labels as optional (sidecar restores carry inputs only), so 0 means "rows, no labels"; nonzero must equal the checked output-port dim | `cov0` export+import round-trip; `coverage_out_dim_mismatch` for nonzero mismatch |
| 6 | Test leaked its own BTNs | every BTN `btn_free`+`free` on all paths | LeakSanitizer clean (was 49,960 bytes in 33 allocations) |
| 7 | Bench unprotected | `knowledge_accumulation_bench` (~1 s) added to `ci_core` alongside `knowledge_capsule` | `CNET_CI_CORE_PASS` runs both |
| 9 | Sanitizer target unguessable | renamed **`make knowledge_capsule_sanitize`** (`knowledge_capsule_san` kept as an alias) | — |

### A bug the sanitizer caught in my own fix

Reserving capacity up front let me replace `CNB_PUSH` with direct writes in the commit
path — but `CNB_PUSH` also **zeroed the new slot**, and dropping that left
`units[].provenance` uninitialised, so `cnb_save`'s `strlen` ran off the end. ASAN
reported a heap-buffer-overflow; the slots are now explicitly zeroed. This is the
argument for the sanitizer target existing at all.

---

## 3. TDD evidence (round 1)


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

## 4. The capsule

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
| explicit rejection | 20+ named `reject_reason` values. Transaction semantics, exactly: all validation completes before any mutation; then coverage is written first (the only rollbackable half) and the base admit follows, with coverage forgotten if the admit fails. A conflicting coverage port shape is refused outright rather than displaced. |

Integrity here is a **corruption check, not a signature** — no PKI is present and
claiming authenticity would be dishonest.

---

## 5. Benchmark (corrected)

`make knowledge_accumulation_bench` → `logs/knowledge_accumulation_bench.json` (schema 2)

```
KNOWLEDGE_ACCUMULATION_BENCH_PASS units=32 distinct=1 isolation=32/32 replay=8/8
  cov_exact=8 pre_unservable=8/8 ood_refused=96
```

| Measure | Floor | Result |
|---|---|---|
| **Distinctness** — 32 unit functions differ pairwise | all differ | **1 (asserted)** |
| Isolation — each unit individually correct | == 32 | **32/32** |
| Interference — unit 0 answers at N=1 vs after 31 adds | drift == 0 | **0/8** |
| **Contract replay** after transfer | 100% | **8/8** |
| **Coverage round-trip** — rows+targets+tags vs source | bit-identical | **8/8** |
| **Pre-import unservable** (same queries, fresh target) | == total | **8/8** |
| Post-import served | == units | **8/8** |
| Corrupted capsule rejected | == 1 | **1** |
| Truncated payload rejected | == 1 | **1** |
| OOD — uncovered refused | == 96 | **96** (160 admitted) |
| Composition | — | **WITHHELD** |
| Semantic intent / broad intelligence | — | **WITHHELD** |

| N | units | build_ms | lookup_ms (materialise) | serve_ms/query |
|---|---|---|---|---|
| 1 | 1 | 23.1 | 0.0051 | 0.00006 |
| 8 | 8 | 179.2 | 0.0050 | 0.00006 |
| 32 | 32 | 716.9 | 0.0050 | 0.00006 |

Capsule payload **2583 bytes**.

### What review corrected in the numbers

- **"32 disjoint units" was false.** `(x+k+1)%8` repeats every 8, so it was 8 functions
  with 4 copies each. Units are now distinct permutations in factorial order, and
  pairwise distinctness is **asserted before training**.
- **"Held-out recall" was false.** All 8 probes ARE the certified exemplars. Renamed
  everywhere to **exhaustive contract replay = serialization fidelity**, with that
  caveat carried inside the JSON itself.
- **The 0.0 baseline was tautological** (an empty base scoring "accuracy"). Replaced
  with pre-import unservability of the *same* queries on the *same* target: 8/8
  unservable before, 8/8 served after.
- **Coverage round-trip probed `P("x")/P("y")`**, which could never match. It now
  compares the actual imported record's rows, targets and port tags against source.
- **Lookup timing measured `cnb_has_unit`** (a name scan). Now real `cnb_get_unit`
  materialisation incl. CNU1 verify, mean of 200 reps — which moved it from 0.0001 to
  0.0050 ms. **The "flat scaling" claim is withdrawn**: this is one run with no
  variance estimate, so the table is reported as measured and nothing is inferred
  about asymptotics.
- `system()` calls replaced with C helpers; both new gates compile warning-clean.

---

## 5b. Finding: tag governance blocks numbered skill families

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
| `include/cnet_capsule.h`, `src/cnet_capsule.c` | capsule export/import; transactional commit, manifest checksum, port+provenance binding, bounded coverage, `O_NOFOLLOW`/regular-file, verified-bytes temp load, temp+rename manifest, standard FNV basis |
| `tests/test_knowledge_capsule.c` | 39 checks incl. 9 RED-first negative controls; C helpers, no `system()` |
| `tests/knowledge_accumulation_bench.c` | distinct permutations + distinctness assert, coverage-vs-source comparison, pre-import unservability baseline, real materialisation timing, replay relabelled, no `system()` |
| `include/base.h`, `src/base.c` | `cnb_format_version()` accessor |
| `src/soul_host.c` | removed stale locals left by the earlier S8 seal refactor (warning-clean) |
| `Makefile` | `knowledge_capsule` into `cognitive_runtime`; `asi_framing` in `claims` |
| `README.md`, `AGENTS.md`, `docs/INDEX.md`, `docs/ARCHITECTURE.md` | canonical ASI framing |

Untouched: `config/personal-ai.env`, `scripts/cnet_offhours_watchdog.sh`.
Production `soul_gemma4v2_final.cnb` is never opened by these gates (its mtime moves
because the running lane service checkpoints it).

---

## 7. Verification run

```
KNOWLEDGE_CAPSULE_PASS checks=39 coverage_rows=5
KNOWLEDGE_ACCUMULATION_BENCH_PASS units=32 distinct=1 isolation=32/32 replay=8/8
  cov_exact=8 pre_unservable=8/8 ood_refused=96
ASI_FRAMING_PASS files=4
COVERAGE_ABSTAIN_PASS checks=55 heldout_correct=4/4 was=0/4
OWN_LEARNING_LOOP_PASS checks=31
HYBRID_AI_PASS checks=21
POST_SEAL_SERVE_PASS checks=16
COLIBRI_INTEGRATE_PASS checks=26
STRUCTURE_MINE_SERVE_DURABLE_PASS checks=13 rows=2
SUBSTITUTION_BENCH_PASS ... heldout_correct=4/4 coverage_abstains=4
CAPABILITY_CERT_PASS certified=6/6
COGNITIVE_RUNTIME_PASS capabilities=6 classification=measured   (now includes knowledge_capsule)
```

`make claims` does **not** pass on this tree: `soul_reopen_test` fails
`SoulHost fails closed on malformed adjacent runtime state`, **pre-existing**, verified
identical on the stashed baseline. Not in scope here, and not masked.

JSON artifact: `logs/knowledge_accumulation_bench.json` (schema 2).

---

## 8. Trust boundary (explicit)

A capsule is a **local transfer object**. Its checksums are **unkeyed**: they detect
accident — truncation, bit-rot, a partial write, a mismatched build — and nothing more.
Anyone who can rewrite a capsule can recompute them. This is **not authenticity**, and
signing/PKI is deliberately out of scope. Provenance is *bound and verified*, but an
empty provenance is accepted, so it proves "unchanged in transit", never "came from a
trusted party". The owner's portable-company use case will eventually need real
provenance identity; that is named backlog, not something claimed here.

Hardened within that scope: `O_NOFOLLOW` + regular-file check (symlinked/special
payloads refused), hash-the-bytes-then-load-those-bytes via a private temp (no
hash-then-reopen window), and exclusive O_NOFOLLOW temp + rename for BOTH the
payload and the manifest. A half-written package is rejected, not prevented — see
§9 for the exact guarantees.

## 9. Umbrella placement and exact guarantees

`knowledge_capsule` (80 checks, `-Werror`) and `knowledge_accumulation_bench` both run in
**`ci_core`** — verified by `CNET_CI_CORE_PASS`. `knowledge_capsule` also runs in
`cognitive_runtime`. `asi_framing` is in `claims`.

Sanitizer: **`make knowledge_capsule_sanitize`** (alias `knowledge_capsule_san`) — ASAN +
UBSAN + LeakSanitizer over the same 80 checks. It deliberately omits `-Werror`: `-O1`
surfaces pre-existing format-truncation warnings in `src/cnet_auto_learn.c`, and unrelated
TUs were not edited to manufacture a clean build. `-Werror` IS enforced on both focused
targets at the project's standard flags.

`make claims` still does not pass: `soul_reopen_test` fails `SoulHost fails closed on
malformed adjacent runtime state`, **pre-existing**, verified identical on the stashed
baseline. Not masked, not in scope.

**Package guarantees, stated exactly.** There is **no atomic directory publication**: the
payload and manifest publish separately, so a capsule directory can be observed
half-written. What is guaranteed is that such a package is **rejected** — both files, a
manifest checksum over every security-relevant field, and a payload checksum are all
required. Per file, each is written to an exclusive `O_CREAT|O_EXCL|O_NOFOLLOW` temp and
renamed, so no reader sees a partial file and a planted symlink at either temp path cannot
be followed. Every earlier "no partial capsule" phrasing is removed.

## 10. Does the evidence support the thesis?

**Specialized portable knowledge: yes, mechanism-proven at small scale.** 32
*genuinely distinct* certified units coexist with zero interference drift; any one
transfers to a fresh runtime with byte-identical contract replay **and** bit-identical
coverage; the same queries are unservable before import and served after; corrupt,
truncated, symlinked, over-dimensioned, tag-mismatched and provenance-mismatched
capsules are all refused with named reasons and leave nothing behind.

**Anything broader: no, and the gate says so.** Composition, semantic intent and broad
capability are WITHHELD. The units are 8-symbol permutations — this proves the
*container and the accounting*, not that CNET knows anything hard. Contract replay is
serialization fidelity, **not** generalisation; the honest reading is "what was
certified survives transfer", nothing more.

---

## 11. Next highest-leverage milestone

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
