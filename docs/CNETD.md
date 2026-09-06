# cnetd — warm UNIX socket front door

## Socket

1. `$XDG_RUNTIME_DIR/cnet/cnet.sock` (preferred)
2. `~/.local/share/cnet-minimal/run/cnet.sock` (fallback)
3. `CNET_SOCK` override

## Commands

```bash
make cnetd
make cnetd-run                    # build + start + smoke
scripts/cnet_sock_ask.sh "who are you"
echo 'PING' | socat - UNIX-CONNECT:$XDG_RUNTIME_DIR/cnet/cnet.sock
```

## Protocol

```
ASK <query>
→ SOURCE … / ANSWER … / END

PEER <name> <query>
→ same response, with request-local peer metadata

{"op":"ask","q":"..."}
→ JSON one line

PING → PONG
STATUS → OK cnetd …
```

Every connection carries exactly one newline-terminated request. The newline
must arrive within the 8,191-byte receive boundary; an overlong request is
refused, never processed as a truncated query. JSON is parsed as JSON rather
than searched as text: `q` is a required, non-empty string, `op` may be omitted
or equal `ask`, duplicate control fields and trailing data are refused, and
valid unknown fields are ignored. `PEER` identity applies to that request only;
the following ASK, JSON, or raw request is local again.

The daemon deliberately dispatches clients serially because `CdState` is one
mutable warm session. A client cannot hold that session indefinitely: framing
uses one monotonic deadline and returns `ERR request_timeout`,
`ERR request_too_long`, or `ERR incomplete_request` on boundary failure.

## Env

`CNET_MINIMAL_ROOT`, `CNET_PACKS_ROOT`, probe short-circuit, domain routes.

- `CNETD_CLIENT_READ_TIMEOUT_MS`: request framing deadline; default `5000`,
  clamped to `50..60000`.
- `CNET_MCP_TIMEOUT_MS`: total deadline for an outbound shared-MCP
  connect/write/newline-reply exchange; default `2000`, clamped to
  `50..60000`. Partial writes are completed within that same deadline.

## systemd

```bash
cp scripts/systemd/cnetd.service ~/.config/systemd/user/
systemctl --user daemon-reload
systemctl --user enable --now cnetd.service
```

Law: never self-CERT; probes short-circuit with `SHORTCIRCUIT 1`.
