# Bounded improvement helpers

The native [API](../include/cnet_asi_improve.h) implements catalog selection,
abstention, routing statistics, episode records and role/effect checks.
Its implementation is [cnet_asi_improve.c](../src/selfimprove/cnet_asi_improve.c).
These are bounded mechanisms, not permission to change certification floors.

| Gate | Scope |
| --- | --- |
| `make asi_libos` | Capability lookup, prerequisites and privilege selection |
| `make asi_defer` | Residual threshold and margin abstention |
| `make asi_route` | Candidate truncation and specialization statistics |
| `make asi_episode` | Episode retention and regression counting |
| `make asi_firewall_eval` | Role/effect matrix and sequential comparison |
| `make asi_improve_all` | Combined local checks |

The [memory runtime](MEM_RUNTIME.md) calls these helpers before resolution and
records outcome feedback. FORM rejects continual regressions; episode text is
never directly served as certified knowledge. Host world masks, thresholds and
privilege declarations are inputs, not independently verified facts.

`make mem_runtime` tests that integration. `make asi_av_bakeoff` exercises a
bounded comparison; an unresolved comparison remains WITHHELD. External Unity
integration and old host timings are historical observations, not prerequisites
or current deployment guarantees. Original guides are recoverable through
[maintenance](MAINTENANCE.md).
