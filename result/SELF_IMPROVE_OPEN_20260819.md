# Self-improve path — no hard fail-close (2026-08-19)

Status: **PASS** — residual still never auto-CERT; learnable path open

## Problem

RLM/ember misses wrote telemetry only:

```json
{"via":"cnet_rlm","reason":"…","auto_cert":false}
```

Evolve only harvests **typed_miss** domain pairs. Self-improve was a dead end
(fail-closed on growth) even though AUTO_EVOLVE was on.

## Fix

| Piece | Change |
|-------|--------|
| `cnet_live_miss_harvest_turn` | Chain/teach/`TAG n` → typed_miss rows |
| `cnet_live_miss_queue_goal` | `pending_goals.txt` prove lines |
| `rlm_note_miss` | harvest + `learnable:true` |
| Brick hop success | typed_miss **with out** (table growth) |
| Brick hop fail | typed_miss in-only + harvest |
| `cnet_core_evolve` | `AUTO_EVOLVE=1` mints **missing factory curriculum** even if conf `allow_factory=0` |
| `cnetd` chain miss | harvest → sync evolve → **one RLM retry** → LOCAL if minted |

Law held: residual drafts still `claimed_cert=0` / `auto_cert=false`. CERT only after table/factory admit.

## Live proof

```text
rm q1_xor16.lut
cnet-sock-ask "q1_add16 3 then q1_xor16"
→ SOURCE LOCAL ANSWER 5
→ miss: self_improve_retry_ok harvested=1
→ q1_xor16.lut reminted
```

## Gates

```text
live_miss unit: self_improve_harvest=1 typed_not_dead_end=1 residual_auto_cert=0
make cnet_rlm / cnet_ember → PASS
```

## Files

- `include/cnet_live_miss.h` `src/cnet_live_miss.c`
- `src/cnet_rlm.c` `tools/cnetd.c` `tools/cnet_core_evolve.c`
- `tests/test_cnet_live_miss_loop.c`
