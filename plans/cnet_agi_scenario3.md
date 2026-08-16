# AGI scenario layer 3

Status: **PASS — plan synthesis + multi-agent brick specialists**

## A) Open-ended goal → CERT-only plan

```text
"goal: prove q1_add16 at 3 then compose with xor"
  → synthesize SERVE/COMPOSE steps only
  → run goal
  → reject "roleplay/chat" goals (non-CERT)
```

## B) Multi-agent specialists

- Each parked domain tag = specialist
- `specialist TAG n` routes to that brick
- `committee TAG n` requires multi-agent presence + secondary CERT serve

## Gate

```bash
make cnet_agi_scenario3
# CNET_AGI_SCENARIO3_PASS layer3=1
# result/bench_agi_scenario3.txt
```

## Typical metrics

- synth_ok ≥ 2, synth_reject ≥ 1 (chat/roleplay refused)
- specialist routes + committee_ok
- parrot_mouth=0 residual_auto_cert=0
