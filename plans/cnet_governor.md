# CNET autonomous governor

## Engines
| | |
|---|---|
| **v2 (default systemd)** | `scripts/governor_autonomous.py` |
| **C core** | `bin/cnet_governor` (`make governor`) |

## v2 capabilities (all 10 + safe web)
1. Real miss_bus (faults + waiting_oracle)
2. Independent JTC eval probe (`eval_jtc_delta`)
3. Outcome close (backlog/eval → goal_health)
4. Standing projects (`config/governor_projects.json`)
5. Resource snap (busy / allow_heavy / night)
6. Expanded muscles (eval, safe_web, research)
7. Human pins (`config/governor_pins.yaml`)
8. Eval-gated PEFT (charter uses eval_jtc_delta)
9. MTK left as optional serve path (not auto-seal)
10. No unbounded crawl

### Safe web
- Allowlist only: `config/governor_verified_urls.txt`
- HTTPS, max size, no creds, text notes under `logs/governor/web_notes/`

## Gates
```bash
make governor_v2       # GOVERNOR_V2_PASS
make governor_quality  # GOVERNOR_QUALITY_PASS
```
