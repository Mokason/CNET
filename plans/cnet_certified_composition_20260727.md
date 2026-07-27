# Certified capsule composition — verdict: **COMPOSITION_PASS** (2026-07-27)

**Repo:** `/home/marble/AI/CNET`, from `116851d`. Local commit only, not pushed.

**Scope of the claim, exactly:** a typed composition *mechanism* is proven at small
scale on one bounded 4-bit arithmetic chain. ASI here means **Artificial Specialized
Intelligence** — not AGI, not superintelligence. Nothing here shows reasoning, and
nothing shows composition beyond the chain actually executed.

---

## 1. What was proven

`make knowledge_composition_bench` → `KNOWLEDGE_COMPOSITION_BENCH_PASS members=3
hops_guarded=every refusals=root+intermediate` (**74 checks**, 0 failures).

```
num_raw --kc_incr--> val_incr --kc_double--> qty_dbl --kc_offset--> res_final
   x            (x+1)%16            ((x+1)*2)%16        (((x+1)*2)+3)%16
```

Three genuinely distinct operations on `PORT_BINARY_MSB` 4-bit ports. Each is trained to
exact reproduction of its contract, **sealed independently** into the source base
(`cnb_add_unit`), and **exported as its own capsule**. Each is then imported into a
**fresh `CnetBase` + `HybridAi`**, and it is `cnb_load_registry` — via the specialist
door — that **establishes planner certification in the fresh runtime**. To be precise:
`specialist_wrap_btn` before export only wraps; it does not certify. Certification is
what the bridge performs, and a unit that fails it is skipped (`skipped=0` here, so all
three passed).

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

## 2. Per-hop coverage enforcement — bound to the executing contract

The first version of this guard asked `hybrid_coverage_admits_unit`, which matches by
**owner name and row only** and **default-allows** an unsupported family or a dimension
mismatch. A record owned by the right unit but describing *different ports* therefore
admitted the hop, so "coverage enforced at every hop" was not true of the ports actually
executing. Review caught this; `logs/knowledge_composition_RED2.log` shows **23 checks
failing** under that legacy query.

New strict API in the owning layer, `hybrid_coverage_admits_exact(h, unit, in_port,
out_port, in, in_len)`, admits only when one active record matches **all** of: owner name
exactly; input family/width/count/**tag** exactly; output family/width/count/**tag**
exactly; `in_dim == in_len`; and the assembled row is a certified row. Missing metadata,
unsupported family, dimension mismatch or any port/tag mismatch **refuses**. Tags compare
exactly — empty is not a wildcard, because a wildcard would reopen the hole. Legacy
`hybrid_coverage_admits_unit` is deliberately unchanged.

The guard binds through the callback's `btn`, never the unit name alone.

**Guard v1 shape support is explicit, not implicit:** exactly one input port and one
output port. A multi-input (branching) or multi-output primitive **fails closed while the
guard is enabled** — unsupported, not merely untested. Legacy `NULL` guard is unchanged.

### Exact-binding evidence (all RED on the legacy query)

| Case (owner + rows correct, only the binding wrong) | Result |
|---|---|
| wrong input **tag** | refused at B, C never consulted, B's `btn_forward` did not run |
| wrong input **width** | same |
| exact ports, record `in_dim` != assembled length | same — only the independent dimension check can refuse this |
| wrong output **tag** | same |
| wrong output shape | same |
| coverage record **missing** | same |
| multi-input primitive under v1 | refused, `refuse_kind=unsupported_shape` |
| multi-output primitive under v1 | refused |

Each wrong-binding case first asserts its own SETUP — the record stored and the metadata
is present — so a negative cannot pass for the missing-metadata reason while claiming to
test port binding. Every refusal then snapshots the primitive's
`output_successes`/`output_failures` before the run and asserts them unchanged afterwards, so "did not serve" is proven by the executor's
own counters rather than inferred from a wrong answer. Restoring the correct binding
admits the hop and computes exactly.

## 2b. Per-hop mechanism

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

**Uncertified compatible primitive is excluded.** An uncertified direct
`num_raw → res_final` shortcut is registered via `registry_add` (`certified=0`). With the
certified chain intact the planner picks the chain regardless of the flag — measured —
so that comparison isolates nothing. The control therefore **breaks the chain** (B reset,
`lifecycle_enabled=1`) so the shortcut is the only remaining route, then toggles only the
flag: `require_certified=1` → **no plan**; `require_certified=0` → plan found with
members exactly `{kc_shortcut}`. The flag is the sole variable.


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
seed whose forward pass is exact on all 16 exemplars.

**What this means for the claim, stated plainly:** all 16 examples of each primitive are
used for training, model selection and certification. There is **no primitive-level
held-out generalisation here at all** — none is claimed. What was never a direct training
target is the *composite* mapping `x → ((x+1)*2+3) mod 16`, which no unit was trained on
and which the planner-discovered chain computes. This is a **mechanism test**, not a
statistical generalisation result. Certification is unchanged — the
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
  (74 checks, 0 failures)
make knowledge_composition_sanitize -> same 74 under ASAN+UBSAN+LeakSanitizer, clean
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

- One linear 3-hop chain over 4-bit binary ports. **Branching / multi-input nodes are
  UNSUPPORTED in guard v1** — not merely untested: a primitive without exactly one input
  and one output port fails closed while the guard is enabled, and that refusal is
  asserted. Supporting them needs per-slot coverage semantics, which do not exist yet.
- The guard is scoped to `dag_execute`. **`dag_execute_circuit` is explicitly left on
  legacy behaviour** (`guard = NULL`) — a circuit plan is not per-hop guarded.
- **Serving remains unwired.** `personal_ai` and `soul_host` supply no guard, so nothing
  in production is per-hop enforced today. The production claim stays **WITHHELD**.
- **Circuit execution remains legacy**: `dag_execute_circuit` passes `guard = NULL`.
- Composition is *discovered by the planner*, which is exact typed matching — not
  search over semantics. No reasoning, no AGI, no superintelligence, no real-domain
  superiority claim.
- Per-primitive trainability is the practical ceiling here, not the composition
  mechanism (§5).

## 9. Next

Wire the guard into a serving path so a composed answer is coverage-enforced in
production, and extend the proof to a branching (multi-input) node. Both are bounded
follow-ups now that the seam exists.
