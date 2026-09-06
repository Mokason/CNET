# CNET distill — split a monobrain into recallable specialists

Date: 2026-08-20  
Status: **architecture + first C slice** — not a second Unlimited.

## Opinion (short)

Yes. Distillation inside CNET should **not** mean “train a smaller parrot of Bonsai.”  
It should mean: **carve competence out of a residual teacher into CERT objects** you can
recall (capsule / brick / ROE skill) and **leave the rest as OPEN_CHAT**.

That is the only distill that matches ASI = Artificial Specialized Intelligence.

```text
Monolithic residual (Bonsai / held / cloud)
        │  teacher only
        ▼
   domain slices (tables, traces, skills)
        │  propose
        ▼
   verify (gold / multi_stable / factory / coverage)
        │  admit
        ▼
   capsule / .lut brick / pack skill   ← recall in cnetd CERT-first
```

## What we already have (do not invent a second stack)

| Primitive | Role | Distill use |
|-----------|------|-------------|
| Residual teacher (held / Bonsai :8081) | propose drafts | teacher mouth |
| `library_evolve` / sleep compress | plan → certified chunk | **in-process** distill of *traces* |
| CORE `.lut` factory | table → brick | numeric/logic slices |
| ROE gold + `roe_evolve_tick` | miss → pack skill | language slices |
| `cnet_capsule_export` | unit + coverage portable | **recall package** |
| `cnet_capsule_propose` | pending inbox, `auto_cert=false` | propose-only door |
| grow_lobe / QAT student | tiny student, anti-collapse | optional neural student — still `claimed_cert=0` |
| MTK `.tskill` | weight delta on a host model | **not** a capsule; residual cartridge only |

**Law:** MTK ≠ capsule. Teacher ≠ runtime. Propose ≠ admit.

## CNET distill (the product definition)

A **distill slice** is:

1. **Domain** — named specialty (`arith_u16`, `cert_law`, `english_tense`, `ocr_table`…).
2. **Teacher traces** — residual answers on a query set (or organic miss_log cluster).
3. **Student object** (one of, in this order of honesty):
   - **Table / brick** if the domain is given→value (CORE).
   - **ROE skill + gold** if the domain is sealed language.
   - **Capsule** if a certified unit + coverage can travel.
   - **Tiny grow student** only as residual helper (`claimed_cert=0`).
4. **Gate** — coverage abstain + never-self-CERT + anti-collapse (no Tier-A self-train).
5. **Recall** — cnetd CERT-first; residual only on miss.

If a slice cannot be tabled, it **stays residual**. That is success (honest abstain), not failure.

## What we will *not* do

| Temptation | Why not |
|------------|---------|
| Layer-split GGUF into “capsules” | No contract, no coverage, fake specialists |
| Distill whole 27B → 8B as “our brain” | Second monobrain; fights product law |
| Auto-CERT teacher text | Self-CERT / collapse |
| MoE expert dump as CERT | Weights without gates |
| Train student on CNET LOCAL answers | Anti-collapse |

## Pipeline (factory)

```text
A. Harvest   miss_log + curriculum + domain query list
B. Teacher   residual draft per query (STAGE / held)  claimed_cert=0
C. Shape     gold file | typed table | library trace
D. Propose   capsule_inbox/<domain>-<ts>/PROPOSE.json
E. Verify    gold | multi_stable+reviewer | factory table ≥ bar
F. Admit     pack_personal / .lut / capsule import
G. Recall    cnetd CERT hit; leftover still residual
```

Compression metric (advisory, from `library.h`):  
`compression_ratio = teacher_mac / student_mac` — never an admit decision.

## Recall in the live system

```text
user / Discord / Hermes PEER
   → cnetd CERT packs + CORE bricks + capsules
   → miss → CORE OPEN_CHAT / STAGE (teacher)
   → miss_log → this distill pipeline (unattended later)
```

Hermes talks to **Marble on PEER**, not MCP. Distilled parts show up as **LOCAL skills/bricks**, not as “the 27B speaking.”

## First slice (this session)

`bin/cnet_distill_slice` — propose-only:

```bash
cnet_distill_slice --domain demo_arith --dry-run
# → var/capsule_inbox/demo_arith-<ts>/PROPOSE.json + rows.jsonl
# DISTILL_SLICE_PASS   auto_cert=0
```

Live teacher optional: `--teacher` uses STAGE HTTP; still no seal.

## Success markers

