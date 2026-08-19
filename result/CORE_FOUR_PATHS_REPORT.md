# CORE four paths — deep implementation report

Generated: 2026-08-17T00:50:30

## Philosophy

AGI-like CORE = understand **given** info + determine value (prove vs abstain).
Not all-human-knowledge. Not LLM parrot mouth.

## Path benchmarks (separate)

### 1 Live waist
```
PATH1_WAIST PASS cert=1 abstain=1 open_chat_block=1 roe_block=1 llm_block=1 ms=0.000
CNET_PATH1_WAIST_PASS
live_waist=1 cert_or_abstain=1 leftover_mouth=0 ms=0.000
```

### 2 Brick factory
```
PATH2_FACTORY PASS built=4/4 served=4 skip_gate=0 ms_total=95.952
  brick[0] tag=q1_add16 ms=24.657
  brick[1] tag=q1_xor16 ms=23.780
  brick[2] tag=q1_add16v ms=23.754
  brick[3] tag=q1_xor16g ms=23.761
CNET_PATH2_FACTORY_PASS
brick_factory=1 n_plus_one_after_serve=1 teacher_gone=1 bricks=4 ms=95.952
```

### 3 Miss→admit
```
PATH3_MISSADMIT PASS propose_rc=-1 proposed=0 table=1 admit=1 serve=1 admit_delta=0 ms_propose=0.120 ms_table=0.064 ms_total=0.184
CNET_PATH3_MISSADMIT_PASS
miss_to_admit=1 propose_neq_admit=1 table_ge_0.95=1 serve_no_llm=1 ms=0.184
```

### 4 Split/compose
```
PATH4_COMPOSE PASS split=1 compose=1 child_a=1 child_b=1 composed=1 n1_block=1 ms_split=0.073 ms_compose=0.024 ms_total=24.017
CNET_PATH4_COMPOSE_PASS
split=1 compose=1 n_plus_one_gate=1 ms=24.017
```

### Bus multi-brick
```
== cnet_core_bus mini stack (multi-brick, no leftover mouth) ==
  default kills OPEN_CHAT leftover                             PASS
  hemi abstains creative leftover                              PASS
  hook never surfaces                                          PASS
  RLM kills OPEN_CHAT answers                                  PASS
  Bonsai-8B.gguf present                                       PASS
  brick1 LEASE→TABLE→CERTIFY→park                        PASS
  brick1 spec≥0.95 teacher gone                              PASS
  table door does not auto-CERT                                PASS
  brick1 parked; bus IDLE for next domain                      PASS
  brick1 RESULT×16 without LLM                                PASS
  brick2 LEASE→TABLE→CERTIFY→park                        PASS
  brick2 spec≥0.95 teacher gone                              PASS
  two bricks installed                                         PASS
  brick2 RESULT×16 without LLM                                PASS
  brick1 still serves after brick2                             PASS
  outside table → abstain                                    PASS
  leftover mouths counted blocked                              PASS
  miss-log A                                                   PASS
  miss-log B                                                   PASS
  misslog e-graph propose                                      PASS
  propose ≠ admit                                            PASS
CNET_CORE_BUS_PASS checks=21 fails=0
verbs=lease,table,certify,result open_chat_answer=0 residual_auto_cert=0 bonsai_q1_brick=1 brick2=1 teacher_gone=1 outside_table_abstain=1 misslog_egraph_propose=1 python=0 broader_claims=WITHHELD
```

## Live waist (cnetd)

- After RLM CERT miss → **outside_table_abstain** (no ROE/LLM answer)
- OPEN_CHAT answers killed
- teacher_on_miss = 0

## How to re-run
```bash
make cnet_path1_waist
make cnet_path2_factory   # needs Bonsai GGUF + CNET_GGUF_MMAP=1
make cnet_path3_missadmit
make cnet_path4_compose
make cnet_core_paths      # all + bus
```
