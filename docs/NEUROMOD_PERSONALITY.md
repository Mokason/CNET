# Neuromod personality (biological metaphor)

Not AGI. Not consciousness. **Homeostatic control signals** that bias
*when* Autonomous-ASI reinforces, sorts, or consolidates.

| Channel | Biology metaphor | System meaning |
|---------|------------------|----------------|
| **Dopamine (DA)** | reward prediction | Task done right → LOCAL/promote; **brief front-door prefer-LOCAL** (TTL ~10m, weight≈1.75, live teacher off) |
| **Serotonin (5HT)** | control vs impulse | **High 5HT = high control** (sort queue, pin memory). **Low 5HT = high impulsivity** (faster teacher, boldness, no pin) |
| **Adenosine (ADO)** | sleep pressure | Consolidate WM; **pause grow/MISS probes** one cycle |

## Tight coupling files

| File | Consumer |
|------|----------|
| `logs/governor/front_door_bias.json` | `roe_front_door` — DA prefer LOCAL |
| `logs/governor/schedule_gate.json` | `cnet_autonomous_cycle` — ADO pause / 5HT modes |
| `logs/governor/neuromod_soft_bias.json` | voice / governor flags |

## Law

- Biases **schedule + consolidate only**
- **Never** seal / CERT / floor change
- Clamped `[0.15, 0.85]` + homeostasis to baseline
- Marble SOUL still owns identity prose; neuromod owns *timing*

## Files

| Path | Role |
|------|------|
| `scripts/cnet_neuromod.py` | Organ + gate |
| `logs/governor/neuromod_state.json` | Live levels + actions |
| `logs/governor/neuromod_history.jsonl` | Audit |
| `config/personality.yaml` → `neuromod:` | Thresholds / baseline |
| `scripts/governor_personality.py` | Calls neuromod each persona tick |
| `scripts/cnet_autonomous_cycle.py` | Runs neuromod after evolve |

## Actions (soft)

| High | Actions |
|------|---------|
| DA | `prefer_local_skills`, `voice_warm`, `bias_outcome_review` |
| 5HT | `sort_task_queue`, `prefer_pin_memory`, `bias_thoroughness` |
| ADO | `run_sleep_consolidate`, `compress_context`, `defer_new_explore` |

High ADO inhibits pure “celebration” and prefers consolidate.

## Commands

```bash
make cnet_neuromod
python3 scripts/cnet_neuromod.py --tick
cat logs/governor/neuromod_state.json
```

## Coupling to Autonomous-ASI

```text
autonomous cycle → KPI (local_hit, promotes, misses)
                 → neuromod update
                 → DA reinforce / 5HT queue / ADO sleep request
                 → next cycle schedule bias
```

Still **Artificial Specialized Intelligence** with a body-like clock —
not a general mind.
