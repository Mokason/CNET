# Allocator sequence feasibility — DEVELOPMENT, 2026-09-07

**Learned gain remains WITHHELD.** The new multi-step workload creates useful
planning value, but an honest deterministic control already proves the optimum
in all 16 development episodes. Its maximum measured CPU time is 0.823 seconds,
within the prospectively fixed one-second budget. Oracle headroom against that
control is exactly zero, below the unchanged +.05 floor. No GPU fitting,
confirmation, activation or live-service change followed this result.

## Actual native outcome

| Planner | Mean verified request coverage |
|---|---:|
| Actual native rotating cursor | 0.1142578125 |
| Actual native demand/cost | 0.0781250000 |
| Actual native completion-first | 0.0781250000 |
| Recomputed exact reachable marginal/cost | 0.3085937500 |
| Full missing-path bundle-greedy | 0.3955078125 |
| One-CPU-second deterministic branch-and-bound | 0.4238281250 |
| Thirty-CPU-second reference, proved optimum | 0.4238281250 |

The reference also leaves only .0283203125 mean coverage over bundle-greedy.
Thus even excluding the exact control would not create +.05 headroom over this
cheap full-plan comparator. This is not an impossibility claim about every
future workload.

| Family, four development graphs each | Cursor | Demand/completion | Marginal | Bundle | Exact/reference |
|---|---:|---:|---:|---:|---:|
| Direct | .218750 | .210938 | .246094 | .246094 | .246094 |
| Shared-prefix | .152344 | .101563 | .535156 | .578125 | .621094 |
| Chain-completion | .058594 | .000000 | .289063 | .425781 | .453125 |
| Evidence-rejection | .027344 | .000000 | .164063 | .332031 | .375000 |

These are development measurements, not the frozen promotion gate. No paired
confirmation interval or acceptance claim is made from 16 exposed episodes.

The successful run performed 256 actual pure-tool oracle calls and 508 actual
generic native candidate builds. Across 112 isolated whole trajectories it
performed 868 canonical candidate admissions, 28 shared charged source-evidence
rejections, 157,696 independently labelled request probes and 1,161,774 canonical
growth replay obligations. Every accepted step preserved prior covered demand.
There were zero wrong verified values, planned/native reachability mismatches,
native admission refusals or original-control chooser parity mismatches.
All three original controls called the actual canonical native chooser; the
Python planner was not simply asserted equivalent.

Elapsed BOOTTIME: 43.739658 seconds. Parent CPU: 6.441089 seconds. Total child CPU:
27.946727 seconds; maximum child CPU/wall: .462180/.520137 seconds. The 876
bounded child calls ran one at a time, with no nonempty stderr, maximum stdout
214 bytes, and no deadline or pipe-cap refusal. The exact control visited 31,728
nodes total, consumed 2.801059 CPU seconds across all 16 episodes, and proved all
16 optima. Its maximum episode CPU/wall was .822702/.822717 seconds, maximum
initialization .007850 seconds, and cap overrun zero. The reference consumed
2.847162 CPU seconds total, maximum .830711 seconds, and explored the same nodes.

## What changed, and what did not

The prospectively frozen [pilot contract](../experiments/offline_controller/allocator_sequence_pilot_contract.md)
defines a **new development workload**, not a denser repair of the old holdout:
32 queued actions, eight jobs, 512 row-work units, 48–64 certified keys per
six-bit primitive, and 64 current requests. Shared and chain graphs include
overlapping multi-missing paths and alternative routes. A task pair can have
positive joint value with zero singleton value; native trajectory replay checks
this rather than unioning task-alone masks.

Every control sees the public graph, prospective exact tool domains and
transforms, demand, costs, ages and cursor. Alternative numeric paths agree by
endpoint XOR potentials. Exactly one overdue task fits every policy; in the
rejection family its actual wrong source row is rejected and charged identically.
The remainder plans are conditioned on the predeclared synthetic task-zero
rejection flag **before** its source is independently checked; execution later
validates that condition. This is a fair shared conditional comparison, not
online replanning after an observed rejection. The historical frozen contract's
"observed shared outcome" wording should be read with this phase-order
qualification; the frozen contract and its hash have not been rewritten.
All capsules start missing, so original completion features are zero and its
unchanged f0/cost tie-break honestly ties demand/cost. That initial-state
limitation is explicit. The four motifs and the four-operand finite XOR oracle
repeat; distinct graphs are not evidence of broad semantic or family coverage.

The generic compiler is an unsealed trusted experimental producer. It grants no
unattended production acquisition authority. Candidate bank reuse across
counterfactual policies is charged equally in prospective rows, not presented as
measured learning throughput. Standard capsules, canonical import/growth and the
existing per-hop coverage/certification guards are unchanged. The new Python
planner is an experiment, not a production supervisor or CORE deployment.

