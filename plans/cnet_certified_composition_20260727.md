# Certified capsule composition — verdict: **COMPOSITION_PASS** (2026-07-27)

**Repo:** `/home/marble/AI/CNET`, from `116851d`. Local commit only, not pushed.

**Scope of the claim, exactly:** a typed composition *mechanism* is proven at small
scale on one bounded 4-bit arithmetic chain. ASI here means **Artificial Specialized
Intelligence** — not AGI, not superintelligence. Nothing here shows reasoning, and
nothing shows composition beyond the chain actually executed.

---

## 1. What was proven

`make knowledge_composition_bench` → `KNOWLEDGE_COMPOSITION_BENCH_PASS members=3
hops_guarded=every refusals=root+intermediate` (23 checks, 0 failures).

```
num_raw --kc_incr--> val_incr --kc_double--> qty_dbl --kc_offset--> res_final
   x            (x+1)%16            ((x+1)*2)%16        (((x+1)*2)+3)%16
```

Three genuinely distinct operations on `PORT_BINARY_MSB` 4-bit ports, each
independently certified through the real specialist door, each exported as its **own
capsule**, each imported into a **fresh `CnetBase` + `HybridAi`**, then bridged into the
planner with `cnb_load_registry` and `require_certified = 1`.

| Evidence | Result |
|---|---|
| Registry after bridge | `count=3 skipped=0`, all `certified=1 state=2 (FROZEN)` |
| Plan discovered for `num_raw → res_final` | members **3**: `kc_offset`, `kc_double`, `kc_incr` |
| Direct shortcut primitive | none exists (asserted) |
| Composed tasks executed | **14 ran, 14 exact** for `f(x)=((x+1)*2+3) mod 16` |
| Hand-built `DagNode`s | **zero** — the bench never constructs one |

The composite task is what is held out: no unit was ever trained on `x → f(x)`. Each
primitive's own contract is exhaustive over its 16 inputs, and that is stated rather
than dressed up as generalisation.

## 2. Per-hop coverage enforcement

New opt-in `DagNodeGuard` on `DagPlan` (`include/router.h`), checked in `dag_full.c`
immediately before `btn_forward(p, assembled)` — so the guard sees the **exact
concatenated input at that hop**, intermediate values included, and a refused hop never
executes. `dag_plan` zeroes it on every path, so a plan handed to a legacy caller
behaves byte-identically to before; `make split` still passes unchanged.

A veto returns the distinct code **`DAG_EXEC_REFUSED_GUARD` (-2)**, never `-1` and never
a partial result: `dag_execute` does not fall back to a residual, so the caller keeps
that policy. A hop that never ran records no reliability evidence — a coverage refusal
is not a wrong model output.

Two refusal points proven with a callback trace, not just an output mismatch:

| Case | Trace | Code |
|---|---|---|
| Root input outside A's coverage | guard consulted for `kc_incr` only — **B and C never consulted** | `-2` |
| A accepts, intermediate outside B's coverage | `kc_incr` ran, `kc_double` checked and **stopped**; C never consulted | `-2` |

Missing coverage metadata also fails closed: the guard refuses a unit with no record
rather than treating absence as "unrestricted".

## 3. RED evidence

`logs/knowledge_composition_RED.log`:

- At `116851d` the capability did not exist — `DagNodeGuard` count 0 in `router.h`, no
  hook of any kind between input assembly and `btn_forward`. (The 5 pre-existing
  "guard" hits in `dag_full.c` are unrelated ORDER_ONLY planner comments.)
- **Causality, in the same run:** with the guard disabled, the intermediate-OOD input
  `x=3` (intermediate `4`, outside B's coverage) **executes and even returns the
  arithmetically correct answer**. The guard is what stops it, and the refusal is not a
  side effect of some earlier failure.

## 4. Negative controls

- **B reset** under `lifecycle_enabled` → `num_raw → res_final` becomes unplannable.
  Asserted on plan validity, not on output.
- **Unknown goal tag** → no plan (typed refusal).
- Every negative asserts its own stage: the plan checks assert member *names and count*,
  and the refusal checks assert *which unit* refused plus which units were never
  consulted, so nothing can pass because an earlier stage failed for an unrelated reason.

## 5. Honest finding: the substrate needed a seed sweep

4-bit modular binary arithmetic did **not** converge to exactness under a single fixed
configuration. Measured: with tight tolerances `+1` stalled at **15/16** and `+3` at
**14/16**; with the proven loose recipe from `src/legacy/main.c` (`btn_init(...,1,128,0.8)`,
160k epochs, tol 0.0015/0.01) `+3` became exact but `+1` stayed 15/16 and `*2` fell to
12/16.

Resolution: a **bounded deterministic seed sweep** (seeds 1..24), stopping at the first
seed whose forward pass is exact on all 16 exemplars. Certification is unchanged — the
contract must still be reproduced exactly, and `cnb_load_registry` re-certifies at the
specialist door. The bench prints the winning seed per unit (`kc_incr` 2, `kc_double` 1,
`kc_offset` 5) so the run stays reproducible. No floor was lowered and no unit was
hand-sealed.

This is a real property worth recording: **the composition mechanism is solid; the
per-primitive trainability of binary carry chains is not**, and a larger chain would
need either wider ports (the legacy increment uses 4→5 with a carry bit) or a better
training schedule.

## 6. Gate results

```
KNOWLEDGE_COMPOSITION_BENCH_PASS members=3 hops_guarded=every refusals=root+intermediate
  (23 checks, 0 failures)
make knowledge_composition_sanitize -> same 23 under ASAN+UBSAN+LeakSanitizer, clean
KNOWLEDGE_CAPSULE_PASS checks=88
KNOWLEDGE_ACCUMULATION_BENCH_PASS units=32 distinct=1 isolation=32/32 replay=8/8
COVERAGE_ABSTAIN_PASS checks=55 heldout_correct=4/4 was=0/4
SPLIT PASS (legacy planner path unchanged by the guard)
HYBRID_AI_PASS checks=21
CNET_CI_CORE_PASS
git diff --check -> clean
```

## 7. CI placement

`knowledge_composition_bench` stays **focused, not in `ci_core`**. It is deterministic
and hermetic (fixed seeds, temp dirs, fresh bases, production base never opened), but it
costs **~20.6 s**, essentially all BTN training across the seed sweep — roughly 20× the
accumulation bench. `ci_core` already carries `knowledge_capsule` and
`knowledge_accumulation_bench` for the capsule/accumulation regressions. Revisit if the
training cost drops.

## 8. Limitations — what this does NOT show

- One linear 3-hop chain over 4-bit binary ports. **Branching / multi-input guard
  composition is untested**; the guard receives the full assembled input so it should
  work for multi-slot nodes, but v1 is only *proven* for linear chains.
- The guard is scoped to `dag_execute`. **`dag_execute_circuit` is explicitly left on
  legacy behaviour** (`guard = NULL`) — a circuit plan is not per-hop guarded.
- Serving paths (`personal_ai`, `soul_host`) do not yet supply a guard; this adds the
  mechanism and proves it, it does not switch production onto it.
- Composition is *discovered by the planner*, which is exact typed matching — not
  search over semantics. No reasoning, no AGI, no superintelligence, no real-domain
  superiority claim.
- Per-primitive trainability is the practical ceiling here, not the composition
  mechanism (§5).

## 9. Next

Wire the guard into a serving path so a composed answer is coverage-enforced in
production, and extend the proof to a branching (multi-input) node. Both are bounded
follow-ups now that the seam exists.
