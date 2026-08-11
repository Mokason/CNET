# cnet-web — Tailscale/localhost cockpit over cnetd

## Run (production — systemd user)

```bash
systemctl --user enable --now cnetd.service
systemctl --user enable --now cnet-web.service
systemctl --user status cnet-web.service
# scripts/cnet_web_run.sh resolves tailscale0 100.x (else 127.0.0.1)
# → http://100.x.x.x:8642/
```

Manual:
```bash
systemctl --user start cnetd.service
scripts/cnet_web_run.sh   # or: python3 tools/cnet_web.py
```

Optional token: `CNET_WEB_TOKEN=secret` in env / drop-in, then `?token=secret` or `Authorization: Bearer secret`.

Never binds `0.0.0.0` unless `CNET_WEB_ALLOW_PUBLIC=1`.
Unit: `Requires=cnetd.service`, `WantedBy=cnet-marble.target`.

## API

| Path | Role |
|------|------|
| `GET /` | Cockpit SPA |
| `POST /api/ask` | `{q}` → cnetd (promote forbidden) |
| `GET /api/status` | cnetd + neuromod + KPI |
| `GET /api/miss_log?n=40&organic=1` | miss stream |
| `GET /api/explore` | curriculum queue |
| `POST /api/explore/approve` | **gold_file only** |
| `POST /api/explore/reject` | review log |

## Law

`never_self_cert=1` — browser cannot mint pack_personal.
