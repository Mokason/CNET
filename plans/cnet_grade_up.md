# Grade-up campaign (C/B- → A-)

## Target areas
1. Live traffic proof of MoE/fault/store
2. Library quality
3. Ghost certified presentation
4. Margin-gated residual
5. Accounting dashboard

## Gates
```bash
make cnet_grade_up
# CNET_GRADE_UP_PASS + LIBRARY_QUALITY_PASS + ACCT_DASHBOARD_PASS + DOCTOR_PASS
```

## Measured (hermetic)
- hard expert multi-hit traffic
- fault bus filled from labeled JTC
- store save/load certified
- residual min-margin env
- auto orch install when FAULT/STORE set

## Ops
```bash
bash scripts/cnet_library_quality.sh
bash scripts/cnet_acct_dashboard.sh
```
