# Personality control signals

Dopamine, serotonin and adenosine are biological names for numerical scheduling
and presentation signals. They are not evidence of feelings or awareness.

[scripts/cnet_neuromod.py](../scripts/cnet_neuromod.py) derives bounded biases
from recent outcomes and [personality configuration](../config/personality.yaml).
Consumers may prefer local routes, sort queues, pin memory, consolidate or
defer exploration. These hints cannot lower a contract floor or seal a unit.

The governor writes `neuromod_state.json`, history and downstream bias/gate
files. Readers can see stale or missing snapshots; inspect timestamps and
effective configuration when diagnosing behavior.

`make cnet_neuromod` runs the local checks. Direct `--tick` execution mutates
governor state, so use a private directory for experiments.
External teacher use remains controlled by its own integration;
a timing bias does not re-enable the daemon's disabled teacher branch.
See [autonomy policy](AUTONOMY_CHARTER.md) and [daemon behavior](CNETD.md).
