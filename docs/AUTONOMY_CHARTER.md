# Scheduled-action policy

The charter is [config/autonomy_charter.yaml](../config/autonomy_charter.yaml).
[scripts/cnet_autonomy_charter.py](../scripts/cnet_autonomy_charter.py) applies
its counters and decisions at wired scheduler boundaries. The charter describes
permission, not proof that every listed action is implemented or universally
enforced by every entry point.

## Categories and budgets

Read-only/status/routing actions are separate from budgeted teacher calls and
personal-pack growth. Self-certification, lowering floors, changing policy or
soul identity, and training on Tier-A answers are not unattended actions.

The checked-in defaults permit 30 promotions/day, 10/tick, 24 teacher calls/hour,
8/tick and 40 probes/tick. Inspect effective configuration and persisted counters
before relying on available budget. Changing a ceiling is an operator policy
change, not a remedy for a failed certification gate.

## Inspection and checks

```sh
make cnet_autonomy_charter
python3 scripts/cnet_autonomy_charter.py --show
python3 scripts/cnet_autonomy_charter.py --check promote
```

`--begin-tick` changes accounting state and belongs to an intentional scheduler
tick, not a read-only inspection. State/counter/log paths are specified by the
charter. The cycle and evolution scripts must honor refusal; a configured budget
does not authorize arbitrary host commands.

[Operations](CNET_MARBLE_24_7.md), [teaching](TEACH_PATH.md), [security](SECURITY.md).
