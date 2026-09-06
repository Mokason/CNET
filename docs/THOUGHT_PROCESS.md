# Control-state trace

[scripts/cnet_thought_process.py](../scripts/cnet_thought_process.py) formats
an operational sequence from query, route and governor state: observe,
control signals, intent rule, route, proposed action, checks and consolidation.

`make cnet_thought` tests the helper. Direct invocation persists diagnostics;
use a private governor directory when experimenting.
`ROE_NO_THOUGHT=1` suppresses trace chatter in consumers that honor it.

The trace skeleton uses no language-model tokens. That describes its
implementation cost, not the quality or certification of its contents.
Route snapshots can be stale, intent is rule-derived, and a displayed action
or verification checklist is not evidence the action or verifier executed.

The native multi-hop trace is documented separately in
[CHAIN_OF_THOUGHT.md](CHAIN_OF_THOUGHT.md). Neither trace grants a promotion
right or supplies independent training labels.
