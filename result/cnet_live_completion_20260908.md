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
Discord and capture monitors were not restarted; history was not reset.
Rollback: move only the new override into the private backup, daemon-reload;
retain alert state and release. The old start-limit bug then returns, so rollback
is a recovery option, not a recommended final configuration.

## Remaining measured gates

Recall live rollout, safe MCP live deployment, broader independently verified
learning and frozen unattended-run acceptance are still pending at this checkpoint.
Actual 72-hour product acceptance and useful learned allocator gains remain
WITHHELD. A source test, synthetic demand or capture-only uptime cannot close them.
