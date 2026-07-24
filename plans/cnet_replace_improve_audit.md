# CNET Replace/Improve Audit — Implementation Plan

> **For Hermes:** Execute with parallel worktrees + coding delegates. Parent owns integration.

**Goal:** One fault economy + certify-before-serve PEFT + product gates across Ghost/MCP/native.

**Post-audit commits already done (do not redo):**
- LoRA → Lily → serve-loop → registry cert gate (`4849076`…`af05a81`)
- Compute-quality teacher characterization (`f79ecd9`) — Lily envelope = smooth gaps; DSA sparse compute REJECTED by gate (correct)

**Architecture:** Shared JSONL fault bus → cheap adapter tick (LoRA/Lily/DoRA) under existing cert policy → dense gap_lane fallback → ghost-eval/task promote → janitor v2 prune.

**Tech:** C (native CNET), optional thin .NET Ghost glue, make gates.

---

## Tier 0 — glue

### T0.1 ✅ Unified fault record
- Create: `include/cnet_fault.h`, `src/cnet_fault.c`, `tests/cnet_fault_test.c`
- JSONL line schema: `ts,source,unit,skill,session,in_dim,out_dim,label_kind,note` + optional base64-free numeric rows file companion
- API: `cnet_fault_open/append/close/count/load_for_unit`
- Gate: `make cnet_fault_test` → `CNET_FAULT_PASS`

### T0.2 ✅ Auto-learn discipline
- Modify: `src/cnet_auto_learn.c`, `include/cnet_auto_learn.h`
- `CNET_AUTO_LEARN_FREEFORM=0` → freeform becomes blob-only note (no tk*q* seal path) unless skill= set
- Keep structured `skill_/research_/chunk_` path
- Gate: unit test in `cnet_fault_test` or `auto_learn_policy_test`

### T0.3 ✅ Janitor v2
- Modify: `tools/cnet_janitor.c`, `scripts/cnet_janitor.sh`
- Flag/report: `gh_` prefix skills, rel≈0.5 with evidence=0, incomplete oracles TTL
- Optional `--prune-report-only` default; `--prune-apply` opt-in
- Gate: `make janitor` still passes; new section in md/json

### T0.4 ✅ Promote gate stub
- Create: `include/cnet_promote.h`, `src/cnet_promote.c`, `tests/cnet_promote_test.c`
- Simple rule: require `fixes - regressions >= min` AND optional external eval_delta file
- Gate: `make cnet_promote_test` → `CNET_PROMOTE_PASS`

## Tier 1 — PEFT productization

### T1.1 ✅ Multi-adapter bank (hard route)
- Create: `include/cce/cce_adapter_bank.h`, `src/cce/cce_adapter_bank.c`, test
- N named slots; `select(name)` hard route; one active serve
- Gate: `make cce_adapter_bank_test`

### T1.2 ✅ Persist adapters sidecar
- Create: `src/router/registry_lora_store.c` + header
- Save/load certified lora next to base path `*.lora/<unit>.bin`
- Gate: round-trip in registry_lora_test extension or `make registry_lora_store_test`

### T1.3 ✅ DoRA-lite experiment
- Create: `include/cce/cce_dora.h`, `src/cce/cce_dora.c`, test
- Magnitude vector + direction low-rank; same residual train API shape as lora
- Host under cert policy reuse where cheap
- Gate: `make cce_dora_test`

### T1.4 ✅ VeRA-lite (optional shared random A)
- Flag on lily/lora: freeze A random, train B (+scales)
- Gate: bench note in docs; test no-op when untrained

### T1.5 ✅ GGUF residual hook stub
- Document + weak symbol / NULL hook on gguf residual path matching DS pattern
- Gate: compile + byte-identical when NULL (existing gguf tests)

## Tier 2 — composition

### T2.1 ✅ Procedure chunk sealer
- Create: `scripts/procedure_chunk_seal.sh` + `artifacts/janitor/procedure_seeds.md`
- Emits structured inbox notes for multi-step skills (not tk*)

