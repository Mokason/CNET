# Local daemon and socket protocol

`cnetd` is a serial UNIX-socket front door over one mutable warm session.
Build it with `make cnetd`. Building does not start or restart a service.

Socket selection uses `CNET_SOCK` when set, otherwise the runtime-directory
socket (`$XDG_RUNTIME_DIR/cnet/cnet.sock`) or the user's installed-runtime
fallback. Clients and the daemon must agree on the effective path.

## Protocol

A connection carries exactly one newline-terminated request:

```text
PING
STATUS
ASK a query
PEER peer_name a query
{"op":"ask","q":"a query"}
```

PING returns PONG. Text queries return SOURCE/ANSWER/END framing; JSON queries
return one JSON line. JSON requires a non-empty string `q`; `op` can be
omitted or be `ask`. Duplicate control fields, trailing data and malformed
JSON refuse. Valid unknown fields are ignored. PEER identity is request-local,
not a permanent session identity.

Query access is not read-only. Enabled chat commands include `teach TAG n m`
and gold actions that write labeled evidence and trigger evolution. Socket
access, or web bearer access that forwards to it, must therefore be restricted
to trusted operators. Certification remains a separate acceptance boundary.

The newline must arrive within the 8191-byte receive boundary. Oversized,
incomplete or timed-out frames are refused, never executed after truncation.
The read deadline uses one monotonic budget:
`CNETD_CLIENT_READ_TIMEOUT_MS`, default 5000, clamped to 50–60000.
Outbound shared-MCP exchanges use `CNET_MCP_TIMEOUT_MS`, default 2000,
clamped to the same range, across connect/write/reply.

## Capsules and residual behavior

`CNET_CAPSULES_DIR` enables the typed capsule path described in
[CAPSULE_CORE.md](CAPSULE_CORE.md). Explicit capsule refusal never becomes a
verified residual/template answer. The ordinary daemon reloads that inventory
per capsule request.

The legacy ROE mouth in the current daemon hard-disables teacher-on-miss in
source. Setting `CNET_TEACHER_ON_MISS=1` does not re-enable that branch.
Other teacher/acquisition integrations are separate; see [TEACH_PATH.md](TEACH_PATH.md).
Templates and presentation text must not be confused with new certified knowledge.

## Verification and operations

```sh
make cnetd_protocol_boundary cnet_mcp_transport capsule_frontdoor
```

These checks use private sockets. For an intentionally selected running daemon,
`scripts/cnet_sock_ask.sh` is the client helper. Check its `CNET_SOCK` first.

Service installation/start/stop changes live state. Use
[the operations guide](CNET_MARBLE_24_7.md), inspect effective configuration, and
obtain operator authority for the intended change. Do not copy installation
commands from historical deployment reports.
