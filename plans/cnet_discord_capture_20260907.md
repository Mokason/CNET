# Private Discord request chronology

## Decision and scope

The owner approved the existing Discord bridge as the first collection point,
then the chronological allocator sequence. A read-only Discord API check found
the application's individual owner is the sole recipient of the last-used DM
(channel type 1). Capture is restricted to that exact account and DM. Numeric
identifiers, requests, tokens and the dataset stay outside git. Other traffic
retains existing serving behavior but is not captured.

## Boundary and threat model

Only authenticated Discord MESSAGE_CREATE events enter the journal. Exact
account/channel matching is necessary but not sufficient: bots, webhooks,
forwarded messages, system events and guild messages are excluded. Text cannot
grant provenance, mapping, verification or training authority. Account origin
does not prove a human typed the request, its answer is correct, or demand is
representative. The local OS account and Discord/token/runtime are trusted;
the journal is not tamper-proof against that account or host compromise.

A private, bounded SQLite journal commits a selected request before forwarding.
Message IDs deduplicate across restarts. A crash after commit never re-executes
the request automatically; its completion remains unknown. This is at-most-once
attempt, not exactly-once delivery. Failure to durably capture selected traffic
fails loud and closes the shared gateway. Unrelated serving is unchanged during
normal operation, but fatal capture errors interrupt the whole bridge.
One committed message receives at most one peer-client invocation, with no
automatic execution/reply retry. Peer-client nonzero exit is a client error,
not proof that downstream execution failed; timeouts remain unknown. Successful
client exit and confirmed Discord reply delivery are separate observations and
neither measures answer correctness. Failure to persist completion leaves the
attempt unfinished/unknown.
Gateway connections are separate segments; startup, disconnect, sequence gaps,
backward wall-clock movement between selected requests and unfinished attempts
prevent a claim of complete history. No wall-clock continuity or missed-request
count is inferred from gateway sequence gaps. Elapsed time uses BOOTTIME.
No automatic history scraping/backfill or deletion of older data is authorized.

Only selected original text, normalized delivered text, IDs/timestamps, local
ordering, connection identity and completion status are retained. CNET answer
text is not retained or used as a label. Generic process logs contain bounded
metadata, not requests, usernames, answers, tokens or API response bodies.
Private directories/files require owner-only access, no symlinks/hardlinks;
storage and content bounds refuse further capture rather than silently truncate
(64 MiB database; rollback journal may consume approximately another 64 MiB).
Only originally observed text is captured: edits, deletions and attachment
contents are not collected. This is not a full or current conversation archive.
Absent/invalid capture policy refuses startup when capture is configured.

## Ordered implementation and evidence

- [x] RED contract tests: scope, exclusions, restart dedup, interrupted request,
  private storage, size limits, failure propagation, metadata-only readiness.
- [x] Durable journal and integration with the existing bridge; test success,
  failure and unknown outcomes without treating answers as correctness labels.
- [ ] Fresh adversarial review and focused regression, then versioned bridge-only
  rollout with preserved prior script/service and an explicit rollback command.
- [ ] Verify live identity, READY, private journal and unchanged core services;
  no fabricated Discord messages or synthetic demand in the live store.
- [ ] Inspect real demand readiness. Freeze explicit capability mappings and
  independent verifier receipts before scoring; exclude unknown mappings.
- [ ] Whole chronological development episodes; identical budgets, initial
  state and action spaces for strong deterministic controls. Require useful
  headroom before bounded AMD fitting. New features/objectives get new versions.
- [ ] Fresh whole-episode confirmation only after development freeze. Preserve
  +0.05 gain, positive lower confidence bound and no negative family mean.

The learned allocator remains inactive. Certification floors, existing runtime
pins and learning ledgers are not changed. Real elapsed demand and independent
labels are evidence prerequisites, not boxes that fixtures can complete.

## Operational questions

How many selected unique requests were captured? Which attempts lack completion?
Where are continuity gaps? Are independently mapped/verified episodes ready?
Readiness reports expose counts/reason codes only and cannot authorize training.

## Sources

Discord's [application object](https://docs.discord.com/developers/resources/application)
and [channel object](https://docs.discord.com/developers/resources/channel) define
owner, recipients and DM type; [message fields](https://docs.discord.com/developers/resources/message)
distinguish author, webhook and system events. Checked 2026-09-07.

## Source verification before rollout

Initial journal RED: missing journal import. Initial gateway RED: absent bridge
class in the new module. Four additional RED regressions reproduced missing
policy/schema reuse, 64-KiB SQLite pages and clock-event quota bypass. A redirect
RED exposed the high-level WebSocket client's default redirect behavior; the
adapter now uses its low-level API with zero redirects and explicit HTTP 101.
Native control/framing/truncation rejection was separately RED before repair.

33 capture tests, 15 legacy-audit tests, 13 sequence-pilot tests and all four
allocator CPU executables pass. Astra design/code review findings were addressed;
the clock claim was narrowed to what is actually measured. No cross-model
provider used, per owner choice. A local 500-request synthetic SQLite benchmark
measured median 1.524 ms and p99 1.938 ms for begin+finish together, 780.697 ms
total, 147,456-byte DB and 14,916-KiB process RSS high-water. No Discord or native
serving latency is included, and none of these fixtures is real demand.

During RED development, two unauthenticated public Gateway probe connections
occurred unintentionally (reviewer compatibility probe and incorrect mock
boundary); one sent only an invalid fixture token. Neither used the real token,
sent a user message or entered the live dataset. Both ended, and fixture tests
now explicitly prohibit socket connections. Token-bearing live checks were
read-only scope metadata requests. Live collection begins only on deployment.

The existing native client maps literal PING/STATUS/QUIT to control operations,
uses line framing and an 8192-byte buffer. The versioned bridge rejects those
controls, leading CLI flags, embedded CR/LF/NUL and >8000-byte queries before
invocation. This intentional safety difference applies to all bridge traffic.
Its existing response formatter is not a correctness verifier. The installed
Python/WebSocket/native runtime remains trusted; this is a scoped review, not
a whole-host dependency or vulnerability clearance.
