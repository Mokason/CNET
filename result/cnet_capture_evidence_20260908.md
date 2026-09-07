# Private capture evidence and operational rollout

## Outcome

The approved mapping, immutable export, development qualification and operational
monitoring implementation is deployed locally. Useful allocator improvement,
fresh confirmation and 72-hour unattended product acceptance remain **WITHHELD**.
No fitting or automatic activation was authorized by these diagnostics.

Source commits: `4e5a572` (independent evidence/exports), `f8073b2` (monitoring),
`345ac81` (development qualification), `82956f9` (portable legacy-audit depth cap).
Frozen live capture release is `345ac81`; the later audit change does not alter it.
Branch is `feature/autonomous-learning-20260906` in the isolated worktree. No push
or change to the user's dirty main worktree occurred.

## Real evidence, not fixture demand

One exclusive private export was published at
`~/.local/share/cnet-discord-capture/exports/20260908-345ac81-unreviewed`.
Its canonical manifest digest is
`7f6dfc24a414d1b9d097b1c3fe3c7523f9b256a809f247fa01f71203fa657419`.
No request/response payload or application-owner/channel ID was printed or
committed. The four existing requests remain origin-unreviewed; the owner was
asked whether they were genuine tasks or tests, without assuming an answer.

| Measured field | Result |
|---|---:|
| Captured requests | 4 |
| Independently mapped labels | 0 |
| Whole UTC windows containing demand | 2 |
| Eligible development windows | 0 |
| Excluded requests retained in denominator | 4 |
| Actual scored native trajectories / fitting runs | 0 / 0 |

Both windows are unreviewed, unmapped and missing historical heartbeat coverage;
one also contains a recorded continuity boundary. Counts of exclusion reasons
overlap. None was repaired, silently filtered or relabelled. A second read-only
qualification reproduced the same digest and refusal. New heartbeat events
cannot retroactively certify these old windows.

The offline catalog contains only the two existing example arithmetic rules,
bytes→bits and u8→masked8, inputs 0–255. It reuses the native parser/tool and checks
expected values independently. This neither assesses live CNET replies nor enables
the example catalog as a live acquisition policy. Extending coverage needs explicit
typed mappings and independent verifiers for actual task families, not guessed
labels or CNET's own Tier-A answers.

## Verification and scoped measurements

- 66 capture/evidence/dataset/qualification/monitor tests passed with the explicit
  deployed native runtime. This includes all 512 finite native rule/input pairs
  and four native parser/refusal cases; it is one integration test in the 66 count.
- Repeatable native build/test target passed independently, using strict compiler
  warnings. Those 512 pairs are fixtures, not 512 captured requests.
- 29 chronology/sequence tests passed on both Python 3.11 and the actual bridge
  Python 3.12 environment. A pre-existing deep-JSON refusal relied on interpreter
  recursion behavior: RED on 3.12, then green with an explicit 64-level cap and
  quoted-bracket/escape boundary tests. No certification floor changed.
- Four existing CPU allocator/data/gate/candidate binaries passed. No new GPU
  benchmark or native capsule outcome campaign ran on the ineligible data.
- RED→green regressions cover independent-label disagreement, immutable publication,
  identity/manifest flags, final-label deadline, byte budget, window/origin/gap
  exclusion, monitor reconnects, lost observations, crash staging and stale watchdog.
- Fresh Astra-only independent reviews found and closed export and monitor issues.
  Reviewed multiwindow development splitting and rehashed receipt/episode tampering
  also have passing tests. No cross-provider review was used.
- Single local diagnostic timings: qualification of the four-request/zero-eligible
  export took 0.02 s wall, peak RSS 19,932 KiB; read-only watchdog took 0.02 s,
  peak RSS 18,780 KiB. These are small-input operator checks, not AI throughput,
  learning-speed or large-store performance measurements.
- `pip check` passed in the existing bridge environment; no dependencies were
  installed. This is dependency consistency, **not full dependency/CVE clearance**.
  Security conclusions are limited to reviewed capture/evidence/operations paths.

## Live rollout and privacy

Before rollout, the bridge PID was 651064 and cnetd PID 2895419. After the scoped
restart they were 750514 and 750511 respectively, both active with zero automatic
restarts and UMask 0077. Original bridge script, token, capture scope, native peer
and certified-core environment override were preserved. The only cnetd drop-in
added was `40-private-log-umask.conf`; its runtime/certification pins were not edited.

