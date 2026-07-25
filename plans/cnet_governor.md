# CNET autonomous governor v3 (self-evolving)

## Single engine
`scripts/governor_autonomous.py` — systemd default.  
C `bin/cnet_governor` remains optional legacy (`make governor`).

## Weak-point fixes
| Weakness | Fix |
|---|---|
| Window-only fuel | Hermes errors.log + agent.log + miss_bus |
| Dual engines | v3 is sole systemd entry |
| Soft projects | top_project hard-biases ranking |
| Soft eval | eval_veto freezes seals on regression |
| Flashy velocity | min_dt_h; suppress rates on tiny cycles |
| Static policy | meta_evolved.json self-tunes weights |

## Self-evolution
Each cycle writes/updates `logs/governor/meta_evolved.json`:
- w_eval, w_real_miss, w_hermes_err, w_backlog
- threshold_backlog, inject_n
- eval_veto flag
- meta_history.jsonl audit trail

## Gates
```bash
make governor_v3
make governor_quality   # GOVERNOR_QUALITY_PASS
```
