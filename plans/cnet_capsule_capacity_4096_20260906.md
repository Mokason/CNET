# Increase certified capsule inventory capacity

Status: implemented and verified in an isolated build; live rollout pending.
Evidence: `result/cnet_capsule_capacity_4096_20260906.md`.

## Decision

Raise the private core limit from 256 to 4,096 after the operator noted that
the measured 5.8 MiB inventory was small relative to 96 GB RAM. Preserve the
public 64-record HybridAi ABI, existing capsule format and certification floors.
This is inventory capacity, not a claim of broader competence.

Use private coverage-owner lookup and sorted direct-contract comparison to
avoid unrelated scans. Keep registry order and route tie order unchanged.
Audit each selected entry through the canonical certificate auditor before
every execution, including sealed-label replay. Demotion must immediately
refuse and subsequent replay must tolerate freed certification labels.

Whole-inventory coverage-label work and growth-replay work scale in 256-unit
increments from two million to 32 million charges. Growth's cooperative
deadline likewise scales from two to 32 seconds: a full proof performs more
work at larger counts. Dense direct-contract comparison remains bounded by
two million charges. Individual searches retain 65,536 charges, 1,024 states,
eight hops and a two-second cooperative deadline. No certification floor changes.

## Verification contract

- Actual RED on the old runtime at 258 capsules (`capsule_count_limit`).
- Full 4,096 canonical synthetic exports, admission, guarded self-replay with
  exactly 393,216 obligations, 196,608 correct covered direct/composed queries
  and 6,144 sampled OOD refusals; refuse 4,097 and retain the old snapshot.
- Ordinary final publication 4,095 → 4,096, with existing historical gate.
- Selected-weight mutation refusal and repeated self-replay after demotion;
  focused address/undefined-behavior instrumentation.
- Existing conflict, history, coverage, route, acquisition and budget gates;
  full `make verify` on the final isolated build.

The fixture relabels two externally supplied XOR tables into isolated chains.
It does not prove independent acquisition of 4,096 skills, rich source assets,
or a full 4,096-step growing-publication run. RAM measurements are fixture-
specific allocated heap, not a fixed bound on all future capsule sizes.

## Remaining scope

Resident snapshots, explicit hot-swap control, durable activation/rollback,
local-source evidence and a configurable RAM admission budget remain in the
lifecycle plan. Current daemon behavior is whole-directory reload per query.
No live service restart or capsule-store mutation is part of this capacity
verification. Keep tested artifacts separate from deployed binaries.