| Marker | Meaning |
|--------|---------|
| `DISTILL_SLICE_PASS` | propose path works, no seal |
| later `DISTILL_ADMIT_PASS` | one domain verified + LOCAL recall |
| **WITHHELD** | “we distilled the model” as AGI/SOTA claim |

## Related

- `include/library.h`, `include/cnet_capsule.h`
- `docs/MARBLE_LIVE.md`, `docs/THIRD_WAY_MARBLE_PEER.md`
- `plans/cnet_grow_lobe.md`
- Skills: `cnet-specialist-spine`, `cnet-residual-teachers`, `cnet-rlm-ember-self-improve`

---

## Implementation notes — first slice hardened (writer lane, 2026-08-20)

Doctrine above is Hermes's and unchanged. This section records what the C
actually enforces, so the law and the code do not drift apart.

### Anti-collapse is a CODE gate, not a JSON field

The row schema always carried `"anti_collapse": true`. Nothing checked it. A
claim in the output of the tool that produced the output is not a gate.

`cnet_distill_slice` now probes every query against the live front door with the
residual disabled (`ROE_LIVE=0 ROE_LLM=0`) before it will accept a teacher
draft. Anything CNET already answers **LOCAL (Tier-A)** is dropped with
`skip_reason: anti_collapse_local_tier_a` and counted in
`skipped_anti_collapse`. Labelling a student with our own certified output is
the collapse loop the doctrine forbids, so it has to be refused in code.

```
$ cnet_distill_slice --domain soul_probe \
    --query "who are you" --query "are you a second brain" --query "what is a quokka"
rows=3 gold=1 skipped_collapse=2 collapse_checked=1
  who are you            -> skipped  anti_collapse_local_tier_a
  are you a second brain -> skipped  anti_collapse_local_tier_a
  what is a quokka       -> distill candidate
```

It **fails open** when the front door is unreachable — an offline box would
otherwise silently drop every row — and records `anti_collapse_checked` in
`PROPOSE.json` so a reviewer can tell *checked* from *could not check*. That
distinction is the whole value of the field. `--no-collapse-check` is for
offline tests only.

### Untrusted input reaches a shell

`--from-miss-log` means query text is arbitrary user input. The teacher call
JSON-escapes the query and then shell-quotes the whole payload; before this a
query containing a quote broke out of both the JSON string and the shell word.
Covered by `--selftest`.

### Inputs

| Flag | Source |
|---|---|
| `--query Q` (repeatable) | hand-written curriculum |
| `--from-file queries.txt` | curated list; `#` comments skipped |
| `--from-miss-log miss.jsonl` | **organic demand** — distinct `query` values |

A curriculum someone typed is a guess; a miss cluster is evidence the coverage
gap is real. Against the live 18k-row miss log this yields 32 distinct queries.

### Outputs

```text
var/capsule_inbox/<domain>-<ts>/
  PROPOSE.json     kind, domain, n_rows, gold_rows, skipped_anti_collapse,
                   anti_collapse_checked, auto_cert:false, status:pending_verify
  rows.jsonl       one row per query; skipped rows keep their skip_reason
  gold_rows.jsonl  {query, answer, auto_cert:false, status:pending_verify}
```

`gold_rows.jsonl` is deliberately in the shape the **existing** ROE gold path
already reads, so a verified slice is promoted with `bin/roe_gold_put` and
`roe_evolve_tick` rather than a new importer. They are candidates for gold, not
gold.

Suggested admit route once a slice is reviewed:

```bash
# per verified row -- gold, then the normal evolve tick promotes it
./bin/roe_gold_put "<query>" "<verified answer>"
systemctl --user start roe-evolve-tick.service
```

### Gate

`make distill_slice` → `DISTILL_SLICE_PASS`. Builds `-Werror`, runs the 8-check
selftest, runs Hermes's demo, then asserts the law: `auto_cert=0` in the log, no
`auto_cert: true` / `claimed_cert: 1` anywhere in the written inbox, and that a
Tier-A query is actually dropped by the collapse gate.

### Not done

- **No student is built.** The slice stops at *propose*. Table/brick/capsule
  minting is `DISTILL_ADMIT_PASS`, and admitting still needs
  gold / multi_stable+reviewer / factory — unchanged.
- `--teacher` posts to STAGE HTTP one query at a time; no batching, no retry.
- Domain shaping (table vs skill vs capsule, section "Student object") is still
  a human call. The tool does not guess which of the three a domain wants.