Exact metadata repair, owned by the same user:

| Path under `~/.local/share/cnet-minimal/` | Before → after |
|---|---|
| `var` | 0775 → 0700 |
| `var/miss_log.jsonl` | 0664 → 0600 |
| `versions/CNET-Minimal-273250e/var` | 0755 → 0700 |
| `versions/CNET-Minimal-273250e/var/discord_last.json` | 0664 → 0600 |

Both files' SHA-256 digests were identical immediately before/after chmod.
No contents were removed. Existing private ancestors already blocked outside-user
traversal; this repair is defense in depth, not evidence of prior disclosure.
The `current` alias was resolved to the listed version. Monitoring refuses privacy
assumptions if that alias is later retargeted. No unrelated service umask changed.

Private serving, mouth, both Bonsai services, offline Mistral and shared MCP
retained PIDs 2698931, 1852, 994466, 991807, 1933 and 2119296. All four existing
HTTP health checks on 8080/8081/8083/8092 returned 200 before and after rollout.
The personal learning lane was not restarted or modified by this work; it had
independent process changes during the session, so no unchanged-PID claim is made.

The deployed gateway SHA-256 is
`8d995bd8b15b41ccd3ded1500d09e4871dbad7c251a586b385ec1f7da29d2946`;
unchanged native peer digest is
`0ecb13d4c3e4696a20bd0a36ef48a860865f77817aabdead2d4cb737917b6059`.
Native parser/tool digests are
`568d4017bea1c4e290faac8e21918d7f6e3dfbfafbbcc53a103ab67c0eda4dc9` and
`d805ecd315452ddbdc61d3e96f34b84aa2d0cfa18c273a873c05fa5d5bee1bf5`.

## Alerts and actual observation

User-systemd monitor and watchdog timers are enabled, each at 60 seconds with
AccuracySec=1s. Services have 20-second, 256-MiB and 16-task limits. A disposable
missing-state watchdog fixture failed as intended, triggered OnFailure, and the
generic local desktop notification command completed successfully. This proves
submission to the local notification service, not that a human saw it. The
fixture never contacted Discord or wrote live capture/observation state.

Initial real observation began at 2026-09-07 21:36:59 UTC (September 8 locally).
Subsequent scheduled monitor/watchdog checks were healthy: live ACK heartbeat,
four retained requests, zero pending attempts, 20-KiB capture DB, private legacy
paths. Capture-source dependency digest:
`905baa3fbae99d7be40ab99c222b1e61dd7ff18c4d4aa6c797d1d03d83e4f8a9`.

Real elapsed accumulation uses boot/source/policy/segment identity, intervening
discontinuities and successful tick spacing <=90 seconds. Failed samples are
durable unhealthy rows; skipped checks reset continuity. Watchdog independently
rejects stale (>120-second), unhealthy or old-boot state. No simulated duration
was credited to the real observer. `observation_72h` and `product_acceptance` were
false at handoff. Even 72 healthy capture hours alone cannot pass product acceptance.

## Rollback and remaining prerequisites

Operator commands and alerts are in [the runbook](../tools/discord_capture/README.md#alerts).
Prior bridge override is preserved privately at
`~/.local/share/cnet-discord-capture/rollback-20260908/40-private-capture.conf`;
release `84883b2` remains intact. To roll back the collector, disable/stop only the
two new capture timers, restore that exact override, daemon-reload and restart
only the bridge. Keep all capture/export/observation data. Do not use systemctl
revert or delete state. Rolling back the new cnetd umask would require moving only
its dedicated drop-in aside and restarting cnetd; retaining private permissions
is recommended. No rollback was needed or executed during this rollout.

Remaining evidence is substantive: genuine reviewed demand, task-specific
independent verification beyond the two-rule catalog, eligible continuous whole
windows, actual native development trajectories across the required families,
demonstrated headroom against strong controls, then bounded AMD fitting and fresh
post-selection confirmation under unchanged floors. Real 72-hour unattended
learning acceptance is also unfinished. Existing dual-AMD compute qualification
is not revoked; these are data/outcome prerequisites, not a GPU compute failure.
