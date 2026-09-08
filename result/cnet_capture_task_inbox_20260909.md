# Captured task inbox — September 9, 2026

Continuation of `82ce283`, implemented through `4705a65` on
`feature/verified-task-core-20260908` in the existing isolated worktree. The
primary checkout, live gateway/learner, capture data, frozen soak and original
budgets were not modified. No push, deployment, migration, real capture replay,
origin review or GPU fitting was performed.

## Delivered source behavior

- `learning gaps ROOT LIMIT`: one bounded experience snapshot, separate
  dataset/key/original-source/origin groups, review/state counts and explicit
  unmeasured acquisition cost. Frequency is not learned scheduling value.
- Pinned bridge schema 2: fresh capture admission precedes one stable-ID native
  task dispatch. Every parser refusal is terminal; no peer or legacy lookup
  fallback. Numeric answers require native certification and independent
  source agreement. Observations do not create demand or approve corrections.
- `learning trace ROOT IDS` and `task_inbox.py`: exact bounded native linkage
  and capture-side pagination, retaining missing observations without invented
  causes. Reads do not advance heartbeats or mutate ledger/capture bytes.
- Private-safe structured task events and explicit compatibility/runbooks.
  Schema-1 bridge semantics and capture SQL are unchanged; native schema 3 is
  still fresh-install-only. No second capsule package or training format exists.

The [operator contract](../docs/CAPTURE_TASK_INBOX.md) and
[decision](../plans/cnet_capture_task_inbox_20260909.md) document commands,
scope, source/version handling and remaining product work.

## Actual-native rehearsal

`LearningCapturedTaskTests` publishes the current managed code and pins the
six native artifacts in a new private home-directory fixture. `/tmp` ancestors
remain deliberately refused by the production bridge. Python's standard
library drives the real bridge, capture journal and inspector; actual Discord
and WebSocket delivery are not exercised. Gateway routing/deduplication is
tested separately with network mocks. Teacher/self-answer paths are disabled.

A real private owner runs once under an immutable **30-second budget**. The
test does not synthesize owner heartbeats, renew the budget or lower any gate.
The pinned Unicode excerpt and all four approved tables are present, so these
are certified-capability misses, not a demonstration of discovering a new source.

| Step | Measured outcome |
| --- | --- |
| Captured µ-uppercase and A-lowercase requests | Two `miss` observations; zero jobs and zero demand for both inputs |
| Bare numeric ambiguity and unrelated weather question | Clarification/abstention; captures retained with no native observations |
| Explicit fixture-owner approvals of two external source hashes | Two native jobs; ordinary capsule acquisition, activation and probation settle |
| New phrasing: “Please convert 'µ' to uppercase.” | Native verified 924 / U+039C |
| New phrasing: “What is the lowercase of A?” | Native verified 97 / U+0061; coexistence preserved |
| “uppercase ß” | Native abstention; no reference/default answer substitution |
| Complete native sweeps | Both 256-key domains pass: 114 correct answers and 398 correct abstentions |
| Inspection after original owner budget completes | Seven captures, five native observations, two capture-only rows; both databases byte-identical before/after read |

All seven requests are **synthetic fixtures**, not genuine usage. The production
captured route records `unreviewed`; this is not human attestation. The test's
separate receipts explicitly identify synthetic provenance. No fixture enters
the real capture/export or supplies training evidence. The linked report keeps
`training_eligible=false` and `origin_attested=false` throughout.

The owner-budget test takes approximately 30 seconds because it waits for real
budget completion. This is not an inference-latency, throughput or learned-gain
benchmark. No new independently frozen paraphrase population was evaluated.

## Final verification

| Check | Result |
| --- | --- |
| Full managed control-plane regression | 961 passed, zero failed/skipped; 1 minute 25 seconds |
| Full Python capture/bridge regression, private native evidence fixture | 148 passed, zero failed/skipped |
| New native report/trace source-line coverage | 52/52: gaps 34/34, trace 18/18 |
| New Python source-line coverage | Identity 13/13; task protocol 76/76; inbox 93/94 |
| Modified bridge whole-file line coverage | 262/263 |
| Capture/private filesystem checks | Read-only bytes, no WAL sidecar creation, invalid schema/scope/refusal, bounded pages and sanitized CLI errors tested |
| Dispatch/protocol checks | Duplicate/crash no-replay, every refusal terminal, independent labels, missing result, unknown outcome, malformed/source/conflict latch and failed pause tested |
| Production NuGet direct/transitive advisory check | No vulnerable packages reported by the configured official NuGet source; no production dependency added |
| Graft freshness and documentation | Source graph in sync; changed Markdown local links and diff whitespace checks pass |

Python coverage used the standard-library tracer with `--count --missing
--summary`; without `--missing`, its summary counts only executed lines and
must not be presented as coverage. The earlier exploratory run without that
flag is not the accepted measurement. `task_inbox.py`'s executable-file wrapper
is the one unmeasured line; its `main()` success/refusal paths are tested. The
remaining bridge line is an older schema-1 OOD rendering branch. These numbers
are executable **line** coverage, not branch completeness, mutation testing or
whole-system security assurance. Native CLI wrappers run in child processes;
their integration results are distinct from in-process source-line coverage.
Existing unrelated CA1416 platform warnings remain.

