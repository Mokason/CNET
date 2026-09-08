# Live completion evidence — 2026-09-08

In progress under [the ordered contract](../plans/cnet_live_completion_20260908.md).
Starting revision `2bfa1d8` was already published to origin/master. Source changes
use a clean worktree; the dirty primary checkout and private data are preserved.

## Retrieval and alert source gates

Synthetic RED reproduced irrelevant answer-only archive matching, duplicate JSON
fields, clipped records/query tokens and corrupt quoted pending proposals.
Fresh-context Astra review additionally reproduced malformed/indented archives,
UTF-8 truncation of the reply utterance and Unicode-adjacent partial matches.
All were fixed, with 18 private daemon regressions passing. A dedicated read-only
recall module replaces the daemon's permissive overlap matcher. Output remains
unverified; this is conservative lexical gating, not semantic or factual proof.

The live alert unit used systemd start limiting as its five-minute notification
throttle. Repeated health failures produced `start-limit-hit` even after a
successful send. A private durable helper now distinguishes successful delivery,
suppression, contention and actual failure. Seven tests cover repeats, expiry,
delivery failure, reboot/clock regression, hostile state, contention and slow
delivery. The latter was an actionable Astra review finding: the interval now
starts at successful delivery completion, not invocation time. Targeted review
reconciliation found no residual actionable issues in either correction set.

`make recall_verify social_reply_verify` passes: 18 new recall cases plus existing
alias/protocol/native-client/daemon cases and 87 capture/presentation tests
(86 pass, one pre-existing skip). Logs are local `/tmp/cnet-recall-social-final-20260908.log`.
No certification floors or training labels changed. No external-model CLI ran.

## Alert deployment

Private staged release: `/home/marble/.local/share/cnet-live-completion-20260908-pOVYbE`.
Only `alert.py` plus its unchanged private-file helper dependency are deployed in
`alert/`; original alert unit is retained under `before/`. Scoped user-service
override: `cnet-capture-alert.service.d/70-alert-helper.conf`. Dedicated mode-0700
state: `/home/marble/.local/share/cnet-discord-capture/alerts`.

At 19:04:39 EEST two real service starts returned `sent`, then `suppressed`.
`Result=success`, `ExecMainStatus=0`, inactive/dead is normal for this oneshot.
Desktop API success is not proof the owner visually read the notification.
This alert deployment did not restart Discord or capture monitors or reset history.
Rollback: move only the new override into the private backup, daemon-reload;
retain alert state and release. The old start-limit bug then returns, so rollback
is a recovery option, not a recommended final configuration.

## Live compatible runtime and MCP rollout

The frozen release above contains a copy of the dirty primary runtime sources
with only the scoped recall/MCP backports, preserving its live ABI and data.
The original capsule-core shared library is retained byte-for-byte (SHA256
`529e139956a72d2795a3dc4f39d63f4aa6e44946f57e40c931ba6796aed7982e`).
The new managed MCP server uses the
unchanged native model library. A private copy of the actual 1.18-GB base,
1096-unit roster, legacy brick bank and certified capsule inventory was rehearsed
before a scoped restart. Native recall ASan/UBSan also passed 2000 bounded random
records. No existing primary files, memories, capsules or LUTs were deleted.

Rehearsal exposed an existing shared-broker limit: the 317-oracle metadata
response exceeded asyncio's default 64-KiB pipe frame and silently killed its
reader. The existing external broker was first imported byte-identically, then
repaired with explicit 2-MiB transport frames, strict complete JSON parsing,
bounded restored-ID replies and failure propagation. This is not an increase in
the safe-read tool's 256-KiB evidence allowance. Five broker tests pass on Python
3.11 and 3.12, including exact/over-limit frames, deep JSON, open failed streams
and original-ID expansion. The external broker original is backed up; future
thin clients receive the fix on reconnect. Existing clients were not killed.

New overrides are `cnetd.service.d/70-recall-safe-mcp.conf` and
`cnet-mcp-shared.service.d/70-safe-read.conf`. The backend permits read requests
only for `en.wikipedia.org`; the daemon uses the separately certified dispatch
capsule. The shared service uses `Restart=always` because inherited SIGTERM
cleanup exits zero even after a failed reader. A deliberate systemd stop still
stops it. Startup finished at 19:26:03 EEST on September 8.

All eight live peer/Discord-renderer checks passed: identity, presence, refusal
of the unrelated archived language-model note, verified conversions for 173 and
181 bytes, abstention for uncovered 255 bytes, real Wikipedia read and blocked
localhost read. The Wikipedia result remained unverified. At 19:33 EEST the
existing owner-only DM scope was revalidated and Discord accepted one clearly
labelled synthetic diagnostic using the deployed gateway's `peer_ask` and renderer.
This proves API delivery, not that the owner read the message. Capture still had
six real requests, zero unfinished/peer/reply failures and no eligible training
labels after that synthetic bot message. Discord independently reconnected at
19:18:58, before this daemon restart; its actual PID/continuity was not assumed.

The public read and private destination refusal also passed in an isolated
actual-backend rehearsal. `make mcp_read_verify` passed native boundary/dispatch,
131 managed tests and hermetic integration; the public opt-in integration test
remained skipped there and was exercised separately. MCP and control-plane NuGet
advisory audits report no vulnerable packages in their current sources; these
are scoped dependency results, not a whole-repository security certificate.

Rollback: stop only the two changed services, move their new `70-*` overrides
into the retained private backup, restore the backed-up external broker, reload
the user manager and restart the previous services. Keep all live data and the
frozen release. Rehearsal logs/build recipes and actual live smoke receipts are
retained under the private release root; they are not committed private data.

## Broader verified learning and monitor qualification

The two numeric and two symbolic pinned Unicode workloads passed their existing
independent acquisition/acceptance gates. A new real native/managed test then
exercised all three truthful source stages, eight reservations, repeated hot
swaps, increasing native revisions and all four capabilities coexisting after
each stage. It passed in 44 seconds; that is a source-refresh test, not elapsed
soak evidence. The existing 804-test managed learning regression passed, as did
11 independent extractor tests and the deployed unattended-learning health check.

The [bounded soak runbook](../docs/UNICODE_LEARNING_SOAK.md) freezes the campaign.
Thirteen monitor tests pass after RED reproductions. Fresh Astra review found
and resolved output accumulation, unchecked synthetic numeric responses, lost
failure receipts and missing owner heartbeat checks. Review reconciliation found
no residual issues in those corrections. External-model review was not invoked,
following the owner's Astra-only preference.

An actual private systemd fault rehearsal at
`/home/marble/cnet-unicode-crash-20260908-hiX3rh` acquired four capsules and completed
the first full sweep. Killing its monitor with SIGKILL stopped the bound owner;
the ledger became paused/failed with `cancelled`, no outstanding job and no pending
mutation. Restarting the monitor refused its permanent start marker. Explicit
quiescence succeeded before stopping its private daemon. The failed rehearsal
and all artifacts remain preserved; it contributes no elapsed acceptance time.

## Remaining measured gates

Frozen long-duration observation is pending at this source checkpoint.
Actual 72-hour product acceptance and useful learned allocator gains remain
WITHHELD. A source test, synthetic demand or capture-only uptime cannot close them.
