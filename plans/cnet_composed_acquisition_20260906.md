# Approved-tool path discovery and composed task transfer

## Scope and decisions

Extend the recorded demand-growth roadmap with acquisition of multi-step
capabilities. A request may have no direct policy entry yet be solvable through
approved integer-tool interfaces. Policy paths propose evidence acquisition;
only existing capsule certification and guarded execution grant answers.
Unapproved-domain teacher discovery and live deployment remain separate.

Use the existing native tool executable for bounded policy parsing/path search,
and the existing curriculum/capsule format for publication. Do not add a second
package, training on core predictions, arbitrary commands or network teachers.
Search states include exact tag, bit width and actual tool-produced value.
Reject inconsistent policy signatures, preserve eight-hop and fixed work caps.
Emit a complete plan only after validation succeeds; failed partial paths are
never consumed. Existing direct-policy acquisition remains supported.

## Ordered tasks

1. [x] Native policy planner: RED CLI fixtures for multi-hop, shared suffix,
   cycles, invalid policies, signature/range mismatch and overflow; implement
   bounded search; existing tool tests remain green. Files: native tool, focused
   planner source/internal declarations, shell test and Make target.
2. [x] Existing worker integration: RED real socket miss-to-three-hop proof;
   publish ordinary per-edge jobs within the existing two-new-job tick budget.
   Retain original demand until all jobs are durable; skip exact existing jobs
   without consuming publication budget so later hops progress. Certification
   stays in the existing curriculum. Test interruption/retry and queue refusal.
3. [x] Transfer gate: evaluate complete task pairs never supplied as direct
   training rows; verify new composed inputs after targeted coverage expansion,
   old answers, restart, and OOD refusal. Distinguish held-out composed tasks
   from unseen primitive inputs and report actual tool work.
4. [x] Review and integration: resolve adversarial findings; run focused gates,
   full isolated verify/capability/knowledge checks, document usage and exact
   remaining limits. Do not commit unrelated dirty-worktree changes.

## Trust and failure cases

Policy is owner-approved local authority, not a teacher supplied by a request.
Policy requests unreachable within the supported eight hops terminate loudly
to free spool capacity;
malformed policy, computation failure, exhaustion and full job queues preserve
retryable demand. Per-edge evidence uses external tool results only. Partially
acquired capsules may remain useful, but no completed end-to-end answer is
claimed before serving verifies every hop. New jobs retain policy/tool hashes.
Evidence ranges are intersected with representable outputs, retaining the
requested input; overflow in a neighboring block row cannot cause retry loops.
All evidence invocations, including failures and existing-job verification,
consume a total 2,048-call tick budget. The planner's native oracle/search work
is separately bounded and reported; stdout flush failure refuses its plan.

No whole-inventory latency or power-loss guarantee follows from this feature.
Those remaining gates must be measured separately, not inferred from task count.

Completed: [measured evidence and remaining gates](../result/cnet_composed_acquisition_20260906.md).
