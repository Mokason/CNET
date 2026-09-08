# Bounded Unicode learning soak

This is a private operational workload, not product acceptance, learned allocator
gain, or genuine chronological demand. It reuses the existing
[learning control plane](AUTONOMOUS_LEARNING.md), source tables and canonical
capsules. It does not connect Wikipedia prose to training or replace the main
Discord daemon. Source authority comes from the pinned independently checked
[Unicode 17 excerpt](../data/unicode17/README.md), never CNET answers.

## Frozen workload

Four datasets are preauthorized before initialization. At startup, the two
numeric explicit-case tables expose their first 16 actual mappings each; both
symbolic tables expose all 95 approved name/label pairs. At original run elapsed
24 and 48 hours, numeric tables expand to 32 rows each, then their complete
58/56 explicit mappings. Symbolic vocabulary and labels remain unchanged.
This is staged delivery of predeclared external evidence, not discovery of new
facts by the monitor. Empty/omitted entries abstain, including during stale-source
refusal. These tables are not full text casing or bidi implementations.

The observer records a source-transition intent with old/new hashes and fixed
deadline before changing either file, then records each replacement. Replacement
requires exact previous bytes and private atomic publication. A failed or killed
observer cannot resume: its exclusive permanent start marker and all partial
artifacts remain. No transition deadline, ledger charge or original run budget is
reset. This intentionally trades automatic monitor recovery for fail-closed
continuity evidence; existing control-plane recovery remains available for cleanup.

The only generated demand is four initial exact requests plus two at each source
expansion. Each is marked `synthetic_demand`, checked against the frozen source,
and kept separate from Discord capture and allocator qualification. A verified
answer is compared to the external label before it can be treated as a success.

## Observation and limits

The script runs once from a frozen private installation as `monitor.py` and
verifies `soak.json`, its own bytes, raw source, native/managed closure and policy
on every round. The manifests use the existing exact installation contract.
`soak.json` contains exactly schema version 1, the trusted absolute `dotnet` host
and a `files` hash map: all eight managed and six native files, the two existing
runtime manifests, policy, monitor and raw Unicode excerpt. Host/framework,
kernel, systemd, Python and libc remain trusted platform dependencies.

The fixed original acquisition budget is **259800 seconds: 72 hours + 10 minutes**.
First complete verification must occur by +180 seconds. Every 60 seconds, the
observer checks owner status, boot identity and heartbeat age (at most 120 seconds).
Observation gaps and gaps between complete successful four-dataset sweeps are
bounded to 300 seconds, including suspend time. A boot change, backwards clock,
missed bound, pin/source drift, unexpected response, or failed verification fails.

Only declared source-transition windows, each at most 180 seconds from its fixed
scheduled start, permit unqualified status observations while acquisition and
the supervisor's independent probation probes run. These are not successful full
sweeps. Exact total job reservations must settle at 4, 6, then 8, with no pending
mutation or outstanding job. This is a **no-retry campaign**, not a claim that
`jobs` counts accepted candidates. Failed/retried reservations are retained but
cannot manufacture a passing campaign.

After settlement, each round requires all four actual `learning verify` commands
to succeed: 1024 encoded-input checks plus 192 symbolic probes, exact counts,
required abstentions, source/installation pins, consistent native generation and
bounded boot-time receipts. Every wrong answer or refusal fails; no rc=2 result
is silently retried or relabelled as healthy. Failed structured evidence and fixed
reason codes are retained. The child collector bounds each output pipe to 32 KiB
and kills/reaps its isolated process group on overflow or deadline.

Owner policy: eight attempts/hour and promotions/day, two attempts/source, 16 jobs,
512 MiB logical work storage, 10-second/512-MiB fixed workers, two-second ticks,
three probation probes, 120-second owner heartbeat gap, allocator disabled.
Each service has one CPU-equivalent quota, 1-GiB memory cap, 128 tasks,
NoNewPrivileges, Unix-only sockets and private working directory. Logical storage
accounting is not a kernel disk quota. Evidence outside `work/` is limited to
4500 exclusive chained receipts of at most 16 KiB each (less than 71 MiB), plus a
small permanent start marker. System journal retention is host-managed, not part
of that evidence quota. Same-UID malicious-owner isolation is not claimed.

## Service lifetime and operator controls

The three [systemd templates](../systemd/cnet-learning-soak-owner@.service) expect a
private installation directly under `%h/INSTANCE` and `%h/dotnet/dotnet`.
They are not enabled at boot and do not change linger settings.

Start `cnet-learning-soak-owner@INSTANCE.service`. Dependencies start the separate
daemon and monitor first; `Type=notify` releases the owner only after the monitor
has persisted its start marker and initial source receipt. The owner's `BindsTo`
and `After` dependencies stop acquisition when the monitor dies, including
SIGKILL/OOM when in-process cleanup cannot run. The owner retains its 60-second
stop allowance for bounded reconciliation/rollback. The monitor is not bound to
the owner, so normal owner completion cannot prevent final verification.

Inspect all three service states and the private `observation/` receipts. To stop:

```sh
systemctl --user stop cnet-learning-soak-owner@INSTANCE.service
env -i /home/marble/dotnet/dotnet /home/marble/INSTANCE/managed/cnet-control.dll \
  learning quiesce /home/marble/INSTANCE
systemctl --user stop cnet-learning-soak-monitor@INSTANCE.service
systemctl --user stop cnet-learning-soak-daemon@INSTANCE.service
```

Replace `INSTANCE` with the exact provisioned directory name. Require successful
quiescence before stopping a healthy daemon; if cleanup refuses, preserve the
daemon/control transport and all recovery evidence. Do not delete markers or
ledgers, refund budgets, change frozen files, or restart a terminal run. Failure
is visible in systemd and structured journals; these templates do not configure
an external paging integration.

## Terminal evidence

Only after `budget_complete`, settled state, all three source stages, a fresh
final full sweep and at least 259200 actual seconds between first and last complete
sweeps may `unicode_soak` be `passed`. `product_acceptance` remains **false** even
then: useful learned allocator gain and genuine reviewed demand are separate,
unmet prerequisites. The marker proves this finite workload and its recorded
continuity, not broader autonomous competence. Synthetic clock tests and short
source-refresh or crash rehearsals never satisfy elapsed acceptance.
