# Thermal-Governed Flagship Harness — Design (as built)

**Date:** 2026-07-02
**Status:** Implemented same day (design pre-approved by user: "proceed with
all of these. already ahead of time agreed to design"). `src/flagship.c`,
`make flagship` gated in verify (synthetic oracle); `make flagship_run_build`
builds the real CCE-oracle CLI. Real smoke run verified: 6/6 conditional
slices extracted from `Models/gemma-4-12B-it-MTP-Q8_0.gguf` (a 4-layer,
~120M-param gemma4-assistant MTP draft model — the "12B" names its parent),
100% extraction rate, resume proven, governor live (max 46 C, duty 0.9).

## What it is

The duty-cycled, crash-resumable, hours-long compounding run over the unified
base, with a CCE-loaded transformer as the acquisition oracle
(scale-arc directive #1 + the run itself).

**Task shape:** conditional next-token slices. Closed token set V (ids into
the model's vocab). For each conditioning token t ∈ V, one unit
`acq_wa<t>q<t>` : `ONEHOT|V| "w_cur"` → `ONEHOT|V| "wa<t>q<t>"` computing
`argmax_{v∈V} P(v | [t, w])` — a genuinely distinct function per t, mined
exhaustively (|V| forwards each), certified CERT_PROVEN on the enumerated
domain, sealed into the base by the acquisition drain.

**The base is the checkpoint.** Resume = `cnb_has_unit` skip (the unit name
mirrors the drain's `acq_<goal tag>` convention — a bug caught by the gate's
resume test). Prior units re-join the planner via certify-on-load. DEFERRED
gaps persist in the ledger sidecar and reopen on re-note, so a healed oracle
retries them (gated test [3]).

**Governor** (hardware budget rules): BELOW_NORMAL process priority;
per-attempt gate = stop-file check (`<base>.stop`, clean user interruption) →
wall budget → GPU temp via `nvidia-smi` (pause above limit, resume below;
absent tool = -1 = skip) → duty-cycle sleep keeping work/wall ≤ duty. All
time is governance-only telemetry — nothing time-derived is persisted
(byte-identical saves preserved).

**Oracle injection:** `FlagshipOracleMaker` callback. The gated test uses a
synthetic deterministic model (`next(w|t) = (3w+t) mod V`, distinct
permutation per t) — no CCE, no GPU, no nvidia-smi in verify. The real CLI
(`tests/flagship_run.c`) binds `cce_anymodel_open` → `cce_gguf_qwen2_forward`
with a **determinism spot check** before mining (same query before/after an
unrelated query must match — refuses a KV-state-leaking oracle outright;
`cur_pos = 0` per call is the reset).

**Tag-family finding (recorded):** systematic tag families collide with the
base's Damerau-1 near-miss guard — `wa100` vs `wa101` IS typo distance 1. The
harness doubles the id (`wa<t>q<t>`), guaranteeing pairwise distance ≥ 2 for
distinct t while real typos still refuse. A first-class family-minting
concept in the tag registry is the honest v2 fix.

## The real run

```
make flagship_run_build
./bin/flagship_run Models/gemma-4-12B-it-MTP-Q8_0.gguf <V> <max_units> <temp_C> <duty> <wall_s> <base.cnb>
```

- Hours-long version: `V=256 max_units=256` ⇒ 256 units × 256 forwards
  ≈ 65k forwards + training/certification; duty 0.75, temp 80.
- Stop anytime: `echo stop > <base>.cnb.stop` (clean checkpoint); re-run the
  same command to resume. A crash costs at most `checkpoint_every` units.
- The decisive metric is printed per run: extraction rate =
  acquired/attempted, with deferral reasons tallied.

## Gated tests (`make flagship`, 49 checks)

[1] sweep with one broken conditioning token → 5 acquired + 1
`oracle_unfit`; [2] resume skips 5, retries only the deferral, zero
certify-on-load skips; [3] healed oracle closes the deferred gap; [4] a
loaded slice matches the synthetic model exactly on all 16 inputs through a
strict certified plan; [5] stop file halts before any work.
