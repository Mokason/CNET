# Continuity display

[scripts/cnet_continuity.py](../scripts/cnet_continuity.py) summarizes control
state, identity text, current query/route, inventory and consolidation hints.
It is an operator-facing state display, not consciousness or proof of task
competence.

```sh
make cnet_continuity
```

Direct script invocation writes governor diagnostics. Select a private test
governor directory before exercising it outside the gate; do not overwrite a
running service's state during a documentation check.

The display combines personality/neuromodulation values with recorded routes.
Those values may be missing or stale. Continuity output is not a promotion
input, and zero model tokens does not make it certified.
See [control signals](NEUROMOD_PERSONALITY.md) and [trace semantics](THOUGHT_PROCESS.md).
