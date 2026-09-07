# Private Discord chronology

`gateway.py` is the versioned successor to the existing local Discord PEER
bridge. `journal.py` records selected request evidence in SQLite. It is not a
learner, correctness verifier, training exporter or allocator activation path.
The [decision and sequence](../../plans/cnet_discord_capture_20260907.md) govern it.

## Configuration

Use the existing bot token and websocket-client environment. Qualification used
Python 3.12.3, websocket-client 1.9.0 and SQLite 3.45.1; no runtime installation is
performed. Keep tokens in the existing private token file or environment file.
Capture requires all three environment variables:

- `CNET_DISCORD_CAPTURE_DIR`: pre-created owner-only directory (mode 0700),
  outside git. Do not share it with unrelated files or another writer.
- `CNET_DISCORD_CAPTURE_OWNER`: exact individual application-owner user ID.
- `CNET_DISCORD_CAPTURE_CHANNEL`: exact existing one-to-one DM ID.

Startup checks Discord application ownership and the DM's sole recipient.
Empty scope IDs, team ownership, group DMs, guild channels, changed existing
dataset policy and a capture channel excluded by `CNET_DISCORD_CHANNELS` refuse.
Without all three variables absent, partial configuration never falls back to
unscoped capture. IDs are stored in the private dataset policy, not source.

Use the existing serving prefix/channel variables as before. Capture follows
that prefix filter and only observed, nonempty text MESSAGE_CREATE events.
No history fetch, edits/deletions, attachment contents, bots/webhooks/forwards,
guild messages or other accounts enter the dataset. Account attribution does
not prove manually typed or representative demand. Same-account automation is
not distinguishable here and must be excluded in later dataset curation.

## Guarantees and limits

A unique message is durably committed before one peer-client invocation;
duplicates never repeat that invocation, even if a previous attempt is unfinished.
Requests lost before receipt are unknown. Connection segments, sequence gaps and
unfinished attempts must be considered before defining whole workload episodes.
`delivered_text` stores the prepared normalized payload, not proof of delivery.
`peer_ok` means client exit zero, not a verified answer. `peer_error` means local
rejection/client error, not confirmed downstream execution failure. Timeouts and
ambiguous reply delivery remain unknown. No response text is stored as truth.

Fatal journal errors stop the shared bridge, including other conversations;
normal outside-scope traffic continues serving without capture. Database limit
is 64 MiB, with a rollback journal potentially consuming approximately another
64 MiB. Each request has at most 16 KiB original and prepared text; each table is
capped at 100,000 rows. Full storage refuses rather than deleting old evidence.
Stop collection and review/archive locally before starting a separate dataset.
This is owner-private storage, not a same-account/host-compromise defense.

The native client's control words (`PING`, `STATUS`, `QUIT`), CLI-like leading
`-`, embedded CR/LF/NUL and queries above 8000 UTF-8 bytes are rejected before
invocation. Multiline requests should be submitted as one line. This closes
control/framing/truncation hazards; ordinary answer formatting is unchanged.
REST and WebSocket redirects are refused; identification requires a validated
TLS endpoint and HTTP 101 handshake. Generic logs contain metadata only.

## Tests and operator status

```sh
make -C experiments/offline_controller discord-capture-test \
  DISCORD_PYTHON=/path/to/existing/venv/bin/python
/path/to/existing/venv/bin/python tools/discord_capture/journal.py /private/capture/root
```

Tests use disposable synthetic stores and prohibit socket connections. Readiness
reads the database without exporting payloads; unknown mappings and independent
labels are explicit. It never authorizes training. Keep synthetic checks out of
the live store, and do not send test prompts as purported real demand.

Deploy from a frozen source revision into a separate owner-private release
directory. Override only the existing bridge's `ExecStart` via a new systemd
drop-in; preserve its token/configuration and original script for rollback.
Use `UMask=0077`, an explicit native peer binary and the three scope variables.
Restart only the bridge after tests/review; check READY, private journal state
and unchanged core-service PIDs. To roll back, move only the new drop-in aside,
daemon-reload and restart the bridge; retain the dataset and release artifacts.
Do not use `systemctl revert`, which may delete unrelated owner configuration.

Continuity acceptance and useful allocator gain are separate later gates, not
implied by bridge READY or these fixture tests. The next evidence is genuine
selected requests with explicit typed mappings and independent verifier receipts.
