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

The opt-in [MCP read brick](MCP_READ_BRICK.md) handles `wiki search ...` and
`web read ...` before legacy actions. It requires a separately configured
dispatch capsule and updated read-policy-enforcing MCP peer. Its answers and
refusals are terminal `MCP_READ`, never certified; returned web text bypasses
automatic learning hooks. The read guide documents configuration and limits.

## Capsules and residual behavior

`CNET_CAPSULES_DIR` selects a startup-only resident inventory for compatibility.
For durable named-set control, configure all of `CNET_CAPSULE_SETS_DIR`,
`CNET_CAPSULE_STATE_DIR` and `CNET_CAPSULE_CONTROL_SOCK`, with no legacy inventory
variable. Invalid or unrecoverable configuration fails startup loudly.
[CAPSULE_CORE.md](CAPSULE_CORE.md) gives the operator sequence and migration.
Explicit capsule refusal, including unload, never becomes a verified residual
or template answer. Publication alone no longer changes serving knowledge.

The separate control socket accepts owner credentials and named operations,
not chat-supplied paths. The daemon remains serial: staging may delay subsequent
ASK requests. Each ASK pins one resident generation; no concurrency or hard
real-time admission claim is implied. `source-fact NAME` uses the closed
[source-evidence adapter](CNET_SOURCE_EVIDENCE.md), with an explicitly configured
`CNET_CAPSULE_SOURCE_ROOT`.

The legacy ROE mouth in the current daemon hard-disables teacher-on-miss in
source. Setting `CNET_TEACHER_ON_MISS=1` does not re-enable that branch.
Other teacher/acquisition integrations are separate; see [TEACH_PATH.md](TEACH_PATH.md).
Templates and presentation text must not be confused with new certified knowledge.

## Verification and operations

```sh
make cnetd_protocol_boundary cnet_mcp_transport capsule_frontdoor capsule_product_closure
```

These checks use private sockets. For an intentionally selected running daemon,
`scripts/cnet_sock_ask.sh` is the client helper. Check its `CNET_SOCK` first.

Service installation/start/stop changes live state. Use
[the operations guide](CNET_MARBLE_24_7.md), inspect effective configuration, and
obtain operator authority for the intended change. Do not copy installation
commands from historical deployment reports.
