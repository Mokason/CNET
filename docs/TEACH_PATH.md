# Evidence acquisition and teaching

Serving, collecting an uncovered request, acquiring a label and certifying a
new unit are separate steps. A miss is not a training target, and a teacher
draft is not a certified answer.

## Certified capsule path

The daemon records a bounded normalized demand only when configured to do so.
An approved integer-tool policy can supply independent labels, producing a
curriculum job. The curriculum tick fits/compiles a candidate, verifies its
contract and exact coverage, replays old/new obligations and publishes only
after the gate passes. See [CAPSULE_CORE.md](CAPSULE_CORE.md).

Queued demand, generated evidence, failed certification and successful
publication have different receipts. Never report the first as the last.

## Other learning surfaces

The gap lane and oracle runtime acquire typed evidence under their own
configured teacher identity, budgets and certification contracts. ROE's
miss/gold/reviewer workflow manages text-pack evidence; it is not the same
package or guarantee as a CNU1-sealed neural capsule.

The current daemon's legacy ROE mouth explicitly disables teacher-on-miss.
Old documentation saying `CNET_TEACHER_ON_MISS=1` alone enables that serving
branch is obsolete. The setting does not enable it in the current source.
Background teacher use and other residual/model APIs are separate integrations.

## Invariants

- No training on CNET's own Tier-A answers.
- External model output remains untrusted until the applicable independent
  evaluation and certification path accepts it.
- No automatic floor reduction, guard removal or teacher-identity substitution.
- Probes and budget-exhausted work must not silently spend teacher resources.
- Re-evaluation of sealed history is not new teaching data.

Relevant checks: `make own_learning_health`, `make gap_lane`,
`make oracle_teacher_runtime`, and the capsule acquisition/security gates.
Run model/teacher-dependent checks only with their explicitly selected
prerequisites. [Charter](AUTONOMY_CHARTER.md), [security](SECURITY.md).
