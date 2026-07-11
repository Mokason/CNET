# Chunk Capacity Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Measure where the flat single-hidden-layer student dies on the decimal-adder scaling family, ship the hierarchical dec_add2 (2-execution chunked circuit, 20,000/20,000 strict, certified), and gate an opt-in shuffled trainer on the stall evidence.

**Architecture:** A budgeted deterministic harness (`make study`) sweeps fixed-width students over the (1,1)/(2,1)/(2,2) adder family with epoch+time caps enforced by chunked `btn_train` calls. The hierarchy demo (`make capacity`) plans dec_add2 over the frozen `dec_full_adder_unit` with `require_certified`, sweeps all 20,000 strict, then makes ONE argv-configured flat attempt via `consolidate_circuit`. The conditional trainer is shuffle-only first (reuses `btn_train_one`), momentum only as a second gated escalation; everything is opt-in so the byte-identical regen gate survives untouched.

**Tech Stack:** C11, MinGW gcc (`-O3 -march=native -mno-avx`), GNU Make.

**Spec:** `docs/superpowers/specs/2026-06-13-chunk-capacity-design.md`.

**Grounding facts:** `btn_train` (src/nn.c:790) is per-sample online GD in FIXED order via the static `btn_train_one`; `consolidate_core` trains with `btn_train_dynamic` (fixed width = set `initial_hidden == max_hidden`); `dec_full_adder_unit` is 21→5 with ports (oh10 dec_symbol, oh10 dec_symbol, bin1 dec_carry) → (bin4 dec_sum, bin1 dec_carry), committed with its contract.

**Invariants:** `make test` green and fast after every task; `make run` / `make decimal` / `./circuit_demo` regenerate every committed artifact byte-identically; total training wall-clock across the milestone ≈ 1 hour, enforced by harness caps.

---

### Task 0: Branch

- [ ] `git checkout -b chunk-capacity`

### Task 1: Capacity harness

**Files:**
- Create: `tests/capacity_study.c`
- Modify: `Makefile` (CAPACITY_STUDY var, `capacity_study` target, phony `study`, clean; NOT in `test:`)

- [ ] **Step 1.1:** `tests/capacity_study.c`. Key pieces (full layout):
  - `build_adder_table(int da, int db, double *inputs, double *targets, size_t *n, size_t *in_w, size_t *out_w)` — the digits(a)×digits(b) family: a < 10^da, b < 10^db, cin ∈ {0,1}; inputs = one-hot-10 per digit of a (most significant first: a1 then a0 for da=2... use LEAST-significant-first per source order convention from circuit_demo: a0, a1) + same for b + cin bit; targets = hard canonical 4 bits per sum digit (s0 first) + carry bit. Domain n = 10^(da+db) × 2. Shapes: (1,1) 21→5 n=200; (2,1) 31→9 n=2,000; (2,2) 41→9 n=20,000.
  - `exact_count(btn, inputs, targets, n)` — executor bar: every output unit unambiguous (<0.25 or >0.75) AND thresholds match the hard target. (The decimal/circuit sweeps validate per port; for the study a per-unit check is equivalent because all ports here are binary.)
  - `run_row(da, db, width, epoch_cap, seconds_cap, seed)` — `btn_init(in_w, out_w, width, width, 0.8, seed)` (fixed width: initial == max); loop: `btn_train(chunk_epochs)` checks return status, then re-measures `exact_count` and loss, track best; stop on exact == n, epoch_cap, or seconds_cap (clock()). chunk_epochs: 2000 for n=200, 500 for n=2,000, 50 for n=20,000. Print one row: `da x db | n | width | epochs | best exact | final loss | seconds | MACs/forward` (MACs = in_w*width + width*out_w).
  - main: argv[1] optional per-run seconds cap (default 240; the two (2,2) rows use 1.5× the cap). Row list:

| scale | widths | epoch cap |
|-------|--------|-----------|
| (1,1) | 64, 128 | 400,000 |
| (2,1) | 64, 128, 256 | 200,000 |
| (2,2) | 128, 256 | 20,000 |

  - After the table: print the chunked-circuit reference line: `chunked dec_add2: 2 executions x (21*128 + 128*5) = 6,656 MACs` and the flat configs' MACs beside it (w=256 flat: 41*256+256*9 = 12,800 — already past the crossover).
  - Everything seeded and deterministic except wall-clock; rows print epochs-used so exact counts reproduce by epoch count on any machine.
