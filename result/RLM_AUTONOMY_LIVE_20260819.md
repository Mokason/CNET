# RLM + AGI autonomy closeout (2026-08-19)

Status: **GATED + LIVE** — NOT AGI — broader claims WITHHELD

## What improved

### RLM outer host (multi-hop is load-bearing)

- One billed step per named CERT hop (capsule + serve-bank brick chains).
- `max_steps` trip → `rlm_budget`, `claimed_cert=0` (no prefix-CERT).
- Serve-bank `TAG n then TAG` hosted by `cnet_rlm_ask` (scenario hops on live host).
- Miss rows: `via=cnet_rlm reason=… q=… claimed_cert=0 auto_cert=false`.
- RLM never calls evolve/admit.

Gate: `make cnet_rlm` →

```text
CNET_RLM_PASS
via_rlm=1 recursive_used=1 budget_trips=1 leftover_no_prefix_cert=1
residual_never_cert=1 open_chat_answer=0 python=0 broader_claims=WITHHELD
```

### Live waist (cnetd)

- Skip single-brick short-circuit on chain turns (`cnet_rlm_is_chain_turn`).
- Chain RLM success → LOCAL CERT.
- Chain RLM abstain (`n_steps>0`) → surface refusal, **no pack fallthrough**,
  then `core_evolve_tick` (autonomy nudge without RLM self-evolve).

### Unattended evolve force-factory

- Bug: `CNET_CORE_EVOLVE_FACTORY=1` was ANDed with conf `allow_factory` and
  therefore dead when overnight conf set `allow_factory=0`.
- Fix: force env honors curriculum list even when conf disables factory;
  residual still never auto-CERTs.

Live seed after empty bank:

```text
evolve: factory curriculum built=2 ms=47.459 force=1
CNET_CORE_EVOLVE_OK luts_before=0 luts_after=2 did=1
```

## Live sock smoke (post restart)

| Query | Source | Answer / skill |
|-------|--------|----------------|
| who are you | LOCAL | soul_who |
| 2 plus 3 | LOCAL | 5 / add_u32_v1 |
| q1_add16 3 | LOCAL | 4 / dir_q1_add16 |
| q1_add16 3 then q1_xor16 | LOCAL | **5** / q1_xor16 (chain) |
| q1_add16 3 then missing_dom | CNET miss | outside_table_abstain |
| increment 41 then crc8 | LOCAL | 223 / crc8_atm |

## AGI scenario floors

| Gate | Result |
|------|--------|
| `cnet_agi_scenario` | PASS (layer1 floors) |
| `cnet_agi_scenario2` | PASS |
| `cnet_agi_scenario3` | PASS |

## Law held

- Residual / OPEN_CHAT never seals CERT.
- Teacher drafts ≠ auto-CERT.
- Prefix-CERT of partial multi-hop illegal.
- Broader AGI competence claims remain **WITHHELD**.

## Plans

- `plans/cnet_rlm_autonomy.md`
- `plans/cnet_rlm.md`
