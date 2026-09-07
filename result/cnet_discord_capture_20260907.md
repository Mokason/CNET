# Scoped Discord capture rollout — 2026-09-07

Source: journal `e405766`, bridge `84883b2`, on
`feature/autonomous-learning-20260906`. The approved owner-only DM collector is
live. Useful allocator improvement, independent mapping/labels and fresh
confirmation remain WITHHELD; this is ingress infrastructure, not a gain result.

## Measured verification

- 33 capture tests passed, including scope exclusions, restart deduplication,
  unfinished attempts, SQLite-full rollback, schema/policy refusal, event quota,
  metadata-only read-only status, pre-forward commit, fatal error closure,
  ambiguous timeout, REST/WebSocket redirect refusal and native input guards.
- 15 legacy chronology-audit and 13 sequence-pilot tests passed.
- All four allocator CPU executables passed: comparator/budget/fairness,
  data validation, gate algebra and canonical candidate validation. Gate algebra
  explicitly reports actual-outcome gain WITHHELD.
- A 500-request disposable synthetic journal benchmark measured begin+finish
  median **1.524 ms**, p99 **1.938 ms**, total **780.697 ms**. Database 147,456 bytes;
  process RSS high-water 14,916 KiB. This excludes Discord/native-serving latency,
  does not measure large-store performance and is not real demand.

Astra-only independent design/code reviews corrected ambiguous attempt/delivery
semantics, schema/policy reuse, page-size bounds, event-quota bypass and socket
redirect handling. Required findings were addressed. TDD RED history and the two
accidental unauthenticated probe connections during fixture development are
recorded in the [decision record](../plans/cnet_discord_capture_20260907.md).
No real user message was synthesized or posted; no fixture entered the live store.
This is not a full dependency/host vulnerability clearance or long-run acceptance.

## Live deployment

Read-only Discord metadata identified the application's individual owner as the
sole recipient of its existing last-used DM. Runtime revalidates that exact
owner/DM before opening the collector. Identifiers remain in private configuration.

Only `cnet-discord-peer.service` was restarted, at **20:52:25 UTC**; it reached
authenticated Gateway READY with capture enabled at **20:52:27 UTC**. At the
20:53:27 UTC follow-up it remained active/running as PID 651064, with zero automatic
restarts. The old service's historical restart counter was 850; the explicit
restart reset it, which is not evidence that prior instability was repaired.

Eight other checked services retained identical PIDs, active/running state,
start timestamps and restart counts across rollout: `cnetd`, private table
daemon, residual mouth, both Bonsai services, offline Mistral, personal learning
lane and shared MCP service. `/health` on ports 8080, 8081, 8083 and 8092 returned
200. These are bounded liveness checks, not concurrent latency/correctness proof.
No GPU job, allocator activation, core-service restart or existing learning
ledger/runtime-policy mutation was performed.

Deployment artifacts:

- Release: `/home/marble/.local/share/cnet-discord-capture/releases/84883b2/`
- Dataset: `/home/marble/.local/share/cnet-discord-capture/data/`
- Scope configuration: `/home/marble/.local/share/cnet-discord-capture/capture.env`
- New service drop-in:
  `/home/marble/.config/systemd/user/cnet-discord-peer.service.d/40-private-capture.conf`

Release/data directories are mode 0700; new Python source 0400, copied native
peer 0500, scope/drop-in files and SQLite/lock files 0600. Source copies match the
frozen commit. Existing token configuration, original gateway script, original
service file and the main worktree were not edited. The native peer copy matches
the original byte-for-byte; its main-worktree permissions were not changed.
Systemd configuration validation passed before rollout.

| Artifact | SHA256 |
| --- | --- |
| gateway.py | `0445f8e9af6ae29e98413ea8d1db2d622cf52a4ba64647893fd27ac7fc769619` |
| journal.py | `eaec889fd5b9b55ff31fa8e5e2909a5c47711b24e5b3cfb87047f74ab1bc226c` |
| copied native peer | `0ecb13d4c3e4696a20bd0a36ef48a860865f77817aabdead2d4cb737917b6059` |
| preserved original gateway | `de64a18345fda51596940ec41059361b5535ade017be7dbbffbd830be4e2e91d` |
| preserved original service | `2f8f6b86e3b3d0d92cfd82d55489c5d27297b016c8bbd50e96e31acbc213b955` |

## Sequence readiness and remaining work

The read-only live readiness check at 20:53:27 UTC reported **0 requests**, one
started/ready segment, zero unfinished attempts and zero observed sequence or
selected-request clock regressions. Database size was 20,480 bytes. This is an
empty new evidence stream, not a complete demand history or an end-to-end live
answer test. Live message forwarding is exercised by ordinary subsequent use;
its mock integration tests are separate evidence.

The next sequence is deliberately stopped before fitting:

1. Collect genuine one-line tasks in the existing private DM. Explicitly exclude
   automation/test sessions during later curation; Discord account identity
   alone does not distinguish a human from same-account automation.
2. Freeze typed-capability mappings and independent verifier receipts. Unknown
   mappings stay unknown; client exit zero and CNET answer text are not labels.
3. Define whole chronological development episodes, exclude continuity gaps,
   and compare strong deterministic controls with equal budgets/state/actions.
4. Only if useful development headroom exists, perform bounded AMD fitting,
   freeze the candidate and evaluate fresh whole-episode confirmation under
   unchanged +0.05 gain, positive lower confidence and family non-regression.

No old mixed miss log, exposed holdout or synthetic traffic is substituted.
The readiness tool is an aggregate diagnostic, not a dataset exporter, mapping
implementation or training gate. Capture stays active; there is no background
automatic training job or scheduled confirmation run.

## Operator commands and rollback

Readiness (no payload output or database writes):

```sh
/home/marble/.config/cnet/venv-discord/bin/python \
  /home/marble/.local/share/cnet-discord-capture/releases/84883b2/journal.py \
  /home/marble/.local/share/cnet-discord-capture/data
```

Rollback only this new override, retaining the original gateway and all evidence:

```sh
mv -n /home/marble/.config/systemd/user/cnet-discord-peer.service.d/40-private-capture.conf \
  /home/marble/.local/share/cnet-discord-capture/40-private-capture.conf.disabled
systemctl --user daemon-reload
systemctl --user restart cnet-discord-peer.service
```

Before using rollback, confirm the `.disabled` destination is absent and the
drop-in has not gained unrelated owner changes; `mv -n` may otherwise leave the
override enabled. Rollback restores the original bridge, including its older
logging/control behavior. No rollback was needed or executed during this run.
Stopping the bridge also stops collection. Storage exhaustion or a fatal journal
error stops the shared bridge, not just the selected conversation.

See [operator limits](../tools/discord_capture/README.md). Owner-private journal
storage does not redact or change existing downstream daemon/legacy miss logs;
whole-host data privacy is not claimed. Those old logs remain ineligible for
this chronology experiment.

## Retained verification receipt

Final test outputs/exit codes and the eight-service before/after snapshots are
retained privately in
`/home/marble/.local/share/cnet-discord-capture/rollout-evidence.json` (mode 0400),
SHA256 `4752ff5f7d29caa171bb3c1a727242e8df55dfc8bb97eb1b748cfcb12502a6ca`.
They contain fixture results and service metadata, not real requests or tokens.
The final Astra evidence review found no required report/handoff changes; live
answer proof, genuine demand and downstream legacy-log privacy remain explicitly
outside the completed capture evidence.
