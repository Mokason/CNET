# Global Plan-Score Optimization — Design

Date: 2026-06-11
Status: Implemented 2026-06-11 (TDD; `make test` + all demos green).
Repo: G:\AI\CNET (not a git repo — written, not committed)

## Problem

Reliability ranking is per-choice greedy: each slot/type takes the locally
best alternative, first complete plan wins. Greedy misses globally better
plans — a flashy first hop can force a terrible second hop, and in
route_plan the visited-by-type mechanism structurally locks each port type
to its single best producer. Deferred as "global plan-score optimization"
since the learned-scoring design.

## Objective: maximize the product of step reliabilities

score(plan) = Π btn_reliability(p) over every primitive EXECUTION in the
plan (a primitive used twice counts twice — two forwards, two chances to
fail; sources contribute 1.0).

The product subsumes the old shortest-path preference instead of fighting
it: every reliability is strictly < 1 ((s+1)/(s+f+2)), so with uniform
evidence fewer steps always score higher — all existing fresh-stats tests
and demos keep their plans. With real evidence, a reliable detour can
legitimately beat a flaky shortcut (0.8 x 0.9 beats a 0.05 direct hop),
which is exactly the project's trust-evidence lesson. Hop count is no longer
an independent objective, just an emergent preference. Ties (equal product)
keep the first plan in the old alternative order — registry order for
fresh stats, so behavior without evidence is unchanged BY CONSTRUCTION.

## route_plan: hop-capped DP (Bellman-Ford over port types)

Linear chains over single-in/single-out primitives form a graph whose nodes
are distinct port types (start + each primitive's output; identity =
same_port_type, so two producers of one type now COMPETE instead of the
first claiming it). dist[k][t] = best product reaching type t in exactly k
steps (k <= ROUTE_MAX_STEPS), with predecessor (primitive, prev type) for
reconstruction. Updates only on strictly-greater product; k scanned
ascending; primitives iterated in registry order -> deterministic
shortest-then-registry tie-breaks. Answer = best dist over all types
port_compatible with the goal. Exact within the hop cap (no negative...
i.e. no >1 factors, so no cycles help). The BFS/visited machinery and
route_plan's use of reliability-ranked iteration are deleted (the DP
considers all edges; scores moved from the search ORDER into the OBJECTIVE).

## dag_plan: exhaustive enumeration with branch-and-bound

dag_solve (first-success) becomes dag_search: on completing the agenda it
scores the candidate, keeps a deep copy if strictly better, then BACKTRACKS
AND KEEPS ENUMERATING. Pruning: entering any obligation with
score <= best_score is dead (every further primitive multiplies by < 1;
ties keep the earlier find) — admissible, so the search stays complete.
Reliability-ranked alternative order is kept: good plans are found early,
which makes the bound prune harder. Source-state undo is the existing
backtracking invariant; the running score is passed DOWN by value (no undo
arithmetic, no FP drift). The winning tree is deep-copied (dag_clone) when
found; out->root receives the best at the end.

Cost: enumeration is exponential where first-found was linear-ish on
success; at toy registry scale with the bound it is irrelevant. Documented;
memoization remains future work if registries grow.

## Tests (TDD)

- test_router: greedy trap — flashy (0.909) -> dud (0.0909) vs steady pair
  (0.8 x 0.909); planner must return the steady chain. Evidence-beats-
  hop-count — a 1-hop direct with 1/20 reliability loses to the 0.727
  two-hop chain (this deliberately retires "shortest always wins" and pins
  its replacement). Existing fresh-stats tests pin the no-evidence
  degeneration (shortest + registry order).
- test_dag: greedy trap in a tree — slot fed by lure (0.9) whose own input
  needs stinker (0.05), vs honest (0.7) directly from the source; planner
  must pick honest. Existing structure/preference tests pin ties and
  no-evidence behavior.

## Out of scope (YAGNI)

- Failed-subgoal memoization / caching across enumeration.
- Plan-score objectives beyond the product (latency, parameter count).
- Shared sub-results (unchanged frontier; scoring counts executions, which
  stays correct if sharing lands later).
