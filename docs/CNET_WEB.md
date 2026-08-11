# cnet-web — Tailscale/localhost cockpit over cnetd

## Run

```bash
# cnetd must be up
systemctl --user start cnetd.service
python3 tools/cnet_web.py
# → http://100.x.x.x:8642/  or http://127.0.0.1:8642/
```

Optional token: `CNET_WEB_TOKEN=secret` then `?token=secret` or `Authorization: Bearer secret`.

Never binds `0.0.0.0` unless `CNET_WEB_ALLOW_PUBLIC=1`.

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
