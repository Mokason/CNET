# CORE bus — four verbs only (product mini stack)

Status: **LIVE GATE — NOT A MIND**

```
LEASE → TABLE → CERTIFY → RESULT
```

Creative emits tables only. Logical proves or abstains. Outside a table → abstain.  
OPEN_CHAT / ROE leftover answers are killed.

## Multi-brick

1. Brick N must **park** (teacher gone, RESULT×16 without LLM)
2. Only then brick N+1 LEASE starts
3. Bank holds up to 4 CERT bricks (`q1_add16`, `q1_xor16`, …)

## First two Bonsai Q1_0 bricks

| Brick | Tensor | Mode | Tag |
|-------|--------|------|-----|
| 1 | `blk.0.attn_q.weight` | add bit | `q1_add16` |
| 2 | `blk.0.attn_k.weight` | xor bit | `q1_xor16` |

## miss-log → e-graph

`cnet_core_bus_misslog_propose` — propose only; admit = `specialist_admit` after TABLE.

## Live

```bash
export CNET_CORE_BUS_BRICKS_DIR=/path/to/bricks
export CNET_BONSAI_GGUF=/path/to/Bonsai-8B.gguf
# cnetd loads two bricks at start if env set
```

## Gate

```text
make cnet_core_bus
# open_chat_answer=0 residual_auto_cert=0 bonsai_q1_brick=1 brick2=1
```
