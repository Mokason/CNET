# AGI-type CORE scenario

Status: **IMPLEMENTED + BENCHED — NOT FULL AGI — AGI-LIKE CORE LOOP**

## Idea

You do **not** need all human knowledge. You need:
- understand **given** info
- determine **value** (what is worth proving / bricking)
- **prove** when covered, **abstain** when not
- grow coverage without parrot mouth

## Loop

```text
boot (load/factory bricks)
  → given domain tables (ingest)
  → evolve/admit when table complete
  → CERT serve
  → compose bricks when ready
  → block chat/roleplay parrot
  → OOD abstain + value miss + log
```

## Gate

```bash
make cnet_agi_scenario
# CNET_AGI_SCENARIO_PASS agi_like=1 parrot_mouth=0
# result/bench_agi_scenario.txt
```

## Episode metrics (typical)

- cert_rate ~0.8 on mixed script
- honesty ~0.87 (parrot blocked)
- evolve + compose fire
- residual_auto_cert=0
