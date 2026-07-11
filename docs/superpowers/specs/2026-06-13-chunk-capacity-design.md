# Chunk Capacity: the dec_add2 Study and Hierarchical Consolidation — Design

**Date:** 2026-06-13
**Status:** Approved

## Goal

Answer the open question from the circuit-plans milestone: can a single
primitive absorb the 20,000-sample 2-digit adder (41 inputs -> 9
outputs), and if not, what is the honest way to scale chunk capacity?

The deliverable is the ANSWER, not a flat chunk at any cost. A flat
`dec_add2` is precisely the monolith this project argues against; the
thesis-aligned answer — already within reach of the existing machinery —
is hierarchical: a 2-execution circuit over the frozen, certified
`dec_full_adder_unit` chunks. The study measures where the flat student
actually dies and documents the law.

**Wall-clock budget: ~1 hour total for all training runs.** The harness
enforces it; nothing in `make test` trains at scale.

## Components

### 1. Capacity harness (`tests/capacity_study.c`, `make study`)

A deterministic, budgeted sweep over a natural scaling family — the
digits(a) x digits(b) decimal adders, identical in kind, x10 in domain:

| Scale | Domain | Student shape |
|-------|--------|---------------|
| (1,1) | 200    | 21 -> 5  (known point: width 128 + 300k epochs reaches 200/200) |
| (2,1) | 2,000  | 31 -> 9 |
| (2,2) | 20,000 | 41 -> 9 |

For each scale x width {64, 128, 256}: FIXED width (no growth — dynamic
growth confounds the capacity axis), canonical hard-target tables (the
same tables a strict teacher would produce), training driven in chunks
of `btn_train` calls that return `0` on success so the harness can enforce
both an epoch cap (the reproducible unit) and a per-run time cap (the safety). Per run it
records: best exact count over the domain, final loss, epochs used,
seconds. Output: one table row per run, printed and collected for the
README. Seeds fixed; exact counts are reproducible by epoch count,
timings are machine-indicative only.

The harness also prints the analytic inference cost (MACs per forward)
of each flat config against the chunked circuit (2 executions of the
21->5 unit, ~7k MACs): the capacity-by-fattening approach must beat
that line to be worth anything, which is the crossover argument.

### 2. Hierarchical dec_add2 (`tests/capacity_demo.c`, `make capacity`)

- Registry: `dec_full_adder_unit` alone (certified, `require_certified`
  set). Sources (oh10 a0, oh10 b0, oh10 a1, oh10 b1, bin1 cin); goals
  {dec_sum, dec_sum, dec_carry}. Types force the 2-execution carry-chain
  circuit; `dag_plan_circuit` discovers it certified end-to-end.
- Exhaustive verification: all 20,000 two-digit additions, strict, via
  `dag_execute_circuit` — TWO forward passes per addition instead of the
  raw circuit's six.
- The flat attempt: ONE distillation try at the most promising config
  the study found (possibly with the conditional trainer below). On
  100% verification: save, certify against `contract_from_circuit`'s
  emission, show the 1-execution certified replan. Otherwise: print the
  refusal with the best measured row — the honest outcome.
- `test_circuit` frozen half gains the 2-unit plan shape + the chunked
  20,000-case sweep (two small forwards per case — fast enough for
  `make test`).

### 3. Conditional opt-in trainer (`btn_train_shuffled`)

Implemented ONLY if the study shows an optimization stall rather than a
capacity shortfall (loss plateaus while exact-count oscillates at widths
that plainly have the parameters). Gate criterion, concretely: at some
width the student reaches >= 99% exact but cannot close the last
fraction within its epoch budget, across two seeds.

Shape when gated in:
- `btn_train_shuffled(btn, inputs, targets, n, epochs, seed)` in
  src/nn.c: per-epoch deterministic permutation (seeded LCG, private to
  the function) + classic momentum (0.9), fixed width, no growth.
- `ConsolidateConfig` gains `use_shuffled_trainer` (zero-init = legacy
  trainer, so every existing call site and weight file is untouched).
- Hermetic test: same seed -> byte-identical weights; knob off ->
  byte-identical consolidation behavior.
- The harness reruns the key configs old-vs-new as a paired table.

**The regen gate is non-negotiable:** `btn_train` / `btn_train_dynamic`
and every existing call path stay byte-for-byte; `make run`,
`make decimal`, `./circuit_demo` must regenerate every committed weight
file identically after this milestone.

### 4. Findings

README gains a "Chunk Capacity" section: the measured table, the
scaling relationship (epochs-and-width to 100% vs domain size), the
inference-cost crossover, and the conclusion the data supports —
expected: capacity scales by composing certified chunks, not by
fattening students. If the flat chunk IS reached, the conclusion gets
stated with its measured price instead. Spec results appended
post-measurement; memory updated.

## Success criteria

1. Chunked dec_add2 verified 20,000/20,000 strict, certified end-to-end
   (guaranteed deliverable, no new machinery).
2. Capacity table committed (deterministic exact counts; indicative
   timings), with the crossover analysis.
3. Flat dec_add2 achieved-and-certified OR refused-with-evidence.
4. All suites green; `make test` stays fast (the study is a separate
   target); byte-identical regeneration of every committed artifact.

## Alternatives considered

- **Fat flat student as the goal** — rejected: even on success it loses
  on inference cost beyond modest widths, and on failure it teaches
  nothing; the study subsumes it as one measured point.
- **Unconditional trainer upgrade** — rejected: without the stall
  evidence it is speculative core surface; the data gates it.
- **Truncated-domain intermediate scales** (e.g. fixing cin) — rejected
  for the (2,1) family: digits(a) x digits(b) keeps every scale a real
  adder, same family, clean x10 steps.

## Risks

1. **The hour disappears into the (2,2) runs.** Mitigation: per-run
   time caps enforced by the harness; the (1,1) and (2,1) curves carry
   the scaling argument even if (2,2) only yields lower bounds.
2. **Stall-vs-capacity ambiguity.** The gate criterion is written down
   above; if results are ambiguous, the trainer stays out and the
   ambiguity is the finding.
3. **Study results tempt unbounded iteration.** The budget is part of
   the spec; one extension requires explicit user approval.
