# Second Domain: Decimal Arithmetic — Design

**Date:** 2026-06-12
**Status:** Implemented

## Goal

Test the project's central claim — the core is domain-agnostic and
knowledge scales by adding primitives — by building a second real domain
(decimal digit arithmetic with carry) using only existing machinery:
typed ports, semantic tags, multi-output primitives, DAG planning,
learned reliability, strict execution, chunk consolidation, exemplar
contracts and property laws. Any forced core change counts as a finding
against the thesis and must be documented, not silently patched.

**Outcome: zero core changes were needed.** Every file in `src/` is
untouched by this milestone (the only edit was a pre-existing one-char
comment typo in `src/property.c` that broke compilation).

## The domain

Tags (new data only):

| Tag | Representation | Meaning |
|-----|----------------|---------|
| `dec_symbol` | onehot 10 | digit symbol — the enumerable boundary representation |
| `dec_digit` | binary_msb 4 | digit value 0-9 — representation-identical to a hex nibble (the collision under test) |
| `dec_carry` | binary_msb 1 | carry; the same tag on carry-out port and carry-in slot is what lets carries chain |
| `dec_sum` | binary_msb 4 | sum digit; distinct from `dec_digit` so a sum goal cannot be satisfied by a bare digit source |

Primitives, all trained in `tests/decimal_demo.c` and frozen behind an
exact exhaustive domain gate (below):

| Primitive | Signature | Domain |
|-----------|-----------|--------|
| `dec_value` | onehot10 `dec_symbol` -> bin4 `dec_digit` | 10 |
| `dec_to_symbol` | bin4 `dec_digit` -> onehot10 `dec_symbol` | 10 |
| `dec_full_add` | (bin4 `dec_digit`, bin4 `dec_digit`, bin1 `dec_carry`) -> (bin4 `dec_sum`, bin1 `dec_carry`) | 200 |
| `dec_swap_ab` | (oh10, oh10, bin1) -> (b, a, carry) | 200 |
| `dec_add_unit` | chunk distilled from the discovered adder plan | 200 |

## The six acts (`make decimal`)

1. **Train + gate + freeze.** New discipline: a primitive may freeze only
   if, over its whole training domain, every raw output segment is
   in-domain (`port_validate`, the executor's bar) and canonicalizes to
   exactly the target row. An undertrained primitive breaks every plan
   built on it, so anything under 100% refuses to save.
2. **Planner discovery.** From sources (symbol, symbol, carry) and a
   tagged `dec_sum` goal, `dag_plan` discovers
   `dec_full_add(dec_value(a), dec_value(b), cin)` and executes all 200
   members exactly. The goal must carry the tag: an untagged bin4 goal is
   satisfied by `dec_value` alone.
3. **Cross-domain refusal.** `hex_value` emits bin4 `nibble_value` —
   representation-identical to `dec_digit`. The tag alone keeps hex
   sources out of decimal adders (`dag_plan` and `route_plan` both
   refuse) while the legitimate decimal route still plans.
4. **Ripple-carry as a program.** Two-digit addition is hand-wired from
   stack `DagNode`s (carry-out projection feeding the tens adder's
   carry-in slot) and run strict. See finding 2.
5. **Consolidate + certify + replan.** `consolidate_dag` distills the
   act-2 plan 200/200; `contract_from_dag` emits the teacher's contract;
   the chunk certifies against it; with `require_certified` set, the
   evidence-seeded chunk wins the replan as a bare
   `dec_add_unit(s0, s1, s2)`.
6. **Laws.** `dec_to_symbol(dec_value(s)) = identity` (10/10) and
   commutativity `add(a,b,c) = add(b,a,c)` as LHS `[dec_add_unit]` vs
   RHS `[dec_swap_ab, dec_add_unit]` (200/200). `dec_swap_ab` exists
   because property chains hand off whole port sequences positionally —
   that is how an argument permutation becomes expressible as a law.

`tests/test_decimal.c` adds the hermetic tag-safety checks (synthetic
nets, no files) and the sweeps the demo only samples: the full adder
domain, the exhaustive 20,000-case ripple add under strict execution,
chunk certification, and both laws from disk.

## Findings

1. **Enumerable boundaries must be one-hot.** `field_cardinality`
   (`src/plan_table.c`) enumerates a binary port's full bit-space — a
   bin4 `dec_digit` source would enumerate 16 values including the
   invalid 10-15. Consolidation teachers, contract emission and property
   replay all enumerate source ports, so every such boundary uses
   onehot10 `dec_symbol` ports; binary digit ports appear only at
   handoffs between primitives. This is a real constraint the hex domain
   never exposed (its binary ports' bit-spaces were exactly their
   domains).
2. **Multi-output circuits are programs, not plans.** Plans are trees
   (one projected output) with globally single-use sources, and the
   score objective prefers fewer primitives. A ripple-carry adder needs
   three results and a shared ones-place computation, so the planner
   cannot discover it — by design, not by bug. The circuit is hand-built
   over frozen primitives and verified exhaustively. If discovered
   multi-output circuits ever matter, the missing mechanism is shared
   sub-results (common subexpression reuse), already on the roadmap.
3. **Start wide enough.** The chunk anti-saturation rule
   (`include/consolidate.h`) generalizes to training: `dec_full_add`
   from 9 hidden neurons with eager growth (`min_improvement` 0.03)
   capped out at 96 neurons and 194/200; from 16 with patient growth
   (0.01) it reaches 200/200 with zero growth. Capacity added late
   cannot repair early saturation, so begin near the task's natural
   width and grow reluctantly.

## Alternatives considered

- **Planner-discovered ripple-carry** — rejected: requires three core
  changes (multi-goal plans, shared sources, positional semantics);
  the milestone's point is to NOT change the core.
- **bin4 digit inputs for the commutativity law** — rejected: the
  enumeration would include invalid digits (finding 1). The law runs
  over one-hot symbol sources through the chunk instead.
- **`dec_add` without carry-in** (fallback) — not needed once the
  full adder trained to 200/200.

## Test plan

- `make decimal` — all six acts pass, exit 0.
- `make test` — nine suites including `test_decimal` (hermetic tag
  safety + frozen sweeps).
- Determinism: a second `./decimal_demo` run reproduces all 12 `dec_*`
  files byte-identically (verified by hash comparison).
