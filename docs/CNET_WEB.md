# Local web client

The web client forwards requests to the local daemon; it is not a new
certification or promotion authority. Sources:
[tools/cnet_web.py](../tools/cnet_web.py) and
[scripts/cnet_web_run.sh](../scripts/cnet_web_run.sh).

## Authentication and binding

`CNET_WEB_TOKEN` is required by default. Clients send
`Authorization: Bearer <token>`. Query-string `?token=` authentication was
removed because URLs leak through logs, history and referrers. Do not use the
old optional-token instructions.

`CNET_WEB_NO_AUTH=1` only bypasses startup refusal: request authentication still
checks for a Bearer header. It is not a supported unauthenticated mode. Store
credentials outside versioned documentation and avoid
printing them in status commands or example URLs.

The wrapper prefers the configured host, then a Tailscale address, then loopback.
The default port is 8642. Wildcard binds need `CNET_WEB_ALLOW_PUBLIC=1`;
do not enable public exposure as a setup shortcut. A private bind does not
replace application authentication.

## Routes

| Route | Role |
| --- | --- |
| `GET /` | Browser interface |
| `POST /api/ask` | Forward a query, including enabled mutating daemon commands |
| `GET /api/status` | Runtime/status information |
| `GET /api/miss_log` | Filtered miss stream |
| `GET /api/explore` | Proposed exploration items |
| `POST /api/explore/approve` | Gold-file review path, not self-certification |
| `POST /api/explore/reject` | Record review rejection |

Inspect the handler for supported fields and limits before integrating a client.
This endpoint is not read-only: query text can invoke daemon commands such as
`teach` and gold actions that write evidence and trigger evolution. Bearer
access includes these chat actions. Rejecting top-level `promote`/`accept`
fields is not a command-level authorization boundary.
A browser approval does not authorize arbitrary code execution or a neural
candidate's self-promotion.

## Running

Inspect the selected socket, environment and authentication first.
`scripts/cnet_web_run.sh` starts a foreground server; the user unit
`cnet-web.service` is a deployment option that requires intentional operator
installation/start. Neither should run automatically during documentation tests.

See [daemon protocol](CNETD.md), [operations](CNET_MARBLE_24_7.md) and
[security](SECURITY.md).