## RED/GREEN and execution receipts

The initial computational contract stub produced nine actual RED tests
(14 failing subcases). The implemented suite is 13/13 GREEN, including 100
randomized small problems checked against independent exhaustive subset search,
pair complementarity, per-hop transformed key coverage, shared cost, budget and
fairness, rejection charging, completion tie-break, and explicit timeout/seed
overrun reporting. A capped unfinished search is never labelled optimal.

An author-separated review reproduced the 13 tests and checked 1,000 additional
randomized interrupted searches against exhaustive enumeration, including
forced accepted/rejected actions and already-covered requests. Every checked
feasible reward was at most the optimum, which was at most the reported cutoff
upper bound. The reviewer independently reconciled coverage, receipt counts,
CPU/node totals and frozen artifact hashes. These checks support this bounded
development conclusion, not prospective online adaptation or a learned gate.

Main's final pure-Python repeat passed all 13 tests in 0.096 seconds, exit 0:
`/tmp/cnet-allocator-sequence-tests-final-20260907.log`, SHA256
`0775e3d4a88c65fe8684ad017595c9bd1de1c541993835d61627d367b102d611`.
This repeat did not rebuild or rerun the native trajectory experiment.

Focused command, from `experiments/offline_controller`:

```sh
python3 -m unittest allocator_sequence_pilot_test -v
```

The serving library does not export the allocator chooser. With approval, a
private comparator library was built from unchanged canonical sources:

```sh
cc -shared -fPIC -O2 -Iinclude src/serve/cnet_core_allocator.c src/serve/cnet_core_cell.c -lm -o /tmp/cnet-sequence-comparator-build-X0Irkp/libcnet_allocator_control.so
python3 experiments/offline_controller/allocator_sequence_pilot.py --compiler bin/cnet_capsule_core --oracle /tmp/cnet-allocator-composition-33nf3kop/bin/cnet_capsule_tool --library bin/libcnet_capsule_core.so --chooser-library /tmp/cnet-sequence-comparator-build-X0Irkp/libcnet_allocator_control.so
```

The successful root is `/tmp/cnet-allocator-sequence-ho7uv95u`. Before native
outcomes, it froze the protocol, producer/support/tests/contract, compiler,
oracle, native runtime and comparator library/source/header hashes. Each episode
retains prospective inputs and decisions, native build calls, source/capsule
identities and every step/final probe receipt. Hashes provide integrity; trusted
phase ordering and the actual calls supply experimental provenance.

- Protocol SHA256: `0114669b71ee7d652075a63d6a7db724cdcae77976c61c6e23822fc1cbb87859`
- Freeze SHA256: `ee338727a60356f33f30f058a78af5982146c51bafc66a733ea38db63f73f7f2`
- Summary SHA256: `3f9f36f1e9adb89273fdc8d01bb05798bffd975018e3a85adc6b6aa80bf50b3c`
- Producer SHA256: `64e59a4d155bbbc33df796951f63fb0ef470af8271de8942a24df3f056187126`

Two earlier harness roots are retained, not silently discarded. At
`/tmp/cnet-allocator-sequence-dqi81lo6`, the absent current oracle binary stopped
the run before any native call; the prior campaign's owner-private frozen pure
tool was then used. At `/tmp/cnet-allocator-sequence-sjy6vru_`, the missing chooser
symbol stopped the first trajectory before any step, after 256 oracle calls and
32 builds. The approved separate comparator library fixed that harness boundary.
Workload, seed, comparators and floors were unchanged; all data are development.
No material was deleted. `du -s` reports 63,672 KiB allocated for the successful
root and 3,732/40 KiB for those earlier roots; these are not logical-byte quotas.

## Smallest justified next step

The measured useful architectural change is bounded deterministic multi-step
acquisition planning with public dependency/coverage information. It is not
evidence that an allocator neural cell learned a useful improvement. Integrating
that planner would separately require sealed authoritative composition sources,
typed state/features, maintained incumbent/policy identity and the existing
durable safety gates; this pilot authorizes none of those production changes.

A future learned search guide/value-to-go objective only makes sense after a
separately frozen development workload demonstrates residual +.05 headroom
against the exact and full-plan controls under a non-starved common CPU budget.
It would need independently verified whole-trajectory return labels, explicit
graph/remaining-budget inputs and a new canonical CORE objective/feature version,
not reinterpretation of v1 singleton masks. This experiment supplies no such
residual headroom, so more epochs or a larger model are not justified here.

Future-demand prediction could be an alternative only with genuine independent
chronological request history. None was found in the scoped allocator
experiments, prior allocator report or autonomous-learning plan; invented
hidden request outcomes are not a substitute. The v1 failure, unchanged gain
floors, future fresh-confirmation obligation and all broader claims remain
WITHHELD.
