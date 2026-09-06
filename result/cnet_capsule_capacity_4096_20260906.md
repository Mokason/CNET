# Certified capsule capacity: 4,096

Implemented and verified in source and the isolated build
`/tmp/cnet-capacity-final-9d5jz5`. Not deployed: the live daemon and capsule
store were left untouched. This report does not claim resident hot-swapping.

## Result

The private inventory ceiling is now 4,096 instead of 256. The public
HybridAi ABI, capsule packaging, certification floors, eight-hop limit,
1,024-state limit and per-query work limit are unchanged.

The full synthetic fixture passed:

- 4,096 canonically exported/imported capsules, arranged as 2,048 XOR chains.
- 393,216 sealed-label obligations checked by public growth validation.
- 196,608 correct covered direct/composed queries; 6,144 sampled OOD refusals.
- Capsule 4,097 refused; the already-loaded snapshot remained usable.
- Ordinary publication from 4,095 to 4,096 passed historical replay with
  393,152 obligations, then the final composed query returned the expected 11.

A separate three-repeat probe passed 589,824 covered answers and 18,435
expected OOD/unknown-interface refusals, with zero wrong verified answers,
unexpected abstentions or invalid results. Resident p95 was 0.033583 ms;
whole-directory reload-and-query p95 was 1,209.000870 ms. These are local,
serial CPU fixture measurements, not daemon concurrency or an SLA.

## Memory

Three runs after identical one-capsule warmup, close/cache reset and allocator
trim reported identical allocated-heap deltas:

| State | Additional allocated heap | Additional RSS |
|---|---:|---:|
| One 4,096-capsule snapshot | 97,077,152 bytes (92.58 MiB) | 97,710,080 bytes |
| Two snapshots after replay | 194,159,568 bytes (185.17 MiB) | 195,002,368 bytes |

The standalone capacity run loaded in 1.191616 seconds and completed full
replay in 2.954553 seconds. The full query sweep took 5.970300 seconds.
Capsule sizes depend on weights, labels and coverage: these small numeric
fixtures do not establish a universal per-capsule footprint. This excludes
the daemon's host base, teachers and other process state.

## Implementation and safety

Private sorted coverage-owner lookup avoids scanning unrelated banks.
Direct-contract comparison sorts auxiliary pointers, independently validates
input/output tag signatures, and compares overlapping labels only for matching
interface pairs. Registry order and routing tie order remain unchanged.

Each selected entry is audited through the existing canonical certificate
auditor immediately before execution, including every replayed hop. This
replaces hashing every unrelated capsule on every ask. Audit demotion returns
immediately; subsequent replay refuses missing certificate labels safely.

Inventory-wide coverage-label and replay work scale from two million to
32 million operations in 256-capsule increments. Replay's cooperative deadline
scales from two to 32 seconds; query and other admission-phase deadlines remain
two seconds. Direct-contract comparison keeps the two-million-operation bound.
This increases bounded proof resources, not certification tolerances.

## Verification and evidence

- Actual old-build RED: 258 capsules refused with `capsule_count_limit`.
- Actual selected-audit RED: changed weights still executed under stale flags.
- Selected audit and self-replay demotion now pass, including a repeated call
  after labels are freed. Address/undefined-behavior instrumentation on the
  focused core/test passes; the linked dependency library was not instrumented.
- Final `make verify`: all 28 suites report fresh success. The authority gate
  includes existing conflict, history, routing, acquisition and coverage gates,
  plus the new selected-audit regression.
- Additional gates pass: 32-unit knowledge accumulation (32/32 isolation,
  8/8 replay, 96 OOD refusals), three-member every-hop composition,
  coverage-abstention (55 checks), and the extended budget/index regression.
- Capability certification passes 6/6, run
  `6076bfe4-1e71-4f56-9bda-511f339468ab`: classification 0.861, JSON tool-call
  adapter 0.775, and the four named held-out fixtures at 1.000. These scores
  retain their existing fixture scope; they do not measure 4,096 domains.
- Reproducible opt-in gate: `make capsule_large_inventory_bench`.
- Smaller boundary gate: `CNET_LARGE_CAPSULES=258 make capsule_large_inventory_bench`.

Logs, JSON measurements and source/build digests are retained in
`result/cnet_capsule_capacity_evidence_20260906/`. The full canonical fixture
is `/tmp/cnet-large-inventory-0mI2NF/capsules`; the fixed-binary measurement
directory is `/tmp/cnet-capacity-memory-zf6NLp`.

Fresh-context review identified and resolved independent output-tag checking,
guard-index lifetime checks, self-replay aliasing tests, incorrect benchmark
stdout/stderr marker selection and missing exact obligation-count assertions.
External cross-model review was offered but not authorized or run. Kernel
performance-event access was unavailable; measurements use monotonic timing
and allocator/RSS diagnostics without changing host security settings.

An initial additional capability run correctly refused evidence because the
outer output log was changing inside its checked worktree. Re-running with
that log outside the worktree passed; no gate or floor was relaxed. The failed
run is retained alongside the passing one.

## Scope still withheld

These are renamed copies of two externally labelled XOR tables, not 4,096
independently acquired domains. The benchmark does not establish a full
4,096-step sequential acquisition run, source-knowledge competence, or asset
memory scaling. Explicit resident hot-swap, configurable RAM admission limits,
durable activation and live rollout remain separate follow-up work. Current
daemon behavior is still whole-directory reload per request.
