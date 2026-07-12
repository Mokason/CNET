# One Dispatch Story

"Which specialist runs?" has exactly one answer in CNET, told in three
layers. Each layer has a stated authority boundary; none may borrow
another's. This document is the deliberate policy the July 2026
unification analysis asked for (its item 5), and `make dispatch_story`
is the gate that keeps the boundaries machine-checked.

| Layer | Mechanism | Granularity | Authority |
|---|---|---|---|
| 1 — Recall | `cce_router` (SSMax over branch centroid similarity + goodness) | INSIDE one Specialist: which branch of a CCE model's forest answers | None. Recall proposes an internal path; the Specialist's contract certifies the ROUTED composite, so an internal misroute is a certification failure, never a silent behavior change |
| 2 — Synthesis | `route_plan` / `dag_plan` / `dag_plan_circuit` | ACROSS Specialists: which certified units compose into the plan | Sole composition authority. Candidates are certified-only under `require_certified`; ranking is learned Laplace reliability (cost as a strict tiebreak under `CNET_POWER_LOW`); lifecycle exclusions (RESET, shadows) apply at one chokepoint; expansion is beam-limited (documented in *Planner scaling*) |
| 3 — Policy | application orchestrators: `perceptual_query`, `soul_request`, `gap_lane_execute` | ABOVE the planner: what to ask for, when to abstain, where a miss goes | None over validity. Policy decides the request and the failure path (abstain, gap inbox); execution happens only through the planner/executors, which validate and canonicalize every handoff |

## Why the layering is deliberate

Recall and synthesis are not competing dispatchers — they answer at
different granularities. A CCE model's internal router is invisible to
the planner on purpose: the Specialist type (see `include/specialist.h`)
makes the whole routed model ONE planner node, and the heterogeneous-plan
gate proves the planner composes it like any other unit. Pulling branch
recall up into the planner would dissolve the specialist boundary the
whole architecture is built on; pushing planning down into the router
would put composition behind an uncertified similarity score.

The layer-3 orchestrators are policy, not dispatch. `perceptual_query`
(the habitat's sub-contract chooser) predates concept ports (6A); its
hand-coded scoring is the part the planner should subsume as concept
ports mature — that remains the direction, and until then its boundary
is the same as every layer-3 citizen's: it may choose what to request
and how to present abstention, and it may not execute anything except
through the strict machinery.

## The invariants the gate pins (`make dispatch_story`)

1. **Recall stays inside the contract.** A two-branch CCE model routes
   per input (distinct centroids, distinct behaviors); the planner sees
   one certified unit whose exemplar table captures the routed composite,
   and strict execution replays it exactly. The router chose; the
   contract answered for the choice.
2. **Certification outranks everything.** An uncertified candidate is
   invisible to the planner regardless of accrued evidence; demoting the
   preferred unit (RESET) re-routes to the certified alternative;
   restoring it restores the preference.
3. **Among the certified, reliability ranks.** Flip the evidence and the
   plan flips with it — the planner's ranking is learned, not configured.

*Related reading:* `include/specialist.h` (the one type the layers
dispatch over), *The Loop* and *Routing* in the README, and the CNET-D
section for the no-authority discipline layer 3 inherits.
