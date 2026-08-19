# RLM — Recursive Loop Module (outer host)

Status: **GATED — WRAPS CORE + BOTH PLANES — STEP BUDGET IS LOAD-BEARING — NOT AGI**

## Picture

```
user user turn
           │
           ▼
   ┌───────────────┐
   │      RLM      │  outer host (bounded steps)
   │  via_rlm = 1  │  recursive re-entry budget
   └───────┬───────┘
           │ always through CORE law
           ▼
   ┌───────────────┐
   │     CORE      │  middle ground / discern
   └───────┬───────┘
      ┌────┴────┐
      ▼         ▼
   CERT      OPEN_CHAT
  logic     creativity
```

RLM does **not** replace CORE. It **hosts** CORE and may re-enter it
(multi-skill capsule_loop, then core_ask) within `max_steps`.

## Law

- Every bind is still a CORE result (`via_core=1` on final).
- `claimed_cert` only on CERT plane.
- Open chat drafts never auto-CERT.
- Not RLM-in-WordLM. Not DeepSeek REPL steal. Not open-ended agent spin.

## API

```c
#include "cnet_rlm.h"

CnetRlmPolicy p;
CnetRlmResult r;
cnet_rlm_policy_default(&p);
cnet_rlm_ask("what is 2 plus 3", &p, &r);
/* r.via_rlm=1 r.final.plane=CERT r.final.claimed_cert=1 */

cnet_rlm_ask("write a haiku…", &p, &r);
/* r.final.plane=OPEN_CHAT r.final.claimed_cert=0 */
```

Multi-hop CERT turns bill one step per named hop (capsule calls unrolled;
live serve-bank `TAG n then TAG` hosted here). Leftover hops when the
budget is exhausted return `rlm_budget` with `claimed_cert=0`. RLM never
evolves; it may append a `via=cnet_rlm` miss row. Decision:
`plans/cnet_rlm_autonomy.md`.

## Gate

```text
make cnet_rlm
CNET_RLM_PASS
via_rlm=1 wraps_core=1 residual_never_cert=1 recursive_bounded=1
recursive_used=1 budget_trips=1 leftover_no_prefix_cert=1
```
