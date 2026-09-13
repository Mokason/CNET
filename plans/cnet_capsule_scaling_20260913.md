# Capsule scaling decision — 2026-09-13

The follow-up supports explicit bounded composition through 32 independent
certified capsule cores. Warm full structured requests meet p95 <= 1 ms and
p99 <= 5 ms on the measured host. Four-core residency with a 31–32-core
working set fails the 5 ms ceiling through repeated reloads. Keep required
working sets warm when the byte budget permits; otherwise account for reload
latency in admission. Core count is not a general RAM budget.

The same result does not establish exponential answer-quality improvement.
Enumerated programs double with depth; distinct finite-domain behaviors
stop doubling at depth six in the measured two-module family. Complete
natural-language answers collapse as more exposed component questions are
required to pass. The individual branch coverage bottleneck remains.

Production integration remains WITHHELD. The explicit graph and checked
sum-mod-64 adapter are experimental tools outside the capsule admission
contract. No teacher is invoked at structured serving time. There is no
automatic semantic decomposition or transformer comparison in this gate.

Independent subquestions can eventually expose separately verified partial
answers without treating the whole request as complete. Dependent outputs
must continue to refuse when any required predecessor fails. That policy
needs a separate implementation and held-out evaluation; no guard or floor
is weakened by this decision.

Evidence: [scaling experiment](../result/cnet_capsule_scaling_20260913.md).
