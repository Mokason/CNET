# Autonomy charter — how much freedom

Single file: `config/autonomy_charter.yaml`  
Enforcer: `scripts/cnet_autonomy_charter.py`

## Tiers

| Tier | Examples |
|------|----------|
| **always_free** | route, LOCAL answer, thought, neuromod, miss log |
| **policy_free** | teacher on miss, gold/reviewer promote, personal pack grow |
| **never_free** | self-CERT, soul edit, lower floors, claim consciousness |

## Budgets (defaults)

| Cap | Default |
|-----|--------:|
| promotes / day | 30 |
| promotes / tick | 10 |
| teacher calls / hour | 24 |
| teacher calls / tick | 8 |
| probes / tick | 40 |

## Commands

```bash
make cnet_autonomy_charter
python3 scripts/cnet_autonomy_charter.py --show
python3 scripts/cnet_autonomy_charter.py --check promote
python3 scripts/cnet_autonomy_charter.py --begin-tick
```

Counters: `logs/governor/autonomy_counters.json`  
State: `logs/governor/autonomy_state.json`

## Enforcement

- **Autonomous cycle** begins with charter tick; caps probes/teacher; consume promote budget after evolve  
- **Evolve tick** skips promote when day/tick cap hit (`charter_promote_budget`)  
- Env hints: `ROE_EVOLVE_REVIEWER=1`, `ROE_EVOLVE_MAX_PROMOTES` ≤ charter  

Tune freedom by editing the YAML only — no code change required for dial-in.
