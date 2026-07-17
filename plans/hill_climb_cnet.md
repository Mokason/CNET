# CNET local hill-climb (EG)

**Inspired by:** Microsoft MAI-Thinking-1 *Building a Hill-Climbing Machine*
(capabilities climb via a measurable system, not one-off model releases).

**CNET translation:** the personal-AI loop is the machine. Local EG measures
whether **teacher work** converts into **useful sealed skills**.

## Metric

| Symbol | Meaning |
|--------|---------|
| `teacher_work` | Sum of `drained`/`examined` on busy ticks |
| `seals` | Sum of `closed` (gaps sealed into CNB) |
| `cost_per_seal` | `teacher_work / max(seals, 1)` — **lower better** |
| `baseline_cost` | Default **32** (≈ one min_evidence teach batch) |
| `local_eg` | `baseline_cost / cost_per_seal` — **>1 = climbing** |
| `seal_rate` | `seals / hours` |

Do **not** promote hermetic `hybrid_bench` 100% to “product learned English.”
EG tracks the **live seal factory**.

## Telemetry

- Each busy `gap_lane_run` tick appends JSONL to `<base>.hill_climb.jsonl`
- Curiosity proposals included when present
- Report: `scripts/personal_ai_hill_climb_report.sh [base] [days]`
- CLI: `make cnet_eg_cli` → `bin/cnet_eg report|compute|log`
- Gate: `make eg` → `EG_PASS`

## Env

| Env | Default | Meaning |
|-----|---------|---------|
| `CNET_EG_BASELINE_COST` | 32 | Baseline examined/seal |
| `CNET_EG_LOG` | `<base>.hill_climb.jsonl` | Event log path |

## Climbing rules

1. **EG > 1** and rising → keep recipe (teachable seeds, curiosity, closures).
2. **EG < 1** with seals → more work per seal (harder gaps / deferred noise).
3. **seals = 0** → feed teachable inbox or wait for curiosity; not a quality fail.
4. Never lower certify bars to improve EG.
