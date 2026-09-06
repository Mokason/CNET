# Selection and execution authority

Three layers answer different questions. None may bypass the contract of the
specialist that ultimately executes.

| Layer | Decision | Authority boundary |
| --- | --- | --- |
| Recall | Which branch inside a CCE specialist to use | The routed composite still owes the specialist's contract |
| Certified planning | Which admitted specialists compose a typed task | Only eligible certified units; every handoff is checked |
| Application policy | What to request, how to present refusal, where a miss goes | No authority to convert a proposal into a certified result |

The planner uses typed compatibility, lifecycle eligibility and evidence-based
ranking. Similarity and reliability are selection inputs, not alternatives to
certification. Demoted/reset candidates cannot win by having a higher score.
Report-only telemetry and agent roles do not grant execution authority.

## Local capsule path

`cnet_capsule_core_ask` searches typed-port/actual-value states in a verified
inventory. A suffix uncovered for one value can still be usable for a different
covered value. The strict executor checks the selected capsule at each hop;
search exhaustion or uncovered intermediates abstain.

The opt-in `cnet_capsule_core_ask_cell` uses a learned graph proposal with the
same final checks. Its graph cap is 62 capsules plus two endpoints. There is no
silent fallback, and it does not reduce deterministic inventory capacity.
Candidate activation must replay actual neural execution on all old/proposed
sealed-label obligations, not merely prove the deterministic path still works.

## Verify the boundary

```sh
make dispatch_story
make capsule_value_search capsule_history_coverage
make -C experiments/offline_controller product-test
```

The first gate tests recall within a contract, certification before ranking and
reliability among eligible units. The last requires AMD ROCm and tests the
experimental activation boundary; it is not required to use deterministic
dispatch.

Contracts: [specialist.h](../include/specialist.h),
[cnet_capsule_core.h](../include/cnet_capsule_core.h),
[cnet_core_host.h](../include/cnet_core_host.h).
See [architecture](ARCHITECTURE.md) and [capsules](CAPSULE_CORE.md).
