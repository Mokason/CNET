# Distrust loop (property next) — design constraints

**Status:** implemented — `make distrust_loop` / `make autonomy_tick` (T2).  
**Property served:** default under ignorance is refusal with provenance; distrust may trigger *gated* structure, not free evolve.

## Already shipped (trust side)

- Detect: domain Wilson LCB + `live_outcomes` / kinds (`trust` | `explore` | `prior`)
- Refuse Tier-A when LCB low or `live < min_live`
- Explore keeps estimator alive; journal labels explore ≠ trust
- Prior alone never ALLOW

## Not shipped (act on distrust)

Ordered actions when decide → REFUSE (after coverage still open):

| Action | Constraint |
|--------|------------|
| Reroute B/C or other specialist | **Target must pass its own seal-trust decide** (or be explicitly uncertified residual). Never chain out of distrust into another cold/bad domain and call it handled. |
| Propose / remine | Existing admit bars only; distrust event is *evidence*, not seal |
| Escalate human | Per-request; self-correcting |
| Scope-out (durable) | **Inside admit fence**, not “durable refuse.” Either human/gate signs removal, **or** low-rate re-examination probe so scoped-out domains stay falsifiable. No permanent freeze without a way back. |

## Frozen-interval trap (scope-out)

Once scoped out, zero outcomes → evidence to reverse never arrives. Same trap as explore-less refuse, but structural.  
**Rule:** scope-out ⊆ structural change ⊆ admit bar + optional probe schedule.

## Reroute trap

Fallback chains that skip target LCB smuggle untrusted claims.  
**Rule:** `reroute(target)` ⇒ `seal_trust_decide(target) ∈ {ALLOW, EXPLORE}` or target is explicitly Tier-C residual (uncertified by law).

## Naming

- Prefer **certified path / residual path / trust arbiter** over hemisphere metaphors.
- Autonomy here means **evolution of trust** and **gated structure from measured failure**, not uncertified self-edit.

## Gate sketch (when built)

`make distrust_loop` → `DISTRUST_LOOP_PASS` covering:

1. distrust → refuse (already seal_trust)
2. reroute blocked when target LCB cold
3. scope-out requires admit-shaped approval stub + probe hook present
4. journal kinds on every edge
5. **Synthetic drift harness (required for timing, not only wiring):**
   deliberately corrupt a domain outcome stream on a schedule (swap input
   distribution, inject label noise, and/or alias a subset). Assert:
   - trust LCB decays within **N** outcomes after corruption starts
   - the intended structural response fires (refuse → reroute/propose/escalate/scope-out)
   - **scope-out reverses when corruption stops** (re-exam probe is not decorative)

   Too-slow decay = hole; too-fast decay = self-eviction of everything.
   Synthetic drift is weaker than field drift but is the only way to bench
   *timing* hermetically. Field drift remains a separate, later claim.

Do not add claims rows until that gate exists and `make claims` sees the log.

## Implementation

- `include/cnet_distrust.h`, `src/cnet_distrust.c`
- Gate: `tests/test_cnet_distrust_loop.c` → `DISTRUST_LOOP_PASS` + `AUTONOMY_TICK_PASS`
- Reroute law + scope-out admit/probe + synthetic drift + miss→goal→admit teach path
