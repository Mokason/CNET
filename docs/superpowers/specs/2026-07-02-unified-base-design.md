# Unified Base (CNB1): one sealed container + tag governance — Design

**Date:** 2026-07-02
**Status:** Approved direction (user 2026-07-02: "make unified base that can be
modular, just don't make it monolithic bloat" + tag governance delegated);
this doc fixes the mechanics.
**Topic:** Replace per-unit file sprawl (`<name>.cnu` + `<name>.stats` +
`<name>.expansion` × hundreds of units) with ONE sealed, content-addressed
container file — while keeping every unit independently sealed, verifiable and
extractable (modular, NOT a monolith). Tag governance (mint-once registry with
near-miss refusal and provenance) lives inside the container, because tags are
minted exactly when units enter the base. Oracle *descriptors* persist here
too, so the acquisition loop's label sources are managed, not scattered.

Prereq for the flagship hours-long run (see scale-arc directives): the run
mints hundreds of units; loose files were about to explode.

---

## 1. Goal & acceptance criteria

1. **One file, many sealed units.** A `CnetBase` holds unit payloads (the
   exact CNU1 byte images) in a content-addressed blob table + a name→blob
   reference table. Adding an identical payload is idempotent
   (`reused=1`, byte-verified against the stored blob — never trusted on the
   64-bit digest alone; digest collision with different bytes is REFUSED).
   The CCE weight-store honesty rules, applied to CNET units.
2. **Trust is replayed, never stored.** `cnb_load_registry` re-certifies every
   unit (`btn_certify` against its own embedded contract — cheap via the
   certification cache) and admits via `registry_add_certified`. A unit whose
   replay fails is skipped and reported, never admitted. No certificate is
   persisted (existing project rule).
3. **Tag governance with teeth.** Tags are mint-once entries with provenance
   (owner unit + mint sequence number — deliberately NO wall-clock timestamps,
   preserving byte-identical saves). `cnb_add_unit` auto-mints every port tag
   of the incoming unit; a NEAR-MISS against an existing tag **refuses the
   unit add** with the colliding tag named. Near-miss (deterministic,
   mechanical): case-insensitive equality, underscore-stripped equality, or
   Damerau-Levenshtein distance 1 (one substitution, insertion, deletion, or
   adjacent transposition — plain Levenshtein misses `nibbel`) — while exact
   equality is simply the same tag
   (idempotent re-mint). `nibble` vs `nibbel`/`Nibble`/`nibble_` → refused;
   `nibble` vs `nibble_next` (distance 5) → fine. Audit report lists every
   tag, owner, referencing-unit count, and orphans.
4. **Stats with mechanical staleness.** Per-unit reliability counters are
   stored bound to the unit's `contract_btn_digest`; `cnb_apply_stats`
   restores counters ONLY when the live unit's digest matches — the
   retrainer-invalidates rule enforced by mechanism instead of convention.
5. **Oracle descriptors.** (name, kind atom, input Port, goal Port) rows
   persist in the base; `cnb_bind_oracles` binds them into a runtime
   `OracleRegistry` through a caller-supplied resolver (functions cannot be
   persisted; intent can). Unresolvable descriptors are skipped, counted.
6. **Sealed + deterministic.** Whole-file FNV-1a seal, verified BEFORE parsing
   (unit.c discipline). save→load→save is byte-identical (regression gate).
   Any flipped byte refuses the whole file; individual unit blobs retain their
   own CNU1 seals, so extraction re-verifies per unit.
7. **The acquisition loop stops sprawling.** `AcquireConfig` gains
   `CnetBase *base` (NULL = legacy `unit_dir` behavior, byte-identical);
   when set, the drain seals acquired units INTO the base and mints their
   tags — a governance-refused tag DEFERs the acquisition (`tag_collision`).
8. **Migration.** `cnb_ingest_cnu_file` ingests existing loose `.cnu` files;
   gated test proves registry-from-base ≡ registry-from-files (behavior
   digests equal).

## 2. Non-goals (v1)

- **No cross-unit blob sharing claims.** CNU1 images embed the unit name, so
  two different-named units never share bytes; dedup fires on re-ingest of
  the same unit (idempotence), like the corpus store's "identical book = 100%
  reuse". Honest framing, stated in the header comment.
- **Gap ledger and expansion recipes stay as their own sidecars.** One ledger
  file is not sprawl; recipes are rare. Embedding them is mechanical follow-up.
- **No CCE-model-as-oracle execution.** That's the flagship milestone; here we
  only persist descriptors (kind atom e.g. `builtin` / `cce_model`).
- **No removal/GC.** Demotion is registry state (PRIM_RESET); the base keeps
  history. Compaction is follow-up if bases ever bloat.

## 3. Container format (binary, little-endian as written by the host, like CNU1)

```
"CNB1" u32_version
u64 blob_count     { u64 digest, u64 len, bytes[len] }            × blob_count
u64 unit_count     { u16 nlen, name, u64 blob_index, u64 behavior_digest } × unit_count
u64 tag_count      { u16 tlen, tag, u16 olen, owner, u64 mint_seq } × tag_count
u64 oracle_count   { u16 nlen, name, u16 klen, kind,
                     port(in: u32 fam, u64 w, u64 c, u16 taglen, tag),
                     port(goal: same) }                            × oracle_count
u64 stats_count    { u16 nlen, name, u64 bound_digest,
                     u64 successes, u64 failures }                 × stats_count
u64 SEAL           (FNV-1a over all preceding bytes)
```

Deterministic: all tables in insertion order; mint_seq is a monotonic counter,
not time. Load parses into a temp CnetBase and swaps on full success
(malformed/tampered → -1, *b untouched — the sidecar rule).

## 4. Components

- **`unit_save_mem` / `unit_load_mem`** (src/contract/unit.c): expose the
  existing in-memory blob build/parse as public API; `unit_save`/`unit_load`
  become thin file wrappers. Behavior-identical; `make contract_unit` is the
  regression gate.
- **`include/base.h`, `src/base.c`** — `CnetBase` (open struct, dynamic
  arrays), `cnb_init/free/save/load`, `cnb_add_unit(btn, c, *reused)`,
  `cnb_get_unit(name, btn, c)`, `cnb_load_registry(reg)` (base owns loaded
  BTNs, freed in `cnb_free` after the registry is done — the acquire-ledger
  ownership pattern), `cnb_tag_mint/lookup/near_miss/audit`,
  `cnb_put_stats/apply_stats`, `cnb_add_oracle_desc/bind_oracles`,
  `cnb_ingest_cnu_file`.
- **`src/acquire.c`**: `AcquireConfig.base` (opt-in) — seal step targets the
  base; new defer atom `tag_collision`. DEFER stays total (a refused add
  leaves the base byte-identical: add is all-or-nothing in memory, and the
  base only touches disk on `cnb_save`).
- **`tests/test_base.c` + `make base`** in verify.

## 5. Error handling / invariants

- Seal-before-parse everywhere; digest-collision-different-bytes refused.
- Name atoms only (unit/tag/oracle/kind), so the text-adjacent fields stay
  scannable and consistent with the rest of the project.
- Duplicate unit name: identical blob → idempotent OK (reused=1); different
  blob → refused (-1) — retraining goes through the rebuild path's fresh
  names, never silent replacement.
- `cnb_apply_stats` on digest mismatch: -1, counters untouched.
- All add operations are in-memory; disk state changes only at `cnb_save`
  (single rewrite, seal recomputed). A crash mid-save loses at most that save:
  callers keep the previous file until rename — v1 writes to `<path>.tmp`
  then `rename()` over the target (atomic-enough on NTFS; crash-resumability
  requirement from the hardware budget).

## 6. Tests (`tests/test_base.c`, gated)

1. Round-trip: 2 units in, save, load, `cnb_get_unit` both — behavior digests
   match; re-add identical unit → reused=1, counts unchanged; same name
   different weights → refused.
2. Tamper: flip one payload byte on disk → load -1, base untouched.
3. Determinism: save→load→save byte-identical files.
4. Tags: mint + idempotent re-mint; case/underscore/edit-1 near-miss all
   refused with the existing tag named; distant tags fine; unit add
   auto-mints; unit whose tag near-misses → add refused; audit prints owners.
5. Stats binding: save counters, restore into a fresh load → applied;
   retrained unit (new digest) → refused, counters untouched.
6. Registry bridge: base with hex_value + increment → `cnb_load_registry` →
   both FROZEN certified, `route_plan` composes the 2-hop chain
   (require_certified=1), strict 16/16.
7. Oracles: descriptor round-trip; resolver binds one, skips one unbound.
8. Migration: two loose `.cnu` files → `cnb_ingest_cnu_file` ×2 → one base;
   registry-from-base behavior-digest-identical to registry-from-files.
9. Acquire integration: drain with `cfg.base` set → no loose `.cnu` written,
   unit in base, tags minted, gap CLOSED, replan OK; tag-collision fixture →
   DEFER `tag_collision`, base byte-identical.