### T2.2 ✅ DAG module boundary
- Create: `include/router/dag_api.h` extracting public execute/plan decls used externally
- No behavior change; `make route_demo dag_demo` still pass

### T2.3 ✅ Serve decode helper
- Create: `include/cnet_serve_decode.h`, `src/cnet_serve_decode.c`
- Map top-k pick indices + alphabet → human string for MCP/Ghost
- Gate: `make cnet_serve_decode_test`

## Tier 3 + cleanup A–F

### T3.1 ✅ Remove `registry_teach_lora_OLD`
### T3.2 specialist_adapters façade notes + thin wrappers to registry_lora when linked
### T3.3 ✅ Quarantine `dotnet/Llm` — README DEPRECATION if dual stack
### T3.4 ✅ AICIMO MCP honesty: document fallback in hermes_hosting
### T3.5 docs/INDEX + cce_lily envelope from f79ecd9 already
### T3.6 ✅ Umbrella: `make cnet_replace_improve` runs all new gates

## Worktree ownership

| Lane | Worktree | Owns |
|------|----------|------|
| A | wt-t0-fault | fault, auto_learn, promote, Makefile slice |
| B | wt-t1-peft | adapter_bank, dora, lora_store, lily vera flag |
| C | wt-t2-product | janitor v2, serve_decode, procedure seeds, dag_api, docs |
| integ | wt-integ | cherry-pick order A→B→C, umbrella gate |

## Verify umbrella

```bash
make cnet_replace_improve
# expects CNET_REPLACE_IMPROVE_PASS
```


## Status 2026-07-24
`make cnet_replace_improve` → **CNET_REPLACE_IMPROVE_PASS** on master.


## Fault loop closed (2026-07-24)

`make cnet_fault_loop_test` → **CNET_FAULT_LOOP_PASS**

- `registry_supply_label` → weak `cnet_fault_mirror_labeled` → `CNET_FAULT_LOG`
- `registry_lora_ingest_fault_bus` + tick auto-ingest when log set
- Host `FaultBus` + JTC ClassifyOrGap breadcrumbs
- Env: `CNET_FAULT_LOG`, `CNET_FAULT_MIRROR=0` to disable mirror, `CNET_PROMOTE=1` optional

## Post-merge audit recheck + gaps closed (2026-07-24, `2624bec`)

Independent recheck of the Tier 0–3 commits (`87abb26`…`decc861`): full `make test`
green (25-suite log gate) **and** all 7 new gates pass (`CNET_FAULT_PASS`,
`CNET_PROMOTE_PASS`, `CCE_ADAPTER_BANK_PASS`, `CCE_DORA_PASS`,
`CNET_SERVE_DECODE_PASS`, `CNET_FAULT_LOOP_PASS`, `CNET_LORA_STORE_PASS`,
`JTC_ADAPTER_BENCH_PASS` — `acc_off=0.2975 → acc_on=0.7375`, Δ+0.44 reproduced on
held-out). No core regression; claims (DoRA-lite, VeRA-lite freeze-A, +0.44 bench)
verified honest; `registry_teach_lora_OLD` cleanly removed.

Two gaps found and **closed** (`2624bec`):

- ✅ **New gates were not in CI.** The 7 new tests lived only under the on-demand
  `cnet_replace_improve` umbrella, so `make test` did not guard them. Wired the 4
  light/deterministic ones into `verify` as prerequisites — `cnet_fault_test`
  (covers `CNET_FAULT_PASS` + `CNET_PROMOTE_PASS`), `cce_adapter_bank_test`,
  `cce_dora_test`, `cnet_serve_decode_test`. The 3 heavy-link targets
  (`cnet_fault_loop_test`, `registry_lora_store_test`, `jtc_adapter_bench`) stay
  on-demand to keep `make test` fast.
- ✅ **Stray `registry.meta`** (regenerable `tests/test_expansion` sidecar) removed
  and added to `.gitignore`.

Open follow-ups (not blocking): T3.2 specialist_adapters thin wrappers still a
façade note only; the 3 heavy gates remain outside `make test` by design.
