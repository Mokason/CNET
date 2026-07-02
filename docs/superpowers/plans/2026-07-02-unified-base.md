# Unified Base (CNB1) Implementation Plan

> Executed inline by the same agent that wrote the spec, immediately —
> API signatures, format, and test list live in the spec
> (docs/superpowers/specs/2026-07-02-unified-base-design.md §3-§6) and are
> not duplicated here. NO GIT PUSH (local commits allowed). TDD per task;
> `make base` is the gate; `make contract_unit` guards the unit.c refactor.

**Goal:** one sealed container (units + tags + stats + oracle descriptors)
replacing per-unit file sprawl; tag governance with refusal teeth; acquisition
drain seals into the base.

- [ ] Task 1: `unit_save_mem`/`unit_load_mem` seam in src/contract/unit.c
      (path fns become wrappers); `make contract_unit` byte-green.
- [ ] Task 2: base container core — include/base.h, src/base.c: init/free/
      save/load (tmp+rename, seal-before-parse, temp-and-swap), add_unit
      (dedup byte-verified, dup-name rules), get_unit; tests [1][2][3];
      `make base` target added (link set = acquire's + base.c).
- [ ] Task 3: tag governance — mint/lookup/near_miss (case, underscore-strip,
      edit-1)/audit; auto-mint inside add_unit with refusal; test [4].
- [ ] Task 4: stats binding (put/apply with behavior-digest guard) + oracle
      descriptors (add/bind via resolver); tests [5][7].
- [ ] Task 5: registry bridge (certify-on-load, base owns BTNs) + migration
      (ingest_cnu_file); tests [6][8].
- [ ] Task 6: acquire integration (AcquireConfig.base, defer atom
      tag_collision) + wire `base` into verify; test [9]; full `make test`;
      warning sweep; local git snapshot commit.