- [ ] **Step 1.2:** Makefile: `CAPACITY_STUDY := tests/capacity_study.c`; target links `$(SRC)` only (no router needed); phony `study: capacity_study` runs `./capacity_study`; extend `clean:`. Do NOT add to `test:`.
- [ ] **Step 1.3:** Build; smoke-run with a tiny cap (`./capacity_study 5`) — rows print, caps respected, exit 0.
- [ ] **Step 1.4:** Commit: `feat: capacity study harness (budgeted, deterministic)`

### Task 2: Hierarchical dec_add2 demo

**Files:**
- Create: `tests/capacity_demo.c`
- Modify: `Makefile` (`capacity_demo` target linking SRC+ROUTER+PLAN_TABLE+CONSOLIDATE+CONTRACT, phony `capacity`, clean)

- [ ] **Step 2.1:** `tests/capacity_demo.c`, three parts (helpers copied per repo convention: PT, bits_to_int, int_to_bits, int_to_onehot, print_node from circuit_demo.c):
  - **Part 1 — certified hierarchical plan:** load `dec_full_adder_unit_weights.txt` + `dec_full_adder_unit_contract.txt`; fresh registry; `registry_add_certified`; `reg.require_certified = 1`. Sources `(oh10 a0, oh10 b0, oh10 a1, oh10 b1, bin1 cin)` all `dec_symbol`/`dec_carry` tagged; goals `{bin4 dec_sum, bin4 dec_sum, bin1 dec_carry}`. `dag_plan_circuit` → assert 2 DISTINCT unit executions, ones consumes cin, tens carry slot = ones node (port 1), cout = tens port 1. Print the structure.
  - **Part 2 — exhaustive strict verification:** all a,b ∈ 0..99, cin ∈ {0,1} via `dag_execute_circuit` (strict; out 9 wide); assert `cout*100 + s1*10 + s0 == a+b+cin`, 20,000/20,000; print "2 forward passes per addition (the raw circuit needed 6)".
  - **Part 3 — the flat attempt:** argv `--flat <width> <epochs> [shuffled]` (no argv = skip with a pointer to `make study`). `consolidate_circuit` on the 2-unit circuit (same 20,000-sample teacher domain) with cfg: `max_samples 20000, initial_hidden = width = max_hidden, max_epochs = epochs, growth_window = epochs (never grows), learning_rate 0.8, target_loss 0.0008`; `use_shuffled_trainer` when the third arg says so (Task 4). On success: save `dec_add2_weights.txt`, emit + save contract via `contract_from_circuit`, `btn_certify`, replan with the chunk registered certified → expect the 1-execution root; on refusal print the report row honestly.
- [ ] **Step 2.2:** Makefile wiring; build; run `./capacity_demo` (no flat attempt) → parts 1-2 pass.
- [ ] **Step 2.3:** Commit: `feat: hierarchical dec_add2 -- certified 2-chunk circuit, 20000/20000 strict`

### Task 3: Frozen test additions

**Files:**
- Modify: `tests/test_circuit.c` (frozen half)

- [ ] **Step 3.1:** Extend `frozen_circuit_chunk()` (or a sibling fn): plan the 2-unit circuit exactly as the demo does (certified registry), assert the forced 2-execution shape, then run the FULL chunked sweep (20,000 strict executions — two 21-wide forwards each, ~1s) and CHECK 20,000/20,000. Skip with FAIL message if the unit files are missing.
- [ ] **Step 3.2:** `make test` → all suites green, still fast. Commit: `test: the chunked dec_add2 sweep joins the frozen suite`

### Task 4 (CONDITIONAL — gate: some width reaches >=99% exact but cannot close within budget, two seeds): opt-in shuffled trainer

**Files:**
- Modify: `include/nn.h`, `src/nn.c`, `include/consolidate.h`, `src/consolidate.c`, `tests/test_nn.c`

