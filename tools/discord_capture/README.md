# Private Discord chronology

`gateway.py` is the versioned successor to the existing local Discord PEER
bridge. `journal.py` records selected request evidence in SQLite. It is not a
learner or allocator activation path. The separate offline evidence tools below
provide explicit independent labels and development-only exports.
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

## Independent evidence and immutable datasets

`evidence.py` reuses the native semantic intent parser and independent native
arithmetic tool. The pinned offline catalog is exactly `bytes → bits` (multiply
by 8) and `u8 → masked8` (xor 255), input 0–255. It does not install live tool
policy, broaden certification or judge the actual CNET reply. Native output must
match a separate integer calculation; a live CNET answer is never a label.
Unrelated/ambiguous text stays unmapped. Repeat all 512 native pairs and four
refusals with `make -C experiments/offline_controller capture-evidence-native-test`.
The printed private build directory contains the two required runtime artifacts.

Run `dataset.py CAPTURE NEW_EXPORT RUNTIME [--review PRIVATE_REVIEW.json]` with
the existing Python environment, a private parent outside git and an external
60-second timeout. It publishes a manifest last; interrupted exports are invalid
and must not be reused. Never overwrite an export or edit labels in place.
Capture contents and IDs remain private. The manifest is identity evidence, not
independent custody against a compromised owner/runtime.

Origin review maps an exact `episode_sha256` from private `episodes.json` to
`human`, `test`, or `automation`; missing review is `unreviewed`. Owner identity
does not prove human demand. Review binds complete records and fixed UTC
30-minute windows. Mixed test/human windows must be excluded, not selectively
trimmed. Closed windows require continuous same-segment recorded ACK heartbeats
covering both boundaries (at most 120 seconds apart), no gap/stop/reconnect,
unfinished attempt or unmapped request. Older pre-heartbeat captures cannot be
retroactively made continuous. Excluded requests remain in denominator reports.

`qualify.py EXPORT RUNTIME` rechecks labels and rebuilt episodes, then uses the
existing six equal-budget planners on 16 fixed, disjoint 32-row direct actions
(8 jobs, 256 row-work). This is a prospective direct-catalog bound, **not native
certified outcomes**. A zero bound against the strongest exact control is expected
for this separable workload, not proof that other workloads cannot improve.
Ordered two-thirds/one-third whole-window partitions are both exposed development;
neither becomes confirmation. The tool never starts fitting or grants activation.
Useful native trajectory headroom, all four campaign families and new post-freeze
confirmation remain required before the existing AMD campaign is justified.
The >=.05 gain, positive paired95 lower bound and nonnegative family means remain.

## Alerts

`monitor.py CAPTURE STATE` records bounded metadata-only health every 60 seconds;
`--watchdog` independently checks fresh successful observation. Install the
provided user-systemd templates after replacing `@RELEASE@` with the frozen
revision; bridge and monitor must execute that same release. Keep `AccuracySec=1s`.
Both failure paths trigger the rate-limited local desktop notification service
(at most one per five minutes). No external notification provider is contacted.

Inspect `systemctl --user status cnet-capture-monitor.timer
cnet-capture-watchdog.timer` and their `.service` journals. Inspect private
`STATE/latest.json` for the current bounded reason codes:

- `bridge_down`, `heartbeat_stale`, `request_stuck`: check bridge liveness and
  connectivity, then reconcile unfinished attempts; never resend them blindly.
- `storage_near_limit`, `disk_reserve_low`: stop/review/archive locally before
  creating a separate capture store. Nothing is automatically deleted.
- `reconnect_burst`, `capture_discontinuity`, `observation_gap`: investigate the
  connection/clock/configuration change; elapsed acceptance starts over.
- `legacy_privacy`: re-resolve the two reviewed legacy paths before repair.
  A retargeted `current` alias deliberately refuses the old assumption.
- `monitor_refused`, watchdog failure: inspect metadata, private boundaries and
  timer status. Preserve history and crash-orphan staging files for inspection;
  never delete state to claim continuity.

The watchdog requires a tick no older than 120 seconds. Healthy time advances
only between successful, same-boot/source/policy/segment observations at most 90
seconds apart, with no intervening journaled discontinuity. Sampling failures are
durable unhealthy rows. Unwritable state fails loudly; a missed minute breaks
continuity on recovery. Idle demand alone is not an alert. Observation history is
capped at 32 MiB and refuses when full; capture alerts start at 80% of its limits.

`observation_72h` describes only real elapsed capture-health continuity after
frozen capture artifacts. `product_acceptance` always remains false here: adequate
genuine traffic, independent outcomes and unattended learning acceptance are
separate gates. No fixture, elapsed-time simulation or newly started timer passes
them. Desktop alerts cannot warn while this host/session itself is unavailable.
