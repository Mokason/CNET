# Identity/presence and Discord repair — 2026-09-08

## Outcome

The repaired live-baseline daemon and Discord adapter are deployed in a separate
private release. Identity/presence no longer steals the tested unrelated requests.
Operational learning/skill-use/progress questions have explicit non-certified
responses. Discord displays answers, refusals and qualification instead of hiding
them behind a fixed personality reply.

Source branch: `fix/identity-presence-discord-20260908`, based on `8bb8d5b`.
No remote push or merge was requested. Prior MCP read work remains in this source
branch but was not enabled in production by this live-baseline backport.

## Evidence and repair

Scoped read-only chronology found six owner requests, including skill usage,
learning and improvement questions. Native reproduction independently showed:

| Request | Before | After |
|---|---|---|
| What are you able to calculate? | `soul_who`: I am Marble | `ACTION/skill_usage_v1`: invocation syntax and coverage limits |
| Can you use those skills? | Still here… | Same explicit skill-use help |
| Can you learn? | Still here… | Independent-evidence/admission requirements; no claim of a running learner |
| Any improvements? | Still here… | Current inventory and explicit inability to confirm improvement without evaluation |
| Who are you? | Identity | Identity preserved |
| Are you still here? | Legacy certified acknowledgment | Operational presence response, not a knowledge certificate |

The causes were a `you` plus model-word heuristic, substring identity aliases,
case-sensitive social guards against case-insensitive catalogs, normalization
truncation and a Discord keyword blacklist/fixed capability replacement.

Whole-request matching now uses bounded courtesy wrappers. Contraction expansion
preserves the suffix before whitespace collapse. The legacy 512-byte preparation
limit refuses overflow rather than answering a clipped prefix. Social catalog
patterns and known IDs are normalized before matching.

Discord uses native JSON with preserved request-local peer context and escaped
stage/peer reply fields. The native client refuses oversized requests and
incomplete/oversized JSON replies. stderr is not response data. The renderer
rejects duplicate fields, wrong types, unknown sources and contradictory authority.
Non-certified informational answers and allowed stage drafts have visible labels.
No certification floor or learning admission rule was changed.

## Verification

RED witnesses preceded fixes, including all nine unrelated alias inputs, native
social routing, Discord replacement failures, contraction/catalog-case bypasses,
JSON peer loss, request/reply truncation and contradictory source verification.

| Gate | Measured result |
|---|---|
| `make social_reply_verify DISCORD_PYTHON=/home/marble/.config/cnet/venv-discord/bin/python` | Alias/query/dialog gates; 9 negative identity cases; 22 native protocol checks; 5 native peer tests; 10 private daemon tests including actual peer-to-Discord renderer; 80 capture/presentation tests, 79 pass and 1 existing skip |
| `make cnetd_mcp_read` | 3 isolated MCP boundary tests pass |
| `make cnetd_brick_capacity` | Private 25-table load and PING pass |
| `make knowledge_capsule` | 94 checks pass |
| `make coverage_abstain` | 55 checks pass; existing held-out 4/4 |
| ASan + UBSan, leak detection | Native alias and request-parser fixtures pass without findings |
| Live-compatible staged binaries | All 10 daemon tests, 5 peer tests and private capacity test pass |
| Live native client plus deployed Discord renderer | 10 smoke requests complete; observed 0.42–2.76 ms including client startup, not a throughput benchmark |

Three fresh-context Astra review passes identified actionable framing, truncation,
case and authority issues. All were reconciled with tests. Multiline Discord
requests remain intentionally rejected; multiline replies are supported. No
external model CLI was used.

Live certified capsule checks: 173 bytes → 1384 bits, 181 → 1448; 192 refuses as
outside coverage and records a demand through the existing queue. No learner was
explicitly started and no capsule was installed by this repair.

## Deployment and recovery

Private release and prior source snapshots:
`/home/marble/.local/share/cnet-social-repair-20260908-vaipvL/{release,before}`.
Only six runtime source/header files were backported into the older dirty primary
checkout: query alias header/source, daemon protocol header/source, daemon and
native peer. Other user changes are preserved.

Dedicated `60-social-reply.conf` user-service drop-ins select the new daemon,
bridge/native peer and matching monitor/watchdog release. Existing token files,
capture scope, factory override, capsule configuration and model services are
retained. Daemon PID 2442779 and bridge PID 2442782 were active with zero automatic
restarts immediately after rollout; the bridge logged `READY capture=True`.

```text
cnetd                  a054e3b795d594106099307a6a6a601dd1cc7a8e5b0afcf20e2e8e5fe0aae465
cnet_peer              d217e3ccc350666f11079ae187f699d58e39bd857a723f584e5e79b33accf1c9
libcnet_capsule_core.so 529e139956a72d2795a3dc4f39d63f4aa6e44946f57e40c931ba6796aed7982e
```

All 24 LUT files and 15 capsule files remained byte-for-byte unchanged. Aggregate
sorted relative-path/hash inventory digests before and after:

```text
LUTs     dc4d0333621001a8912c265735814f3d1c1dfb3b53b1196a7a5f8303f8de24e5
capsules e378603b92d9a5f92ff749a847448c91cb8c7723f807a1aaa3b8a78657eacf92
```

Rollback: move only the four new `60-social-reply.conf` drop-ins to the private
backup, run `systemctl --user daemon-reload`, and restart cnetd and the bridge.
Monitor/watchdog timers then select the previous frozen release. Preserve their
history; a restart/release change resets continuity honestly. Restore source
snapshots only after checking for subsequent edits; never reset the worktree.

## Limits

No synthetic Discord message was posted or inserted into the owner-demand store;
its six requests were unchanged. Live checks used the native client and the exact
deployed rendering function, not proof of a fresh Discord reply delivery.

The long-duration observation/product gates are not passed by this rollout.
The monitor initially recorded the expected release/connection discontinuity and
missing first heartbeat; no history or counter was reset to hide it.
Subsequent real heartbeat/monitor observations recovered with an empty alert list;
the 72-hour observation and product-acceptance flags remained false.

This is not new domain knowledge or proof of autonomous improvement. Existing
unverified memory retrieval is unchanged: a model-definition query now reaches
legacy notes rather than identity, but those notes can include archived teacher
drafts and be off-topic. Their accuracy/relevance is not certified by this repair.
Arbitrary compound-query handling, legacy non-JSON CLI framing, the full product
benchmark suite and a repository-wide vulnerability audit remain outside scope.
