# CNET Janitor G1 — Autonomous library maintenance (2026-07-21)

## Goal
Nightly (or on-demand) **library janitor** without human babysitting:
health pass, low-reliability scan, gap census, report. No unit deletion.
No policy rewrite. Human remains legislator.

## Run
```bash
make cnet_janitor_build
./bin/cnet_janitor soul_gemma4v2_final.cnb
# or
bash scripts/cnet_janitor.sh
RUN_JANITOR=1 make janitor
```

## Systemd
```bash
mkdir -p ~/.config/systemd/user
cp config/cnet-janitor.service config/cnet-janitor.timer ~/.config/systemd/user/
systemctl --user daemon-reload
systemctl --user enable --now cnet-janitor.timer
systemctl --user list-timers | grep janitor
# one-shot now:
systemctl --user start cnet-janitor.service
```

## Env
| Var | Default | Meaning |
|---|---|---|
| `CNET_JANITOR_LOW_REL` | 0.55 | Reliability floor |
| `CNET_JANITOR_LOW_REL_MIN_EV` | 64 | Min outcomes before listing |
| `CNET_JANITOR_NOTE_LOW_REL` | 1 (script) | Write LOW_RELIABILITY gaps |
| `CNET_JANITOR_SNAPSHOT` | 0 | Copy CNB to artifacts/janitor/snapshots |
| `CNET_JANITOR_REPORT_DIR` | artifacts/janitor | Reports + LATEST.md/json |

## Safety
- Refuses if `<base>.stop` exists
- Single-flight lock `artifacts/janitor/.janitor.lock`
- Does **not** delete units or raise cert bars
- `soul_close` persists runtime reliability sidecars

## Reports
- `artifacts/janitor/janitor_YYYYMMDD_HHMMSS.md`
- `artifacts/janitor/LATEST.md` → latest
- Marker: `JANITOR_OK`

## Next (G2)
Policy object bounds + consolidate/dedupe + rollback pins.
