# Operating the CNET/Marble service stack

CNET services and optional Hermes clients have separate lifecycles. This document
describes repository tooling, not a claim that a particular service is currently
running. Paths, ports and enabled timers are deployment-specific.

## Inspect before changing anything

```sh
scripts/cnet_marble_24_7.sh status
scripts/cnet_marble_24_7.sh doctor
bash scripts/check_deployed_learning.sh
systemctl --user status cnet-marble.target
```

Read the effective unit definitions and drop-ins, selected base, capsule root,
teacher endpoint and writer coordination. An exported variable in an interactive
shell does not automatically alter the environment of an existing systemd unit.

The helper's `health` operation records a snapshot under
`logs/marble_24_7/`; log timestamps and executable generation matter.
Do not infer health from the existence of an old status file.

## Components

The umbrella can coordinate `cnetd`, the personal-AI lane, a residual/teacher
service, autoteach/governor/janitor timers, personal-AI maintenance, ROE evolution
and health snapshots. Exact membership is in
[scripts/systemd/cnet-marble.target](../scripts/systemd/cnet-marble.target)
and the install helper. Optional clients/sidecars are not certification
authorities.

Capsule curriculum and acquisition are opt-in and use owner-private paths.
Their ticks can produce new knowledge artifacts; they are not read-only health
checks. [Capsule operations](CAPSULE_CORE.md) specifies their bounds and refusal.

## Intentional lifecycle changes

`scripts/cnet_marble_24_7.sh install`, `start` and `stop` are operator
actions. Inspect the script and effective configuration before using them.
Do not use a documentation refresh, build or benchmark as authorization to
install units, enable linger, replace a base or restart services.

For a planned update: preserve the current base and sidecars, coordinate all
same-base writers, stage and verify the candidate, change only the selected
configuration, then validate service health and covered/refused socket queries.
Keep a rollback selection and preserve newly acquired evidence.

Do not restore quarantined units by disabling coverage. Atomic publication does
not make arbitrary concurrent base writers safe. The new experimental in-memory
core host is not the ordinary daemon's durable working-set deployment mechanism.

## Policy and evidence

The [autonomy charter](AUTONOMY_CHARTER.md) limits scheduled actions. Charter
permission does not prove every declared capability is implemented, and local
teacher availability does not mean a particular serving path uses it.

September deployment observations remain in
[acquisition evidence](../result/cnet_live_acquisition_20260906.md) and
[sequence evidence](../result/cnet_remaining_sequence_20260906.md).
Use them as dated records, not current process state.
[Security](SECURITY.md) and [release policy](RELEASE_POLICY.md) remain separate gates.