Fresh-context Astra reviews produced concrete corrections: all parser refusal
statuses consume schema-2 routing; only fresh capture admission can authorize
dispatch; capture-first pages preserve missing observations; ten-ID tracing
fits the existing pipe cap; replayed conflicts still latch; unsupported WAL
capture format refuses before a reader could create sidecars. These issues
received tests and fixes. No other model provider was called.

No new production dependency or remote authority was introduced. Scoped
regressions/advisory checks are not an absence-of-vulnerability claim. The
trusted OS/framework, hostile same-UID races, source truth beyond the pinned
finite tables, real Discord availability and broader learned-policy quality
remain outside this result.

## ECC and Graft trial

ECC's installed TDD and verification-loop instructions guided the execution:
real failing tests and committed RED checkpoints preceded production behavior;
focused GREEN checks preceded the full regressions and measured coverage. Its
package-manager detector reported npm by default, with no repository package
manifest; the actual runners remained C/.NET/Python. No broad home-level ECC
configuration install or unrelated npm test suite was substituted.

Feature RED commits: `edd8fd2` (gaps), `58e714b` (explicit mode/dispatch),
`40a5cc9` (task envelope), `392d305` (exact trace), `d410f6f` (capture inbox).
Review/diagnostic REDs: `284f16c` (replayed conflict), `df18772` (WAL reader),
`ae39546` (correlated task logs). One later test-edit placement error produced a
NameError; it was diagnosed, corrected and followed by the final full pass,
not excluded. No certification or evidence floor was lowered.

Graft supplied code spans and relationships after the MCP code graph reported
this worktree unindexed. It remains a local source-navigation aid, not evidence
that CNET learned or became more capable. Graft 0.16.0 rebuilt the local source
graph with 1,994 indexed files, 24,076 nodes, 24,235 edges and 1,966 map cards;
16 files were parsed and 1,978 unchanged files replayed from its cache. No
deep/model pass or external provider was requested. Estimated token savings
are not included as a measured result. `graft check` reported the wiring graph
in sync; its optional meaning/deep tier remains unbuilt, not silently treated
as a completed semantic analysis.

## Reproduction and local evidence

From the implementation worktree, with the required native binaries built:

```sh
dotnet test dotnet/CnetControlPlane.Tests/CnetControlPlane.Tests.csproj \
  --artifacts-path .artifacts/task-verified --no-restore --nologo \
  --collect 'Code Coverage;Format=cobertura' \
  --logger 'trx;LogFileName=capture-final.trx' \
  --results-directory .artifacts/task-verified/capture-final

CNET_EVIDENCE_RUNTIME=/absolute/private/native-fixture \
  "$DISCORD_PYTHON" -m trace --count --missing --summary \
  --coverdir .artifacts/task-verified/capture-python-coverage \
  --module unittest discover -s tools/discord_capture -p 'test_*.py' -v
```

The evidence fixture came from the previously audited `native_check.sh` and
contains both compiled mapper artifacts. It is not a live daemon. The managed
captured-task rehearsal defaults to `/usr/bin/python3`; `CNET_TASK_PYTHON` may
select another absolute Python host. Python gateway tests need the existing
websocket-client environment; no runtime package installation occurs.

Retained local output:

- `/tmp/cnet-capture-final-managed-20260909.log`
- `/tmp/cnet-capture-final-python-20260909.log`
- `.artifacts/task-verified/capture-final/capture-final.trx`
- `.artifacts/task-verified/capture-final/386b01ad-13c5-4106-a92c-f8b5faa04e5e/marble_marble-system_2026-09-09.00_34_16.cobertura.xml`
- `.artifacts/task-verified/capture-python-coverage/`
- `/tmp/cnet-capture-advisories-20260909.log`
- `/tmp/cnet-capture-graft-build-20260909.log`
- `/tmp/cnet-capture-graft-check-20260909.log`

These local artifacts are not committed; this report records the measured
scope. Disposable rehearsal installations were removed by their fixtures;
no user capture, live installation or frozen-soak artifact was removed.

## Remaining sequence

Next source work: useful reusable compositions in the existing capsule format
and an independently frozen paraphrase/OOD evaluation. Then dependency-aware
freshness and targeted AMD task-policy experiments, contingent on independently
verified chronological experience and measurable headroom. Proposed-source UX,
measured acquisition costs, real origin review and controlled rollout remain
open. A read-only frequency queue is not autonomous choice of useful learning.

The original real-duration and allocator gates stay WITHHELD until their own
evidence exists: adequate genuine whole windows, eligible native trajectories
for all four campaign families, >=.05 useful headroom, >=.05 measured gain,
positive paired 95% lower bound and nonnegative family means. No synthetic
capture, line-coverage percentage, capsule count or GPU utilization passes them.
