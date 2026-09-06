# Exploration drafts

[roe_explore_tick.py](../tools/roe_explore_tick.py) proposes curriculum drafts.
It is a candidate generator, not a certification authority.

The tick consults idle/control-state limits, mutates known seed patterns and
checks bounded draft syntax/content. An optional teacher can supply drafts;
`--force` bypasses an idle gate, not certification. Neither option should be
enabled implicitly during operations.

`make roe_explore_tick` exercises the local gate. Direct tool execution writes
exploration/curriculum reports and may make teacher calls when requested.
Use private paths and inspect the selected configuration first.

A deferred tick is a valid control outcome, not a learning success. Syntax
checks do not prove program behavior, and operational keywords do not prove
a prose answer true. Drafts remain `auto_cert=false`; do not use CNET's own
Tier-A answers as new training labels. Follow
[gold review](GOLD_CURRICULUM_HARVEST.md) for independent evidence.