- [ ] **Step 4.1:** `src/nn.c` — shuffle-only first, reusing `btn_train_one` verbatim:

```c
/* Opt-in trainer: identical per-sample updates to btn_train, but the
   sample ORDER is a fresh deterministic permutation each epoch (seeded
   LCG + Fisher-Yates). Fixed-order online GD can orbit instead of
   converging on large tables; shuffling breaks the orbit. New code only
   -- btn_train/btn_train_dynamic and every existing weight file are
   untouched. */
void btn_train_shuffled(
    BinaryTransformNetwork *btn,
    const double *inputs,
    const double *targets,
    size_t sample_count,
    size_t epochs,
    unsigned int seed
) {
    /* order[] init 0..n-1; per epoch: Fisher-Yates with
       lcg = lcg * 1664525u + 1013904223u; then btn_train_one per index. */
}
```

  (Momentum variant `btn_train_momentum` is a SECOND gate: only if shuffle alone still stalls on the gate case; same opt-in pattern, velocity buffers over all weight arrays.)
- [ ] **Step 4.2:** `ConsolidateConfig` gains `int use_shuffled_trainer;` (zero-init = legacy). In `consolidate_core`, branch: knob set → fixed-width `btn_train_shuffled(..., cfg.max_epochs, cfg.seed)` + loss computed after; else the existing `btn_train_dynamic` call, byte-identical.
- [ ] **Step 4.3:** Tests in `tests/test_nn.c`: (a) same seed → byte-identical weights across two runs (memcmp the weight arrays); (b) different seed → different trajectory (any weight differs); (c) defaults: `consolidate_config_defaults` leaves the knob 0.
- [ ] **Step 4.4:** `make test` green; **regen sentinel:** `./nn_demo && ./decimal_demo && ./circuit_demo && git status --short` → clean tree. Commit: `feat: opt-in shuffled trainer (btn_train_shuffled), regen-gate safe`

### Task 5: The measurement session

- [ ] **Step 5.1:** `make study`; capture the full table. (~35-40 min. Run it once; one extension only with explicit user approval per the spec.)
- [ ] **Step 5.2:** Apply the trainer gate criterion to the table. If gated in: execute Task 4, rerun ONLY the gate rows old-vs-new (paired table), then decide the flat-attempt config.
- [ ] **Step 5.3:** The flat attempt: `./capacity_demo --flat <best width> <epochs that fit ~10 min> [shuffled]`. Record success (then commit `dec_add2_weights.txt` + contract as baseline, extend the frozen test) or the refusal row.
- [ ] **Step 5.4:** Append a `## Results (measured)` section to the spec with the table verbatim and the gate decision.

### Task 6: Findings, regression, merge

- [ ] **Step 6.1:** README "Chunk Capacity" section: the measured table, the scaling relationship, the MACs crossover (flat w=256 = 12,800 MACs/forward vs chunked 6,656), and the conclusion the data supports. Make-targets table: `make study`, `make capacity`.
- [ ] **Step 6.2:** Full regression: `make clean && make test` (Bash for clean); `./nn_demo && ./decimal_demo && ./circuit_demo`; `git status --short` clean.
- [ ] **Step 6.3:** Memory entry (capacity findings + law) + MEMORY.md line.
- [ ] **Step 6.4:** Commit docs; merge `chunk-capacity` → master fast-forward; delete branch; re-run `./test_circuit` on master.

---

## Self-review notes

- **Spec coverage:** harness → Task 1; hierarchy demo + certified plan + sweep → Task 2; frozen test → Task 3; conditional trainer + knob + determinism tests + regen sentinel → Task 4; measurement + gate decision + flat attempt + spec results → Task 5; findings/README/memory/merge → Task 6.
- **Type consistency:** `btn_train_shuffled(btn, inputs, targets, n, epochs, seed)` used identically in Tasks 4 and 5; `use_shuffled_trainer` named identically in consolidate.h/.c and capacity_demo's `--flat ... shuffled` path.
- **Budget:** rows 5×240s + 2×360s ≈ 32 min + flat attempt ≤ 10 min + slack = within the hour.
