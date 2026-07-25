# CNET self-direction governor (2026-07-25)

## Purpose

**Autonomy of agenda**, not queue polling.

```
charter (human) + scoreboard (live)
    → pick 1–3 goals
    → emit actions into existing muscles
    → lane / Bonsai / PEFT / mine execute
```

## Files

| Path | Role |
|---|---|
| `config/cnet_governor_charter.yaml` | Standing goals (edit this) |
| `scripts/cnet_governor.py` | Collector + policy + emitters |
| `logs/governor/scoreboard.json` | Last live metrics |
| `logs/governor/last_decision.json` | Last goals + action results |
| `logs/governor/state.json` | last_peft / mine / procedure times |

## Goals (default)

0. keep teacher alive  
1. drain open gaps  
2. PEFT JTC when faults warm  
3. structure-mine residual  
4. procedure curriculum  
5. coverage curiosity when healthy-idle  
9. health hold (no thrash)

## Gate

```bash
python3 scripts/cnet_governor.py --test   # GOVERNOR_SELFTEST_PASS
python3 scripts/cnet_governor.py --dry-run
python3 scripts/cnet_governor.py          # one live cycle
make governor
```

## systemd

```bash
systemctl --user enable --now cnet-governor.timer
systemctl --user start cnet-governor.service
cat logs/governor/last_decision.json
```

## Relation to autoteach

- **Governor** = self-direction (what/why)  
- **Autoteach timer** = default muscle bundle (can still run)  
- Governor *hires* inject / cert_learn / mine rather than only waiting on a queue  

## Safety

- Charter-only goals  
- max_tasks_per_cycle=3  
- No gateway restart  
- Freeform still off; inject is ONEHOT window only  
