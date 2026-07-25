# CNET self-direction governor (native C)

## Purpose

**Autonomy of agenda**, not queue polling.

```
charter (human) + evolving scoreboard
    → pick ≤3 goals
    → hire muscles (lane / Bonsai / PEFT / mine)
```

## Binary

```bash
make governor_build          # bin/cnet_governor
make governor                # GOVERNOR_PASS
./bin/cnet_governor          # one cycle
./bin/cnet_governor --daemon 900
```

Python `scripts/cnet_governor.py` is legacy prototype only.

## systemd

- `cnet-governor.timer` → oneshot `bin/cnet_governor` every 15m (default)
- `cnet-governor-daemon.service` → optional always-on `--daemon 900`

## Artifacts (`logs/governor/`)

scoreboard.json, history.jsonl, ewma.json, last_decision.json, state.json  
All tagged `"engine": "cnet_governor_c"`.
