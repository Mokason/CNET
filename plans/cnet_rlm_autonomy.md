# RLM hosts multi-hop CERT; unused budget is a law

Status: **GATED — NOT AGI — AUTONOMOUS EVOLVE UNCHANGED**

Date: 2026-08-18.

## Decision

RLM is the live outer host. A step budget that never trips is a vanity
metric (AGENTS.md #3). Multi-hop CERT turns now bill one RLM step per
named hop. Leftover named hops when `max_steps` is exhausted return
`rlm_budget` with `claimed_cert=0` — prefix-CERT of a partial chain is
illegal.

AGI-scenario compose/chain competence is hosted by the same `cnet_rlm_ask`
path (serve-bank `TAG n then TAG`), not by a new in-process layer-4
episode. Layers 1–3 stay as regression detectors of their scripted
fixtures. Broader capability remains **WITHHELD**.

Autonomy is unchanged in the evolve direction: RLM never calls evolve,
never admits, never self-CERTs. On budget/brick abstain it appends a
`via=cnet_rlm` miss row (`claimed_cert=0`, `auto_cert=false`) so the
existing unattended evolve tick can harvest later.

## Law

| Allowed | Forbidden |
|---------|-----------|
| One billed step per named CERT hop | Prefix-CERT when hops remain |
| Serve-bank chain when the first tag is already live | RLM calling evolve / admit |
| `rlm_budget` abstain + typed miss | Residual / OPEN_CHAT as a hop answer |
| cnetd skip of single-brick serve on chain turns | Filename-only join of a second registry |

## Gate

```text
make cnet_rlm
CNET_RLM_PASS
via_rlm=1 recursive_used=1 budget_trips=1 leftover_no_prefix_cert=1
residual_never_cert=1 open_chat_answer=0 python=0
broader_claims=WITHHELD
```

Makefile also refuses `cnet_core_evolve` / `cnet_agi_scenario` inside
`src/cnet_rlm.c`.

## Live waist (cnetd)

- Single-brick serve short-circuits only when `!cnet_rlm_is_chain_turn(q)`.
- Chain RLM success → LOCAL CERT (existing bind path).
- Chain RLM abstain with `n_steps>0` → surface refusal (`rlm_budget` /
  brick miss), **no pack fallthrough**, then `core_evolve_tick` (RLM never
  evolves itself).
- Miss rows: `via=cnet_rlm reason=… q=… claimed_cert=0 auto_cert=false`.
- Process-local `CnetRlmSession`: CERT hop memory; `"again"` recalls last
  CERT value without residual. Ember residual draft opt-in only
  (`allow_ember_draft`, default 0). See `plans/cnet_ember.md`.

## Not done (WITHHELD)

- Sealed-digest join of `.lut` bricks into skill-lane capsules
  (`cnb_export_subset` + operation-digest refuse). Portable knowledge
  stays the existing capsule door.
- Replay of the full layer-1/2/3 scripts over a cnetd socket.
- Any general-capability claim.

## Alternatives rejected

- New AGI scenario layer 4 script — would keep the two stacks unjoined.
- RLM calling evolve and retrying — couples the host to unattended
  growth and hides harvest in the ask path.
- Better keyword plan synthesis in layer 3 — does not make `max_steps`
  load-bearing.
